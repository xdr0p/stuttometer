#include "dark_controls.hpp"
#include "theme.hpp"
#include "gui_state.hpp"
#include "gui_helpers.hpp"

#include <algorithm>
#include <string>

namespace stuttometer::gui {

void apply_combo_popup_border(HWND h_combo, COLORREF color) {
    if (!h_combo) return;
    COMBOBOXINFO cbi = { sizeof(COMBOBOXINFO) };
    if (!GetComboBoxInfo(h_combo, &cbi)) return;
    if (!cbi.hwndList || !IsWindow(cbi.hwndList)) return;

    // Win11: DWMWA_BORDER_COLOR overrides the accent-colored popup border.
    // On Win10 this returns E_INVALIDARG and is a harmless no-op.
    DwmSetWindowAttribute(
        cbi.hwndList,
        DWMWA_BORDER_COLOR,
        &color,
        sizeof(color)
    );
}

void apply_edit_centered_padding(HWND hwnd, HFONT hFont) {
    RECT rc_client;
    GetClientRect(hwnd, &rc_client);
    const int box_h = rc_client.bottom - rc_client.top;
    const int box_w = rc_client.right - rc_client.left;
    if (box_h <= 0 || box_w <= scale_dpi(4)) return;

    if (!hFont) {
        hFont = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    }

    int font_h = scale_dpi(14);
    if (hFont) {
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            HFONT old = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
            TEXTMETRICW tm{};
            if (GetTextMetricsW(hdc, &tm)) {
                font_h = tm.tmHeight + tm.tmExternalLeading;
            }
            SelectObject(hdc, old);
            ReleaseDC(hwnd, hdc);
        }
    }

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & ES_MULTILINE) {
        const int top_pad = (box_h > font_h) ? ((box_h - font_h) / 2) : scale_dpi(2);
        RECT rc = { scale_dpi(2), top_pad, rc_client.right - scale_dpi(2), rc_client.bottom };
        SendMessageW(hwnd, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rc));
    } else {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK EditCenteredSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, EditCenteredSubclassProc, uIdSubclass);
            break;

        case WM_NCCALCSIZE: {
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            if (!(style & ES_MULTILINE) && wParam) {
                auto* p = reinterpret_cast<LPNCCALCSIZE_PARAMS>(lParam);
                int h = p->rgrc[0].bottom - p->rgrc[0].top;
                if (h > 0) {
                    HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
                    int font_h = scale_dpi(14);
                    if (hFont) {
                        HDC hdc = GetDC(hwnd);
                        if (hdc) {
                            HFONT old = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
                            TEXTMETRICW tm{};
                            if (GetTextMetricsW(hdc, &tm)) {
                                font_h = tm.tmHeight + tm.tmExternalLeading;
                            }
                            SelectObject(hdc, old);
                            ReleaseDC(hwnd, hdc);
                        }
                    }
                    int top_pad = (h > font_h) ? ((h - font_h) / 2) : 0;
                    const int horz_pad = scale_dpi(4);
                    p->rgrc[0].left += horz_pad;
                    p->rgrc[0].right -= horz_pad;
                    p->rgrc[0].top += top_pad;
                    p->rgrc[0].bottom = (std::max<LONG>)(p->rgrc[0].top + font_h, p->rgrc[0].bottom - (h - font_h - top_pad));
                }
                return 0;
            }
            break;
        }

        case WM_NCPAINT: {
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            if (!(style & ES_MULTILINE)) {
                HDC hdc = GetWindowDC(hwnd);
                if (hdc) {
                    RECT rc_win;
                    GetWindowRect(hwnd, &rc_win);
                    RECT rc = { 0, 0, rc_win.right - rc_win.left, rc_win.bottom - rc_win.top };

                    POINT pt_client = { 0, 0 };
                    ClientToScreen(hwnd, &pt_client);
                    RECT rc_client;
                    GetClientRect(hwnd, &rc_client);
                    int cl_left = pt_client.x - rc_win.left;
                    int cl_top = pt_client.y - rc_win.top;
                    int cl_right = cl_left + (rc_client.right - rc_client.left);
                    int cl_bottom = cl_top + (rc_client.bottom - rc_client.top);

                    ExcludeClipRect(hdc, cl_left, cl_top, cl_right, cl_bottom);
                    FillRect(hdc, &rc, g_theme.br_input);
                    SelectClipRgn(hdc, NULL);

                    HPEN pen = IsWindowEnabled(hwnd) ? g_theme.pen_input_border : g_theme.pen_card_border;
                    HGDIOBJ old_pen = SelectObject(hdc, pen);
                    HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, scale_dpi(6), scale_dpi(6));
                    SelectObject(hdc, old_br);
                    SelectObject(hdc, old_pen);
                    ReleaseDC(hwnd, hdc);
                }
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_theme.br_input);
            return 1;
        }

        case WM_SETFONT:
        case WM_SIZE: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HFONT hFont = (uMsg == WM_SETFONT) ? reinterpret_cast<HFONT>(wParam) : nullptr;
            apply_edit_centered_padding(hwnd, hFont);
            return res;
        }

        case WM_CHAR: {
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                SetFocus(GetParent(hwnd));
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS: {
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0);
            break;
        }

        case WM_PAINT: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HDC hdc = GetWindowDC(hwnd);
            if (hdc) {
                RECT rc_win;
                GetWindowRect(hwnd, &rc_win);
                RECT rc = { 0, 0, rc_win.right - rc_win.left, rc_win.bottom - rc_win.top };
                HPEN pen = IsWindowEnabled(hwnd) ? g_theme.pen_input_border : g_theme.pen_card_border;
                HGDIOBJ old_pen = SelectObject(hdc, pen);
                HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, scale_dpi(6), scale_dpi(6));
                SelectObject(hdc, old_br);
                SelectObject(hdc, old_pen);
                ReleaseDC(hwnd, hdc);
            }
            return res;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ReportInspectorSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ReportInspectorSubclassProc, uIdSubclass);
            break;
        case WM_SETFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HideCaret(hwnd);
            return res;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HideCaret(hwnd);
            return res;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemovePropW(hwnd, L"Hovered");
            RemovePropW(hwnd, L"BtnStyle");
            RemovePropW(hwnd, L"OnCard");
            RemoveWindowSubclass(hwnd, DarkButtonSubclassProc, uIdSubclass);
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

LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, HeaderSubclassProc, uIdSubclass);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT client_rc;
            GetClientRect(hwnd, &client_rc);

            FillRect(hdc, &client_rc, g_theme.br_list_hdr_bg);

            HPEN old_pen = (HPEN)SelectObject(hdc, g_theme.pen_list_hdr_border);
            MoveToEx(hdc, 0, client_rc.bottom - 1, NULL);
            LineTo(hdc, client_rc.right, client_rc.bottom - 1);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT_LABEL);
            HFONT old_font = (HFONT)SelectObject(hdc, g_font_ui_bold);

            int count = Header_GetItemCount(hwnd);
            for (int i = 0; i < count; ++i) {
                RECT item_rc;
                Header_GetItemRect(hwnd, i, &item_rc);

                if (i < count - 1) {
                    MoveToEx(hdc, item_rc.right - 1, item_rc.top + scale_dpi(4), NULL);
                    LineTo(hdc, item_rc.right - 1, item_rc.bottom - scale_dpi(4));
                }

                wchar_t text[128] = {0};
                HDITEMW hdi{};
                hdi.mask = HDI_TEXT | HDI_FORMAT;
                hdi.pszText = text;
                hdi.cchTextMax = 128;
                Header_GetItem(hwnd, i, &hdi);

                RECT text_rc = item_rc;
                text_rc.left += scale_dpi(8);
                text_rc.right -= scale_dpi(8);

                UINT uFormat = DT_LEFT;
                if (hdi.fmt & HDF_RIGHT) {
                    uFormat = DT_RIGHT;
                } else if (hdi.fmt & HDF_CENTER) {
                    uFormat = DT_CENTER;
                }
                DrawTextW(hdc, text, -1, &text_rc, uFormat | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            SelectObject(hdc, old_font);
            SelectObject(hdc, old_pen);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ListViewSubclassProc, uIdSubclass);
            break;

        case WM_ERASEBKGND:
            if (ListView_GetItemCount(hwnd) == 0) {
                return 1;
            }
            break;

        case WM_PAINT: {
            if (ListView_GetItemCount(hwnd) == 0) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                if (hdc) {
                    RECT client_rc;
                    GetClientRect(hwnd, &client_rc);

                    HWND hHdr = ListView_GetHeader(hwnd);
                    if (hHdr) {
                        RECT hdr_rc;
                        GetWindowRect(hHdr, &hdr_rc);
                        client_rc.top += (hdr_rc.bottom - hdr_rc.top);
                    }

                    // Fill canvas below header to eliminate column divider grid lines when empty
                    FillRect(hdc, &client_rc, g_theme.br_list_bg);

                    SetBkMode(hdc, TRANSPARENT);

                    int center_y = client_rc.top + (client_rc.bottom - client_rc.top) / 2;

                    bool is_capturing = (g_current_session_state == GuiSessionState::RUNNING || 
                                         g_current_session_state == GuiSessionState::DEGRADED_USER_ONLY || 
                                         g_current_session_state == GuiSessionState::DEGRADED_KERNEL_ONLY);
                    bool is_starting = (g_current_session_state == GuiSessionState::STARTING);
                    bool is_stopping = (g_current_session_state == GuiSessionState::STOPPING);

                    std::wstring title_str;
                    std::wstring subtitle_str;
                    COLORREF pen_col, bg_col;

                    if (is_capturing) {
                        pen_col = COLOR_ACCENT_EMERALD;
                        bg_col  = COLOR_ACCENT_EMERALD;
                        title_str = L"Actively Monitoring for Stutters...";
                        subtitle_str = L"Trace buffers recording. Stutter events and root-cause diagnoses will appear here when detected.";
                    } else if (is_starting) {
                        pen_col = COLOR_ACCENT_CYAN;
                        bg_col  = RGB(10, 40, 55);
                        title_str = L"Starting Trace Sessions...";
                        subtitle_str = L"Initializing kernel and user ETW event providers...";
                    } else if (is_stopping) {
                        pen_col = COLOR_ACCENT_AMB;
                        bg_col  = RGB(50, 35, 10);
                        title_str = L"Stopping Trace Sessions...";
                        subtitle_str = L"Flushing in-flight buffers and stopping trace sessions...";
                    } else {
                        pen_col = RGB(75, 85, 99);
                        bg_col  = RGB(24, 28, 36);
                        title_str = L"No Stutter Events Detected";
                        subtitle_str = L"Click 'Start' or press " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" to monitor frame drops and ETW telemetry.";
                    }

                    // Native Vector Status Indicator (10px Crisp Centered Pip)
                    int center_x = (client_rc.left + client_rc.right) / 2;
                    int icon_cy = center_y - scale_dpi(20);
                    int r = scale_dpi(5);

                    HPEN h_pen = CreatePen(PS_SOLID, 1, pen_col);
                    HBRUSH h_br = CreateSolidBrush(bg_col);
                    HPEN h_old_pen = (HPEN)SelectObject(hdc, h_pen);
                    HBRUSH h_old_br = (HBRUSH)SelectObject(hdc, h_br);

                    Ellipse(hdc, center_x - r, icon_cy - r, center_x + r + 1, icon_cy + r + 1);

                    SelectObject(hdc, h_old_pen);
                    SelectObject(hdc, h_old_br);
                    DeleteObject(h_pen);
                    DeleteObject(h_br);

                    // Watermark Title
                    SelectObject(hdc, g_font_ui_bold);
                    SetTextColor(hdc, COLOR_TEXT_LABEL);
                    RECT r_title = client_rc;
                    r_title.top = icon_cy + r + scale_dpi(8);
                    r_title.bottom = r_title.top + scale_dpi(20);
                    DrawTextW(hdc, title_str.c_str(), -1, &r_title, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

                    // Watermark Subtitle (Enhanced Muted Contrast #94A3B8)
                    SelectObject(hdc, g_font_ui);
                    SetTextColor(hdc, COLOR_TEXT_MUTED);
                    RECT r_sub = client_rc;
                    r_sub.top = r_title.bottom + scale_dpi(4);
                    r_sub.bottom = r_sub.top + scale_dpi(20);
                    DrawTextW(hdc, subtitle_str.c_str(), -1, &r_sub, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

                    EndPaint(hwnd, &ps);
                }
                return 0;
            }
            return DefSubclassProc(hwnd, uMsg, wParam, lParam);
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK DarkComboSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, DarkComboSubclassProc, uIdSubclass);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_NCPAINT:
            return 0;

        case CB_SETCURSEL:
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);
            return res;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);

            // Pre-fill bounding rectangle with parent card background to eliminate corner artifacts
            FillRect(hdc, &rc, g_theme.br_card);

            HBRUSH old_br = (HBRUSH)SelectObject(hdc, g_theme.br_input);
            HPEN old_pen = (HPEN)SelectObject(hdc, g_theme.pen_input_border);

            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, scale_dpi(8), scale_dpi(8));

            SelectObject(hdc, old_br);
            SelectObject(hdc, old_pen);

            wchar_t text[256] = {0};
            int cur_sel = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
            if (cur_sel != CB_ERR) {
                SendMessageW(hwnd, CB_GETLBTEXT, cur_sel, reinterpret_cast<LPARAM>(text));
            } else {
                GetWindowTextW(hwnd, text, 256);
            }

            SetBkMode(hdc, TRANSPARENT);
            COLORREF text_col = IsWindowEnabled(hwnd) ? COLOR_TEXT_PRI : COLOR_TEXT_MUTED;
            SetTextColor(hdc, text_col);
            SelectObject(hdc, g_font_ui);

            RECT text_rc = rc;
            text_rc.left += scale_dpi(8);
            text_rc.right -= scale_dpi(24);
            DrawTextW(hdc, text, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SetTextColor(hdc, IsWindowEnabled(hwnd) ? COLOR_TEXT_MUTED : COLOR_TEXT_DIM);
            RECT arrow_rc = rc;
            arrow_rc.left = rc.right - scale_dpi(22);
            arrow_rc.right = rc.right - scale_dpi(2);
            DrawTextW(hdc, L"\u25BC", -1, &arrow_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void draw_custom_button(LPDRAWITEMSTRUCT pdis) {
    HDC hdc = pdis->hDC;
    RECT rc = pdis->rcItem;

    // Pre-fill bounding rectangle with parent container background to eliminate black corner edges.
    // Resolves container background solely via L"OnCard" window property.
    HBRUSH bg_parent = GetPropW(pdis->hwndItem, L"OnCard") ? g_theme.br_card : g_theme.br_bg;
    FillRect(hdc, &rc, bg_parent);

    bool is_disabled = (pdis->itemState & ODS_DISABLED) != 0;
    bool is_pressed  = (pdis->itemState & ODS_SELECTED) != 0;
    bool is_hovered  = (GetPropW(pdis->hwndItem, L"Hovered") != nullptr) && !is_disabled;

    BtnStyle style = static_cast<BtnStyle>(reinterpret_cast<INT_PTR>(
        GetPropW(pdis->hwndItem, L"BtnStyle")));
    if (style == static_cast<BtnStyle>(0)) style = BtnStyle::SecondarySlate;

    const auto pal = resolve_button_palette(style, is_disabled, is_pressed, is_hovered);
    HBRUSH   br         = pal.brush;
    HPEN     pen        = pal.pen;
    COLORREF text_color = pal.text_color;

    HBRUSH old_br = (HBRUSH)SelectObject(hdc, br);
    HPEN old_pen = (HPEN)SelectObject(hdc, pen);

    // True 6px radius via scale_dpi(12) diameter
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, scale_dpi(12), scale_dpi(12));

    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);

    wchar_t btn_text[128] = {0};
    GetWindowTextW(pdis->hwndItem, btn_text, 128);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text_color);
    HFONT old_font = (HFONT)SelectObject(hdc, g_font_ui_bold);

    RECT calc_rc = { 0, 0, 0, 0 };
    DrawTextW(hdc, btn_text, -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int text_w = calc_rc.right - calc_rc.left;
    int btn_w = rc.right - rc.left;
    int start_x = rc.left + (btn_w - text_w) / 2;
    int offset_y = is_pressed ? 1 : 0;
    if (is_pressed) start_x += 1;

    RECT text_rc = { start_x, rc.top + offset_y, (std::min<int>)(start_x + text_w, rc.right - scale_dpi(4)), rc.bottom + offset_y };
    DrawTextW(hdc, btn_text, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(hdc, old_font);
}

} // namespace stuttometer::gui
