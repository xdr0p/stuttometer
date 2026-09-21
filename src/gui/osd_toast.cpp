#include "osd_toast.hpp"
#include "theme.hpp"
#include "stuttometer/internal/redaction_utils.hpp"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <mutex>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace stuttometer::gui {

static const wchar_t* OSD_CLASS_NAME = L"StuttometerOSDToastClass";
static std::once_flag s_osd_class_flag;

struct ProcessWindowSearch {
    DWORD pid{0};
    HWND hwnd{nullptr};
};

// Locates the process's primary viewport window by filtering out message-only,
// zero-sized, invisible tray helper, or tooltip windows via a >100x100px rect heuristic.
static BOOL CALLBACK EnumProcessWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* pSearch = reinterpret_cast<ProcessWindowSearch*>(lParam);
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id == pSearch->pid) {
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            if ((rc.right - rc.left) > 100 && (rc.bottom - rc.top) > 100) {
                pSearch->hwnd = hwnd;
                return FALSE; // Found main window
            }
        }
    }
    return TRUE;
}

static HWND find_process_main_window(uint32_t target_pid) {
    if (target_pid == 0) return nullptr;
    ProcessWindowSearch search;
    search.pid = target_pid;
    EnumWindows(EnumProcessWindowsProc, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

static std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), needed);
    return result;
}

OsdToast::OsdToast() noexcept = default;

OsdToast::~OsdToast() noexcept {
    destroy();
}

