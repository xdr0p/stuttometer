#include "benchmark_view.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <cmath>

namespace stuttometer::gui {

static std::atomic<bool> s_benchmark_dialog_open{false};

constexpr int IDC_BENCH_COPY_SUMMARY  = 3001;
constexpr int IDC_BENCH_EXPORT_JSON   = 3002;
constexpr int IDC_BENCH_RESET_SESSION = 3003;
constexpr int IDC_BENCH_CLOSE         = 3004;
constexpr UINT_PTR IDT_BENCH_REFRESH  = 4001;

struct BenchmarkViewState {
    std::shared_ptr<SessionBenchmark> benchmark;
    bool redact{false};
    HWND h_btn_copy{nullptr};
    HWND h_btn_export{nullptr};
    HWND h_btn_reset{nullptr};
    HWND h_btn_close{nullptr};
    HFONT font_title{nullptr};
    HFONT font_metric_val{nullptr};
    HFONT font_regular{nullptr};
    HFONT font_bold{nullptr};
    HFONT font_small{nullptr};
    int dpi{96};
    std::wstring copy_btn_text{L"Copy Summary"};
    std::wstring export_btn_text{L"Export JSON"};
    // cached_summary is UI-thread-only state. All mutations occur in WM_CREATE,
    // WM_TIMER, WM_COMMAND, and (one-shot) WM_PAINT. No synchronization required.
    BenchmarkSummary cached_summary{};
    bool summary_valid{false};
};

static std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), needed);
    return result;
}

static void refresh_cached_summary(BenchmarkViewState* state) {
    if (!state || !state->benchmark) return;
    state->cached_summary = state->benchmark->get_summary(state->redact);
    state->summary_valid = true;
}

static void apply_window_dark_titlebar(HWND hwnd) {
    if (!hwnd) return;
    BOOL use_dark_mode = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &use_dark_mode, sizeof(use_dark_mode)))) {
        DwmSetWindowAttribute(hwnd, 19 /*DWMWA_USE_IMMERSIVE_DARK_MODE_OLD*/, &use_dark_mode, sizeof(use_dark_mode));
    }
    COLORREF caption_color = RGB(13, 17, 23);
    COLORREF text_color    = RGB(226, 232, 240);
    COLORREF border_color  = RGB(36, 43, 61);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption_color, sizeof(caption_color));
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text_color, sizeof(text_color));
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border_color, sizeof(border_color));
    DWORD corner_pref = 2 /*DWMWCP_ROUND*/;
    (void)DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner_pref, sizeof(corner_pref));
}

static void update_fonts(BenchmarkViewState* state, int dpi) {
    state->dpi = dpi;
    if (state->font_title) DeleteObject(state->font_title);
    if (state->font_metric_val) DeleteObject(state->font_metric_val);
    if (state->font_regular) DeleteObject(state->font_regular);
    if (state->font_bold) DeleteObject(state->font_bold);
    if (state->font_small) DeleteObject(state->font_small);

    auto make_font = [dpi](int pt, int weight) {
        int height = -MulDiv(pt, dpi, 72);
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };

    state->font_title = make_font(13, FW_BOLD);
    state->font_metric_val = make_font(16, FW_BOLD);
    state->font_regular = make_font(9, FW_NORMAL);
    state->font_bold = make_font(9, FW_BOLD);
    state->font_small = make_font(8, FW_NORMAL);
}

static void draw_rounded_card(HDC hdc, const RECT& rc, COLORREF bg_color, COLORREF border_color, int radius) {
    HBRUSH br = CreateSolidBrush(bg_color);
    HPEN pen = CreatePen(PS_SOLID, 1, border_color);
    HGDIOBJ old_br = SelectObject(hdc, br);
    HGDIOBJ old_pen = SelectObject(hdc, pen);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_br);
    DeleteObject(pen);
    DeleteObject(br);
}

struct BenchmarkInitParams {
    std::shared_ptr<SessionBenchmark> benchmark;
    bool redact{false};
};