LRESULT CALLBACK OsdToast::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* toast = reinterpret_cast<OsdToast*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!toast) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_TIMER:
            if (wParam == TIMER_ID) {
                toast->on_timer();
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            toast->render(hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DPICHANGED: {
            UINT new_dpi = HIWORD(wParam);
            toast->recreate_fonts(new_dpi);
            auto* prc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, HWND_TOPMOST, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // Double-buffered in WM_PAINT

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void OsdToast::recreate_fonts(UINT dpi) noexcept {
    try {
        if (dpi == 0) dpi = 96;
        current_dpi_ = dpi;

        if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
        if (font_main_)  { DeleteObject(font_main_);  font_main_  = nullptr; }
        if (font_sub_)   { DeleteObject(font_sub_);   font_sub_   = nullptr; }

        font_title_ = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        font_main_  = CreateFontW(-MulDiv(11, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        font_sub_   = CreateFontW(-MulDiv(10, dpi, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    } catch (...) {
    }
}

void OsdToast::init_gdi_resources() noexcept {
    try {
        if (!br_bg_) br_bg_ = CreateSolidBrush(RGB(17, 21, 31));
        if (!pen_border_) pen_border_ = CreatePen(PS_SOLID, 1, RGB(42, 53, 75));
    } catch (...) {
    }
}

void OsdToast::destroy_gdi_resources() noexcept {
    try {
        if (br_bg_) { DeleteObject(br_bg_); br_bg_ = nullptr; }
        if (pen_border_) { DeleteObject(pen_border_); pen_border_ = nullptr; }
    } catch (...) {
    }
}

bool OsdToast::create(HINSTANCE hInst) noexcept {
    try {
        if (hwnd_) {
            return true; // Idempotent check
        }
        hinst_ = hInst;

        std::call_once(s_osd_class_flag, [hInst]() {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = OsdToast::WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = OSD_CLASS_NAME;
            RegisterClassExW(&wc);
        });

        DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
        DWORD style = WS_POPUP;

        hwnd_ = CreateWindowExW(
            exStyle,
            OSD_CLASS_NAME,
            L"StuttometerOSDToast",
            style,
            0, 0, 0, 0,
            nullptr, nullptr, hInst, this
        );

        if (!hwnd_) {
            return false;
        }

        // Pin timeBeginPeriod(1) strictly AFTER CreateWindowExW succeeds
        timeBeginPeriod(1);

        UINT dpi = GetDpiForWindow(hwnd_);
        recreate_fonts(dpi);
        init_gdi_resources();

        return true;
    } catch (...) {
        return false;
    }
}

void OsdToast::destroy() noexcept {
    try {
        if (hwnd_) {
            if (timer_id_) {
                KillTimer(hwnd_, timer_id_);
                timer_id_ = 0;
            }
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            timeEndPeriod(1);
        }
        if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
        if (font_main_)  { DeleteObject(font_main_);  font_main_  = nullptr; }
        if (font_sub_)   { DeleteObject(font_sub_);   font_sub_   = nullptr; }
        destroy_gdi_resources();

        state_ = State::HIDDEN;
        current_alpha_ = 0;
    } catch (...) {
    }
}

void OsdToast::show(const DiagnosticReport& report, uint32_t duration_ms, OsdPosition position) noexcept {
    try {
        if (!hwnd_) {
            return;
        }

        // Populate OsdToastData respecting redaction
        current_data_.source = report.trigger.source;
        current_data_.glitch_count = report.trigger.glitch_count;
        current_data_.duration_ms = report.trigger.duration_ms;
        current_data_.spike_ratio = report.trigger.spike_ratio;
        current_data_.attribution = report.attribution;
        current_data_.present_threshold_ms = report.present_threshold_ms;
        current_data_.trigger = report.trigger;

        if (report.redacted) {
            current_data_.process_name = "Process_REDACTED";
        } else {
            current_data_.process_name = report.target_process.empty() ? "Game Process" : report.target_process;
        }

        if (report.redacted || report.attribution_redacted) {
            current_data_.culprit = get_redacted_module_name(report.attribution_process, true);
        } else {
            current_data_.culprit = report.attribution_process.empty() ? "Unattributed" : report.attribution_process;
        }

        if (!report.diagnoses.empty()) {
            current_data_.confidence = report.diagnoses[0].confidence;
            std::string sum = report.diagnoses[0].summary;
            if (report.redacted || report.attribution_redacted) {
                auto ids = collect_report_ids(report);
                sum = redact_text_with_ids(sum, ids);
            }
            current_data_.summary = std::move(sum);
        } else {
            current_data_.confidence = 0.0;
            current_data_.summary = "No diagnosis hypothesis";
        }

        current_position_ = position;
        display_duration_ms_ = duration_ms > 0 ? duration_ms : 3500;
        auto now = std::chrono::steady_clock::now();

        if (state_ == State::HIDDEN) {
            current_alpha_ = 0;
            // Nit 4 Fix: Reset alpha to 0 before ShowWindow
            SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);

            update_position(position, report.trigger.target_pid);
            state_ = State::FADING_IN;
            state_start_tp_ = now;

            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            timer_id_ = SetTimer(hwnd_, TIMER_ID, 16, nullptr);
            InvalidateRect(hwnd_, nullptr, TRUE);
        } else {
            // Rapid re-trigger: immediately full opacity and extend display deadline
            current_alpha_ = TARGET_ALPHA;
            SetLayeredWindowAttributes(hwnd_, 0, current_alpha_, LWA_ALPHA);
            update_position(position, report.trigger.target_pid);
            state_ = State::DISPLAYING;
            state_start_tp_ = now;
            display_deadline_ = now + std::chrono::milliseconds(display_duration_ms_);
            timer_id_ = SetTimer(hwnd_, TIMER_ID, 16, nullptr);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    } catch (...) {
    }
}

void OsdToast::hide() noexcept {
    try {
        if (!hwnd_) return;
        state_ = State::HIDDEN;
        current_alpha_ = 0;
        SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd_, SW_HIDE);
        if (timer_id_) {
            KillTimer(hwnd_, timer_id_);
            timer_id_ = 0;
        }
    } catch (...) {
    }
}

bool OsdToast::is_visible() const noexcept {
    return state_ != State::HIDDEN;
}

void OsdToast::on_timer() noexcept {
    try {
        if (!hwnd_ || state_ == State::HIDDEN) {
            return;
        }

        auto now = std::chrono::steady_clock::now();

        switch (state_) {
            case State::FADING_IN: {
                double elapsed_ms = std::chrono::duration<double, std::milli>(now - state_start_tp_).count();
                double progress = std::clamp(elapsed_ms / static_cast<double>(FADE_IN_MS), 0.0, 1.0);
                current_alpha_ = static_cast<uint8_t>(progress * TARGET_ALPHA);
                SetLayeredWindowAttributes(hwnd_, 0, current_alpha_, LWA_ALPHA);

                if (progress >= 1.0) {
                    state_ = State::DISPLAYING;
                    state_start_tp_ = now;
                    display_deadline_ = now + std::chrono::milliseconds(display_duration_ms_);
                }
                break;
            }

            case State::DISPLAYING: {
                if (now >= display_deadline_) {
                    state_ = State::FADING_OUT;
                    state_start_tp_ = now;
                }
                break;
            }

            case State::FADING_OUT: {
                double elapsed_ms = std::chrono::duration<double, std::milli>(now - state_start_tp_).count();
                double progress = std::clamp(elapsed_ms / static_cast<double>(FADE_OUT_MS), 0.0, 1.0);
                current_alpha_ = static_cast<uint8_t>((1.0 - progress) * TARGET_ALPHA);
                SetLayeredWindowAttributes(hwnd_, 0, current_alpha_, LWA_ALPHA);

                if (progress >= 1.0) {
                    state_ = State::HIDDEN;
                    current_alpha_ = 0;
                    SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
                    ShowWindow(hwnd_, SW_HIDE);
                    if (timer_id_) {
                        KillTimer(hwnd_, timer_id_);
                        timer_id_ = 0;
                    }
                }
                break;
            }

            case State::HIDDEN:
                break;
        }
    } catch (...) {
    }
}

void OsdToast::update_position(OsdPosition position, uint32_t target_pid) noexcept {
    try {
        if (!hwnd_) return;

        HWND hTarget = (target_pid != 0) ? find_process_main_window(target_pid) : GetForegroundWindow();
        if (!hTarget) hTarget = GetDesktopWindow();
        HMONITOR hMon = MonitorFromWindow(hTarget, MONITOR_DEFAULTTONEAREST);

        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        GetMonitorInfoW(hMon, &mi);
        RECT work_area = mi.rcWork;

        UINT dpi = GetDpiForWindow(hwnd_);
        if (dpi == 0) dpi = 96;
        if (dpi != current_dpi_) {
            recreate_fonts(dpi);
        }

        int toast_w = MulDiv(360, dpi, 96);
        int toast_h = MulDiv(80, dpi, 96);
        int pad = MulDiv(24, dpi, 96);

        int x = 0;
        int y = 0;
        switch (position) {
            case OsdPosition::TOP_RIGHT:
                x = work_area.right - pad - toast_w;
                y = work_area.top + pad;
                break;
            case OsdPosition::BOTTOM_RIGHT:
                x = work_area.right - pad - toast_w;
                y = work_area.bottom - pad - toast_h;
                break;
            case OsdPosition::TOP_LEFT:
                x = work_area.left + pad;
                y = work_area.top + pad;
                break;
            case OsdPosition::BOTTOM_LEFT:
                x = work_area.left + pad;
                y = work_area.bottom - pad - toast_h;
                break;
        }

        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, toast_w, toast_h, SWP_NOACTIVATE);
    } catch (...) {
    }
}

void OsdToast::render(HDC hdc, const RECT& rc) noexcept {
    try {
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bmp));

        init_gdi_resources();

        COLORREF col_text_pri = RGB(241, 245, 249); // Soft white
        COLORREF col_text_sec = RGB(148, 163, 184); // Slate
        COLORREF col_accent = get_attribution_color(current_data_.attribution);
        HBRUSH br_accent = get_attribution_brush(current_data_.attribution);
        std::wstring attr_tag;

        switch (current_data_.attribution) {
            case AttributionTag::GAME_ENGINE:
                attr_tag = L"GAME ENGINE";
                break;
            case AttributionTag::DWM_COMPOSITION:
                attr_tag = L"DWM COMPOSITION";
                break;
            case AttributionTag::EXTERNAL_CONTENTION:
                attr_tag = L"EXTERNAL CONTENTION";
                break;
            case AttributionTag::UNKNOWN:
            default:
                attr_tag = L"UNKNOWN";
                break;
        }

        UINT dpi = current_dpi_ > 0 ? current_dpi_ : 96;
        int stripe_w = MulDiv(5, dpi, 96);
        int pad_left = MulDiv(16, dpi, 96);
        int pad_right = MulDiv(14, dpi, 96);
        int r1_top = MulDiv(8, dpi, 96);
        int r1_bot = MulDiv(28, dpi, 96);
        int r2_top = MulDiv(30, dpi, 96);
        int r2_bot = MulDiv(52, dpi, 96);
        int r3_top = MulDiv(54, dpi, 96);
        int r3_bot = MulDiv(74, dpi, 96);
        int callout_w = MulDiv(156, dpi, 96);

        // Fill background (cached brush)
        FillRect(mem_dc, &rc, br_bg_);

        // 1px Border (cached pen)
        HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc, pen_border_));
        HBRUSH old_br = static_cast<HBRUSH>(SelectObject(mem_dc, GetStockObject(NULL_BRUSH)));
        Rectangle(mem_dc, 0, 0, width, height);
        SelectObject(mem_dc, old_pen);

        // Left accent stripe (cached brush)
        RECT rc_stripe = { 0, 0, stripe_w, height };
        FillRect(mem_dc, &rc_stripe, br_accent);

        SetBkMode(mem_dc, TRANSPARENT);

        // Row 1: Process Name (Left) + Duration / Glitch Callout (Right)
        HFONT prev_font = static_cast<HFONT>(SelectObject(mem_dc, font_title_));
        SetTextColor(mem_dc, col_text_pri);

        std::wstring proc_w = utf8_to_wide(current_data_.process_name);
        RECT rc_proc = { pad_left, r1_top, width - callout_w - MulDiv(8, dpi, 96), r1_bot };
        DrawTextW(mem_dc, proc_w.c_str(), -1, &rc_proc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // Nit 2 Fix: OSD Duration Display for Audio Glitch vs Stutter
        std::wstring callout_str;
        if (current_data_.source == TriggerSource::AUDIO_GLITCH) {
            uint32_t cnt = current_data_.glitch_count > 0 ? current_data_.glitch_count : 1;
            callout_str = L"AUDIO GLITCH (x" + std::to_wstring(cnt) + L")";
        } else {
            std::wstringstream dss;
            dss << std::fixed << std::setprecision(1) << current_data_.duration_ms << L" ms STUTTER";
            callout_str = dss.str();
        }

        MetricSeverity sev = classify_severity(current_data_.trigger, current_data_.present_threshold_ms);
        COLORREF col_severity = get_severity_color(sev);

        SetTextColor(mem_dc, col_severity);
        RECT rc_callout = { width - callout_w - pad_right, r1_top, width - pad_right, r1_bot };
        DrawTextW(mem_dc, callout_str.c_str(), -1, &rc_callout, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Row 2: Culprit Driver/Module + Summary
        SelectObject(mem_dc, font_main_);
        SetTextColor(mem_dc, col_text_sec);

        std::wstring diag_w;
        if (!current_data_.culprit.empty() && current_data_.culprit != "Unattributed") {
            diag_w = utf8_to_wide(current_data_.culprit) + L": ";
        }
        diag_w += utf8_to_wide(current_data_.summary);

        RECT rc_diag = { pad_left, r2_top, width - pad_right, r2_bot };
        DrawTextW(mem_dc, diag_w.c_str(), -1, &rc_diag, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // Row 3: Attribution Tag pill (Left) + Confidence (Right)
        SelectObject(mem_dc, font_sub_);
        SetTextColor(mem_dc, col_accent);

        int conf_w = MulDiv(80, dpi, 96);
        RECT rc_tag = { pad_left, r3_top, width - pad_right - conf_w, r3_bot };
        DrawTextW(mem_dc, attr_tag.c_str(), -1, &rc_tag, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (current_data_.confidence > 0.0) {
            std::wstringstream css;
            css << std::fixed << std::setprecision(0) << std::round(current_data_.confidence * 100.0) << L"% CONF";
            std::wstring conf_str = css.str();

            SetTextColor(mem_dc, col_text_sec);
            RECT rc_conf = { width - pad_right - conf_w, r3_top, width - pad_right, r3_bot };
            DrawTextW(mem_dc, conf_str.c_str(), -1, &rc_conf, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // BitBlt to screen
        BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);

        SelectObject(mem_dc, prev_font);
        SelectObject(mem_dc, old_br);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
    } catch (...) {
    }
}

} // namespace stuttometer::gui