static LRESULT CALLBACK BenchmarkButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemovePropW(hwnd, L"Hovered");
            RemoveWindowSubclass(hwnd, BenchmarkButtonSubclassProc, uIdSubclass);
            break;
        case WM_MOUSEMOVE: {
            bool is_hovered = GetPropW(hwnd, L"Hovered") != nullptr;
            if (!is_hovered) {
                SetPropW(hwnd, L"Hovered", (HANDLE)1);
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            RemovePropW(hwnd, L"Hovered");
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK BenchmarkWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<BenchmarkViewState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* params = reinterpret_cast<BenchmarkInitParams*>(cs->lpCreateParams);
            state = new BenchmarkViewState();
            if (params) {
                state->benchmark = params->benchmark;
                state->redact = params->redact;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            UINT dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;
            update_fonts(state, dpi);

            HINSTANCE hInst = cs->hInstance;
            state->h_btn_copy = CreateWindowExW(0, L"BUTTON", L"Copy Summary", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BENCH_COPY_SUMMARY, hInst, NULL);
            state->h_btn_export = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BENCH_EXPORT_JSON, hInst, NULL);
            state->h_btn_reset = CreateWindowExW(0, L"BUTTON", L"Reset Session", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BENCH_RESET_SESSION, hInst, NULL);
            state->h_btn_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BENCH_CLOSE, hInst, NULL);

            SetWindowSubclass(state->h_btn_copy, BenchmarkButtonSubclassProc, IDC_BENCH_COPY_SUMMARY, 0);
            SetWindowSubclass(state->h_btn_export, BenchmarkButtonSubclassProc, IDC_BENCH_EXPORT_JSON, 0);
            SetWindowSubclass(state->h_btn_reset, BenchmarkButtonSubclassProc, IDC_BENCH_RESET_SESSION, 0);
            SetWindowSubclass(state->h_btn_close, BenchmarkButtonSubclassProc, IDC_BENCH_CLOSE, 0);

            SetTimer(hwnd, IDT_BENCH_REFRESH, 1000, NULL);
            refresh_cached_summary(state);
            return 0;
        }

        case WM_SIZE: {
            if (!state) return 0;
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            int dpi = state->dpi;
            auto scale = [dpi](int val) { return MulDiv(val, dpi, 96); };

            int margin = scale(16);
            int btn_h = scale(34); // Standard 34px dialog action button height matching Settings dialog
            int btn_y = height - margin - btn_h;

            int btn_copy_w = scale(130);
            int btn_export_w = scale(120);
            int btn_reset_w = scale(125);
            int btn_close_w = scale(95);
            int gap = scale(10);

            int bx = margin;
            MoveWindow(state->h_btn_copy, bx, btn_y, btn_copy_w, btn_h, TRUE);
            bx += btn_copy_w + gap;
            MoveWindow(state->h_btn_export, bx, btn_y, btn_export_w, btn_h, TRUE);
            bx += btn_export_w + gap;
            MoveWindow(state->h_btn_reset, bx, btn_y, btn_reset_w, btn_h, TRUE);

            int close_x = width - margin - btn_close_w;
            MoveWindow(state->h_btn_close, close_x, btn_y, btn_close_w, btn_h, TRUE);
            return 0;
        }

        case WM_DPICHANGED: {
            if (!state) return 0;
            UINT new_dpi = HIWORD(wParam);
            update_fonts(state, new_dpi);
            RECT* prc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, NULL, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == IDT_BENCH_REFRESH) {
                refresh_cached_summary(state);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_CTLCOLORBTN: {
            static HBRUSH s_btn_bg = CreateSolidBrush(RGB(17, 19, 23));
            SetBkColor((HDC)wParam, RGB(17, 19, 23));
            return (LRESULT)s_btn_bg;
        }

        case WM_DRAWITEM: {
            auto* pDIS = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (!pDIS || !state) return FALSE;

            HDC hdc = pDIS->hDC;
            RECT rc = pDIS->rcItem;
            bool is_pressed = (pDIS->itemState & ODS_SELECTED);
            bool is_disabled = (pDIS->itemState & ODS_DISABLED);
            bool is_hovered = (GetPropW(pDIS->hwndItem, L"Hovered") != nullptr) && !is_disabled;

            // Pre-fill bounding rectangle with dialog background to eliminate light corner artifacts
            HBRUSH bg_parent = CreateSolidBrush(RGB(17, 19, 23));
            FillRect(hdc, &rc, bg_parent);
            DeleteObject(bg_parent);

            // Match main_gui.cpp BtnStyle palettes
            COLORREF bg = is_pressed ? RGB(22, 26, 36) : (is_hovered ? RGB(38, 45, 62) : RGB(28, 33, 46));
            COLORREF border = is_pressed ? RGB(44, 52, 72) : (is_hovered ? RGB(65, 78, 105) : RGB(50, 60, 82));
            COLORREF text_color = is_disabled ? RGB(100, 116, 139) : (is_hovered ? RGB(241, 245, 249) : RGB(226, 232, 240));

            if (pDIS->CtlID == IDC_BENCH_RESET_SESSION) {
                // DangerRed palette matching main_gui.cpp
                bg = is_pressed ? RGB(35, 18, 18) : (is_hovered ? RGB(70, 30, 30) : RGB(45, 25, 25));
                border = is_pressed ? RGB(80, 35, 35) : (is_hovered ? RGB(120, 50, 50) : RGB(90, 40, 40));
                text_color = is_hovered ? RGB(254, 202, 202) : RGB(248, 113, 113);
            }

            int dpi = state->dpi;
            int radius_px = MulDiv(12, dpi, 96); // True 6px radius via 12px diameter, matching main_gui.cpp
            draw_rounded_card(hdc, rc, bg, border, radius_px);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, text_color);
            HGDIOBJ old_font = SelectObject(hdc, state->font_bold);

            std::wstring text;
            if (pDIS->CtlID == IDC_BENCH_COPY_SUMMARY) text = state->copy_btn_text;
            else if (pDIS->CtlID == IDC_BENCH_EXPORT_JSON) text = state->export_btn_text;
            else if (pDIS->CtlID == IDC_BENCH_RESET_SESSION) text = L"Reset Session";
            else if (pDIS->CtlID == IDC_BENCH_CLOSE) text = L"Close";

            // Tactile 1px vertical offset when pressed, matching main_gui.cpp
            RECT text_rc = rc;
            if (is_pressed) {
                text_rc.top += 1;
                text_rc.bottom += 1;
            }

            DrawTextW(hdc, text.c_str(), -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old_font);
            return TRUE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!state || !state->benchmark) break;

            if (id == IDC_BENCH_CLOSE) {
                DestroyWindow(hwnd);
            } else if (id == IDC_BENCH_RESET_SESSION) {
                state->copy_btn_text = L"Copy Summary";
                state->export_btn_text = L"Export JSON";
                state->benchmark->reset();
                refresh_cached_summary(state);
                InvalidateRect(hwnd, NULL, TRUE);
                InvalidateRect(state->h_btn_copy, NULL, TRUE);
                InvalidateRect(state->h_btn_export, NULL, TRUE);
            } else if (id == IDC_BENCH_COPY_SUMMARY) {
                auto summary = state->benchmark->get_summary(state->redact);
                std::string md = summary.to_markdown();
                int wlen = MultiByteToWideChar(CP_UTF8, 0, md.c_str(), -1, nullptr, 0);
                bool ok = false;
                if (wlen > 0 && OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
                    if (hGlob) {
                        wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hGlob));
                        if (pMem) {
                            MultiByteToWideChar(CP_UTF8, 0, md.c_str(), -1, pMem, wlen);
                            GlobalUnlock(hGlob);
                            if (SetClipboardData(CF_UNICODETEXT, hGlob)) {
                                ok = true;
                            }
                        }
                        if (!ok) GlobalFree(hGlob);
                    }
                    CloseClipboard();
                }
                state->copy_btn_text = ok ? L"Copied \u2713" : L"Failed \u2715";
                InvalidateRect(state->h_btn_copy, NULL, TRUE);
            } else if (id == IDC_BENCH_EXPORT_JSON) {
                auto summary = state->benchmark->get_summary(state->redact);
                wchar_t filename_buf[MAX_PATH] = L"stutto_benchmark_summary.json";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = filename_buf;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrDefExt = L"json";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    std::ofstream ofs(filename_buf);
                    bool ok = false;
                    if (ofs.is_open()) {
                        ofs << summary.to_json();
                        ok = true;
                    }
                    state->export_btn_text = ok ? L"Exported \u2713" : L"Failed \u2715";
                    InvalidateRect(state->h_btn_export, NULL, TRUE);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!state || !state->benchmark) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            int width = client_rc.right - client_rc.left;
            int height = client_rc.bottom - client_rc.top;

            // Double buffering
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP mem_bm = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ old_bm = SelectObject(mem_dc, mem_bm);

            // Fill background
            HBRUSH bg_br = CreateSolidBrush(RGB(17, 19, 23));
            FillRect(mem_dc, &client_rc, bg_br);
            DeleteObject(bg_br);

            int dpi = state->dpi;
            auto scale = [dpi](int val) { return MulDiv(val, dpi, 96); };
            int margin = scale(16);

            if (!state->summary_valid) {
                refresh_cached_summary(state);
            }
            const auto& summary = state->cached_summary;

            // 1. Top Banner Card (Y: margin, H: scale(72))
            int banner_y = margin;
            int banner_h = scale(72);
            RECT banner_rc = { margin, banner_y, width - margin, banner_y + banner_h };
            draw_rounded_card(mem_dc, banner_rc, RGB(24, 28, 38), RGB(42, 50, 68), 8);

            SetBkMode(mem_dc, TRANSPARENT);

            // Target Process & PID
            std::wstring w_proc = L"All Processes (Monitor All)";
            if (!summary.target_process.empty()) {
                w_proc = to_wide(summary.target_process);
                if (summary.target_pid != 0) {
                    w_proc += L" (PID: " + std::to_wstring(summary.target_pid) + L")";
                }
            } else if (summary.target_pid != 0) {
                w_proc = L"PID: " + std::to_wstring(summary.target_pid);
            }

            SelectObject(mem_dc, state->font_title);
            SetTextColor(mem_dc, RGB(241, 245, 249));
            RECT title_rc = { banner_rc.left + scale(16), banner_rc.top + scale(12), banner_rc.right - scale(200), banner_rc.top + scale(36) };
            DrawTextW(mem_dc, w_proc.c_str(), -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Duration (MM:SS), Frames, Stutters Detected
            const unsigned total_sec = static_cast<unsigned>(summary.duration_ms / 1000.0);
            const unsigned mins = total_sec / 60;
            const unsigned secs = total_sec % 60;
            wchar_t sub_buf[256];
            swprintf_s(sub_buf, L"Duration: %02u:%02u  |  Total Frames: %llu  |  Stutters Detected: %llu",
                       mins, secs, summary.total_frames, summary.stutters_detected);

            SelectObject(mem_dc, state->font_regular);
            SetTextColor(mem_dc, RGB(148, 163, 184));
            RECT sub_rc = { banner_rc.left + scale(16), banner_rc.top + scale(38), banner_rc.right - scale(16), banner_rc.top + scale(60) };
            DrawTextW(mem_dc, sub_buf, -1, &sub_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 2. Metrics Grid (5 Cards side-by-side)
            int grid_y = banner_y + banner_h + scale(12);
            int grid_h = scale(76);
            int num_cards = 5;
            int card_gap = scale(8);
            int total_card_w = (width - 2 * margin - (num_cards - 1) * card_gap);
            int card_w = total_card_w / num_cards;

            struct MetricCardData {
                std::wstring label;
                std::wstring value;
                COLORREF val_color;
            };

            std::wstring avg_fps_str = (summary.frametimes.avg_fps > 0.0)
                ? std::to_wstring(static_cast<int>(std::round(summary.frametimes.avg_fps))) + L" FPS"
                : L"N/A";
            std::wstring low_1_str = (summary.frametimes.low_1pct_fps > 0.0)
                ? std::to_wstring(static_cast<int>(std::round(summary.frametimes.low_1pct_fps))) + L" FPS"
                : L"N/A";
            std::wstring low_01_str = (summary.frametimes.low_01pct_fps > 0.0)
                ? std::to_wstring(static_cast<int>(std::round(summary.frametimes.low_01pct_fps))) + L" FPS"
                : L"N/A";

            wchar_t stall_buf[64];
            swprintf_s(stall_buf, L"%.1f ms", summary.net_stall_ms);

            wchar_t worst_buf[64];
            if (summary.worst_stutter_ms > 0.0) {
                swprintf_s(worst_buf, L"%.1f ms", summary.worst_stutter_ms);
            } else {
                wcscpy_s(worst_buf, L"None");
            }

            COLORREF avg_color = (summary.frametimes.avg_fps > 0.0) ? RGB(56, 189, 248) : RGB(148, 163, 184);
            COLORREF low_1_color = (summary.frametimes.low_1pct_fps > 0.0) ? RGB(16, 185, 129) : RGB(148, 163, 184);
            COLORREF low_01_color = (summary.frametimes.low_01pct_fps <= 0.0)
                ? RGB(148, 163, 184)
                : ((summary.frametimes.low_01pct_fps < 30.0) ? RGB(239, 68, 68) : RGB(245, 158, 11));

            MetricCardData cards[5] = {
                { L"Average FPS", avg_fps_str, avg_color },
                { L"1% Low FPS", low_1_str, low_1_color },
                { L"0.1% Low FPS", low_01_str, low_01_color },
                { L"Net Stall Time", stall_buf, (summary.net_stall_ms > 100.0) ? RGB(239, 68, 68) : RGB(203, 213, 225) },
                { L"Worst Stutter", worst_buf, (summary.worst_stutter_ms > 50.0) ? RGB(239, 68, 68) : RGB(203, 213, 225) }
            };

            for (int i = 0; i < num_cards; ++i) {
                int cx = margin + i * (card_w + card_gap);
                RECT c_rc = { cx, grid_y, cx + card_w, grid_y + grid_h };
                draw_rounded_card(mem_dc, c_rc, RGB(24, 28, 38), RGB(40, 48, 66), 6);

                SelectObject(mem_dc, state->font_small);
                SetTextColor(mem_dc, RGB(148, 163, 184));
                RECT lbl_rc = { cx + scale(8), grid_y + scale(8), cx + card_w - scale(8), grid_y + scale(26) };
                DrawTextW(mem_dc, cards[i].label.c_str(), -1, &lbl_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(mem_dc, state->font_metric_val);
                SetTextColor(mem_dc, cards[i].val_color);
                RECT val_rc = { cx + scale(8), grid_y + scale(28), cx + card_w - scale(8), grid_y + grid_h - scale(8) };
                DrawTextW(mem_dc, cards[i].value.c_str(), -1, &val_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // 3. Culprit Attribution Card (Top 5 + Other)
            int table_y = grid_y + grid_h + scale(12);
            int btn_h = scale(34); // Standard 34px dialog action button height matching Settings dialog
            int table_h = height - margin - btn_h - scale(12) - table_y;
            RECT table_rc = { margin, table_y, width - margin, table_y + table_h };
            draw_rounded_card(mem_dc, table_rc, RGB(22, 26, 34), RGB(40, 48, 66), 8);

            // Table Title
            SelectObject(mem_dc, state->font_bold);
            SetTextColor(mem_dc, RGB(241, 245, 249));
            RECT tbl_title_rc = { table_rc.left + scale(16), table_rc.top + scale(12), table_rc.right - scale(16), table_rc.top + scale(32) };
            DrawTextW(mem_dc, L"Root-Cause Culprit Attribution (Top 5 + Other)", -1, &tbl_title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Column Headers & Table Geometry
            int col_hdr_y = table_rc.top + scale(38);
            int pad = scale(10);

            // Rebalanced Column Geometry (Option A: Data-Matched Alignment Standard)
            int col_w_driver = scale(180); // TOP DRIVER (fits single modules; compound pairs >23 chars cleanly ellipsize)
            int col_w_count  = scale(80);  // COUNT (small integer)
            int col_w_stall  = scale(115); // TOTAL STALL (e.g., "1245.8 ms")
            int col_w_pct    = scale(75);  // STALL % ("100.0%" is ~42px, fits with 33px margin)
            int right_fixed_total = col_w_driver + col_w_count + col_w_stall + col_w_pct;

            int col0_left  = table_rc.left + scale(16);
            int table_inner_right = table_rc.right - scale(16);
            int available_table_w = table_inner_right - col0_left;

            // Note: dialog is fixed-size (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 840px base client width);
            // col0_w is ~326px in practice at 96 DPI.
            // The scale(150) lower bound is a defensive floor for col0 only — if a future refactor
            // shrinks the dialog enough to trigger it, columns to the right will overflow the card
            // border and the layout will require a proportional-scaling rework.
            int col0_w     = std::max(scale(150), available_table_w - right_fixed_total);
            int col0_right = col0_left + col0_w;

            int col1_left  = col0_right;
            int col1_right = col1_left + col_w_driver;

            int col2_left  = col1_right;
            int col2_right = col2_left + col_w_count;

            int col3_left  = col2_right;
            int col3_right = col3_left + col_w_stall;

            int col4_left  = col3_right;
            int col4_right = col4_left + col_w_pct; // In the non-clamped case, this equals table_inner_right.

            SelectObject(mem_dc, state->font_small);
            SetTextColor(mem_dc, RGB(100, 116, 139));

            RECT h0 = { col0_left + pad, col_hdr_y, col0_right - pad, col_hdr_y + scale(20) };
            RECT h1 = { col1_left + pad, col_hdr_y, col1_right - pad, col_hdr_y + scale(20) };
            RECT h2 = { col2_left + pad, col_hdr_y, col2_right - pad, col_hdr_y + scale(20) };
            RECT h3 = { col3_left + pad, col_hdr_y, col3_right - pad, col_hdr_y + scale(20) };
            RECT h4 = { col4_left + pad, col_hdr_y, col4_right - pad, col_hdr_y + scale(20) };

            DrawTextW(mem_dc, L"HYPOTHESIS", -1, &h0, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(mem_dc, L"TOP DRIVER", -1, &h1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(mem_dc, L"COUNT", -1, &h2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(mem_dc, L"TOTAL STALL", -1, &h3, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(mem_dc, L"STALL %", -1, &h4, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // Subtle vertical column dividers in header
            HPEN pen_div = CreatePen(PS_SOLID, 1, RGB(42, 50, 68));
            HGDIOBJ old_pen = SelectObject(mem_dc, pen_div);

            int v_top = col_hdr_y + scale(2);
            int v_bot = col_hdr_y + scale(18);
            int divs[] = { col0_right, col1_right, col2_right, col3_right };
            for (int dx : divs) {
                MoveToEx(mem_dc, dx, v_top, NULL);
                LineTo(mem_dc, dx, v_bot);
            }

            // Horizontal dividing line below header
            int sep_y = col_hdr_y + scale(22);
            MoveToEx(mem_dc, table_rc.left + scale(12), sep_y, NULL);
            LineTo(mem_dc, table_rc.right - scale(12), sep_y);

            SelectObject(mem_dc, old_pen);
            DeleteObject(pen_div);

            // Table Rows
            int row_y = col_hdr_y + scale(26);
            int row_h = scale(26);

            if (summary.culprits.empty()) {
                SelectObject(mem_dc, state->font_regular);
                SetTextColor(mem_dc, RGB(148, 163, 184));
                RECT empty_rc = { table_rc.left + scale(16), col_hdr_y + scale(28), table_rc.right - scale(16), table_rc.bottom - scale(16) };
                DrawTextW(mem_dc, L"No stutters recorded in current session. Frame pacing is smooth.", -1, &empty_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                for (size_t i = 0; i < summary.culprits.size() && row_y + row_h <= table_rc.bottom - scale(8); ++i) {
                    const auto& c = summary.culprits[i];

                    if (i % 2 == 1) {
                        RECT row_rc = { table_rc.left + scale(8), row_y, table_rc.right - scale(8), row_y + row_h };
                        HBRUSH r_br = CreateSolidBrush(RGB(28, 33, 44));
                        FillRect(mem_dc, &row_rc, r_br);
                        DeleteObject(r_br);
                    }

                    SelectObject(mem_dc, state->font_regular);
                    SetTextColor(mem_dc, RGB(226, 232, 240));

                    std::wstring w_hyp = to_wide(c.hypothesis);
                    std::wstring w_drv = c.top_driver_module.empty() ? L"-" : to_wide(c.top_driver_module);
                    std::wstring w_cnt = std::to_wstring(c.count);

                    wchar_t stl_buf[64];
                    swprintf_s(stl_buf, L"%.1f ms", c.total_stall_ms);
                    wchar_t pct_buf[64];
                    swprintf_s(pct_buf, L"%.1f%%", c.stall_pct);

                    RECT r0 = { col0_left + pad, row_y, col0_right - pad, row_y + row_h };
                    RECT r1 = { col1_left + pad, row_y, col1_right - pad, row_y + row_h };
                    RECT r2 = { col2_left + pad, row_y, col2_right - pad, row_y + row_h };
                    RECT r3 = { col3_left + pad, row_y, col3_right - pad, row_y + row_h };
                    RECT r4 = { col4_left + pad, row_y, col4_right - pad, row_y + row_h };

                    DrawTextW(mem_dc, w_hyp.c_str(), -1, &r0, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextW(mem_dc, w_drv.c_str(), -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextW(mem_dc, w_cnt.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(mem_dc, stl_buf, -1, &r3, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(mem_dc, pct_buf, -1, &r4, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                    row_y += row_h;
                }
            }

            // Blit buffer to screen
            BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);

            SelectObject(mem_dc, old_bm);
            DeleteObject(mem_bm);
            DeleteDC(mem_dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCDESTROY: {
            s_benchmark_dialog_open.store(false, std::memory_order_release);
            KillTimer(hwnd, IDT_BENCH_REFRESH);
            if (state) {
                if (state->font_title) DeleteObject(state->font_title);
                if (state->font_metric_val) DeleteObject(state->font_metric_val);
                if (state->font_regular) DeleteObject(state->font_regular);
                if (state->font_bold) DeleteObject(state->font_bold);
                if (state->font_small) DeleteObject(state->font_small);
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

void ShowBenchmarkView(HWND parent_hwnd, std::shared_ptr<SessionBenchmark> benchmark, bool redact) {
    if (!benchmark) return;

    bool expected = false;
    if (!s_benchmark_dialog_open.compare_exchange_strong(expected, true)) {
        // Already open: do not allow second dialog
        return;
    }

    HINSTANCE hInstance = parent_hwnd ? (HINSTANCE)GetWindowLongPtrW(parent_hwnd, GWLP_HINSTANCE) : GetModuleHandleW(NULL);

    static bool s_class_registered = false;
    if (!s_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = BenchmarkWindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"StuttometerBenchmarkWindowClass";
        ATOM atom = RegisterClassExW(&wc);
        s_class_registered = (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    }

    UINT dpi = parent_hwnd ? GetDpiForWindow(parent_hwnd) : 96;
    if (dpi == 0) dpi = 96;

    int client_w = MulDiv(840, dpi, 96);
    int client_h = MulDiv(600, dpi, 96);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT rc = { 0, 0, client_w, client_h };
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int outer_w = rc.right - rc.left;
    int outer_h = rc.bottom - rc.top;

    RECT rc_work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc_work, 0);
    if (outer_h > (rc_work.bottom - rc_work.top) - 20) {
        outer_h = (rc_work.bottom - rc_work.top) - 20;
    }
    if (outer_w > (rc_work.right - rc_work.left) - 20) {
        outer_w = (rc_work.right - rc_work.left) - 20;
    }

    int pos_x = (rc_work.right - rc_work.left - outer_w) / 2;
    int pos_y = (rc_work.bottom - rc_work.top - outer_h) / 2;
    if (parent_hwnd) {
        RECT rc_parent{};
        GetWindowRect(parent_hwnd, &rc_parent);
        pos_x = rc_parent.left + ((rc_parent.right - rc_parent.left) - outer_w) / 2;
        pos_y = rc_parent.top + ((rc_parent.bottom - rc_parent.top) - outer_h) / 2;
    }

    BenchmarkInitParams params{ benchmark, redact };

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"StuttometerBenchmarkWindowClass",
        L"Session Benchmark Summary",
        dwStyle,
        pos_x, pos_y,
        outer_w, outer_h,
        parent_hwnd, NULL, hInstance, &params
    );

    if (!hDlg) {
        s_benchmark_dialog_open.store(false, std::memory_order_release);
        return;
    }

    apply_window_dark_titlebar(hDlg);

    if (parent_hwnd) {
        EnableWindow(parent_hwnd, FALSE);
    }
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (parent_hwnd && !IsWindow(parent_hwnd)) {
            DestroyWindow(hDlg);
            break;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hDlg)) break;
    }

    if (parent_hwnd && IsWindow(parent_hwnd)) {
        EnableWindow(parent_hwnd, TRUE);
        SetForegroundWindow(parent_hwnd);
    }
    s_benchmark_dialog_open.store(false, std::memory_order_release);

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
}

} // namespace stuttometer::gui
