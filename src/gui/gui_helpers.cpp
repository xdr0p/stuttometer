#include "gui_helpers.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <commdlg.h>

namespace stuttometer::gui {

HWND create_control(HWND parent, const ControlSpec& spec) noexcept {
    HWND h = CreateWindowExW(
        0, spec.wnd_class, spec.text ? spec.text : L"", spec.style,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)spec.id, nullptr, nullptr);
    if (!h) return nullptr;
    if (spec.font)     SendMessageW(h, WM_SETFONT, (WPARAM)spec.font, TRUE);
    if (spec.prop_key) SetPropW(h, spec.prop_key, spec.prop_value);
    if (spec.subclass_proc)
        SetWindowSubclass(h, spec.subclass_proc, spec.id, spec.ref_data);

    // Single-line EDITs are custom-painted by EditCenteredSubclassProc /
    // SettingsHotkeySubclassProc. DarkMode_Explorer makes the default EDIT
    // paint draw its own 1px dark border inside the client area, which leaks
    // through the rounded corners of our custom border. Skip theming for
    // single-line edits; multiline edits (Report Inspector) still need it
    // for dark scrollbars.
    const bool is_single_line_edit =
        (spec.wnd_class && _wcsicmp(spec.wnd_class, L"EDIT") == 0) &&
        ((spec.style & ES_MULTILINE) == 0);
    if (!is_single_line_edit) {
        apply_control_dark_theme(h);
    }
    return h;
}

HWND create_checkbox(HWND parent, int id, bool checked) noexcept {
    HWND h = create_control(parent, {
        id, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        nullptr, nullptr, nullptr, nullptr, 0
    });
    if (h) SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

HWND create_dropdown(HWND parent, int id,
                     std::initializer_list<const wchar_t*> items,
                     int initial_sel) noexcept {
    HWND h = create_control(parent, {
        id, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        g_font_ui, DarkComboSubclassProc, nullptr, nullptr, 0
    });
    if (!h) return nullptr;
    SendMessageW(h, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
    SendMessageW(h, CB_SETITEMHEIGHT, (WPARAM)0,  (LPARAM)scale_dpi(22));
    for (const wchar_t* s : items)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)s);
    if (initial_sel >= 0)
        SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(initial_sel), 0);
    return h;
}

HWND create_owner_button(HWND parent, int id, const wchar_t* text,
                         BtnStyle style, bool on_card) noexcept {
    HWND h = create_control(parent, {
        id, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        g_font_ui_bold, DarkButtonSubclassProc,
        L"BtnStyle", reinterpret_cast<HANDLE>(style),
        0
    });
    if (!h) return nullptr;
    if (on_card) SetPropW(h, L"OnCard", reinterpret_cast<HANDLE>(1));
    return h;
}

HWND create_numeric_edit(HWND parent, const NumericEditDef& def,
                         HFONT font) noexcept {
    wchar_t buf[64]{};
    // Format dispatch: %u formats unsigned 32-bit integer; %.Nf formats double.
    // (Note: Currently only %u and %.Nf are used. If 64-bit integers or wider format specifiers
    // are introduced in the future, migrate to an explicit NumFmt enum).
    if (wcschr(def.fmt, L'u') != nullptr) {
        swprintf_s(buf, def.fmt, static_cast<unsigned>(def.value));
    } else {
        swprintf_s(buf, def.fmt, def.value);
    }
    return create_control(parent, {
        def.id, L"EDIT", buf,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
        font, EditCenteredSubclassProc, nullptr, nullptr, 0
    });
}

namespace {
    thread_local HHOOK t_cbt_hook = nullptr;
    thread_local HWND t_target_owner = nullptr;

    LRESULT CALLBACK CenterMessageBoxCbtProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HCBT_ACTIVATE) {
            HWND hDlg = reinterpret_cast<HWND>(wParam);
            if (hDlg && t_target_owner && IsWindow(t_target_owner)) {
                // Ensure this is actually a dialog window (standard Win32 dialog class #32770)
                wchar_t class_name[32]{};
                if (GetClassNameW(hDlg, class_name, 32) > 0 && wcscmp(class_name, L"#32770") == 0) {
                    RECT rc_owner{}, rc_dlg{};
                    GetWindowRect(t_target_owner, &rc_owner);
                    GetWindowRect(hDlg, &rc_dlg);

                    int dlg_w = rc_dlg.right - rc_dlg.left;
                    int dlg_h = rc_dlg.bottom - rc_dlg.top;
                    if (dlg_w > 0 && dlg_h > 0) {
                        int x = rc_owner.left + ((rc_owner.right - rc_owner.left) - dlg_w) / 2;
                        int y = rc_owner.top + ((rc_owner.bottom - rc_owner.top) - dlg_h) / 2;

                        // Ensure within work area of the monitor containing owner
                        HMONITOR hMon = MonitorFromWindow(t_target_owner, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi{ sizeof(MONITORINFO) };
                        if (GetMonitorInfoW(hMon, &mi)) {
                            if (x < mi.rcWork.left) x = mi.rcWork.left;
                            if (y < mi.rcWork.top) y = mi.rcWork.top;
                            if (x + dlg_w > mi.rcWork.right) x = mi.rcWork.right - dlg_w;
                            if (y + dlg_h > mi.rcWork.bottom) y = mi.rcWork.bottom - dlg_h;
                        }

                        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

                        // Once centered, unhook immediately so subsequent activations within the modal loop have zero overhead
                        if (t_cbt_hook) {
                            UnhookWindowsHookEx(t_cbt_hook);
                            t_cbt_hook = nullptr;
                        }
                    }
                }
            }
        }
        return CallNextHookEx(t_cbt_hook, nCode, wParam, lParam);
    }
} // namespace

int show_centered_message_box(HWND hOwner, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) noexcept {
    if (!hOwner || !IsWindow(hOwner)) {
        return MessageBoxW(hOwner, lpText, lpCaption, uType);
    }

    t_target_owner = hOwner;
    t_cbt_hook = SetWindowsHookExW(
        WH_CBT,
        CenterMessageBoxCbtProc,
        NULL,
        GetCurrentThreadId()
    );

    int result = MessageBoxW(hOwner, lpText, lpCaption, uType);

    // RAII safety cleanup in case the hook wasn't unhooked during activation
    if (t_cbt_hook) {
        UnhookWindowsHookEx(t_cbt_hook);
        t_cbt_hook = nullptr;
    }
    t_target_owner = nullptr;

    return result;
}

bool register_window_class_once(bool& registered, const wchar_t* class_name, WNDPROC wnd_proc, HINSTANCE hInstance) noexcept {
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = class_name;
        ATOM atom = RegisterClassExW(&wc);
        registered = (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    }
    return registered;
}

RECT compute_centered_dialog_rect(HWND parent, int base_w, int base_h, DWORD dwStyle) noexcept {
    UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    if (dpi == 0) dpi = 96;

    int client_w = MulDiv(base_w, dpi, 96);
    int client_h = MulDiv(base_h, dpi, 96);

    RECT rc = { 0, 0, client_w, client_h };
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int outer_w = rc.right - rc.left;
    int outer_h = rc.bottom - rc.top;

    RECT rc_work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc_work, 0);
    int work_w = rc_work.right - rc_work.left;
    int work_h = rc_work.bottom - rc_work.top;
    if (outer_h > work_h - 20) outer_h = work_h - 20;
    if (outer_w > work_w - 20) outer_w = work_w - 20;

    int pos_x = rc_work.left + (work_w - outer_w) / 2;
    int pos_y = rc_work.top + (work_h - outer_h) / 2;
    if (parent) {
        RECT rc_parent{};
        GetWindowRect(parent, &rc_parent);
        pos_x = rc_parent.left + ((rc_parent.right - rc_parent.left) - outer_w) / 2;
        pos_y = rc_parent.top + ((rc_parent.bottom - rc_parent.top) - outer_h) / 2;
    }

    return { pos_x, pos_y, pos_x + outer_w, pos_y + outer_h };
}

std::optional<int> run_modal_message_loop(HWND hDlg, HWND hParent) noexcept {
    apply_window_dark_titlebar(hDlg);
    if (hParent) {
        EnableWindow(hParent, FALSE);
    }
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (hParent && !IsWindow(hParent)) {
            DestroyWindow(hDlg);
            break;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hDlg)) break;
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }

    if (msg.message == WM_QUIT) {
        return static_cast<int>(msg.wParam);
    }
    return std::nullopt;
}

void dismiss_modal_dialog(HWND hDlg) noexcept {
    HWND hParent = GetWindow(hDlg, GW_OWNER);
    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
    DestroyWindow(hDlg);
}

bool copy_utf8_to_clipboard(HWND hwnd, const std::string& utf8_text) {
    std::wstring wtext = stuttometer::utf8_to_utf16(utf8_text);
    bool ok = false;
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (wtext.size() + 1) * sizeof(wchar_t));
        if (hGlob) {
            wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hGlob));
            if (pMem) {
                wcscpy_s(pMem, wtext.size() + 1, wtext.c_str());
                GlobalUnlock(hGlob);
                if (SetClipboardData(CF_UNICODETEXT, hGlob)) {
                    ok = true;
                }
            }
            if (!ok) GlobalFree(hGlob);
        }
        CloseClipboard();
    }
    return ok;
}

bool prompt_save_file(HWND hwnd, const wchar_t* filter, const wchar_t* def_ext, wchar_t (&buf)[MAX_PATH]) noexcept {
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn) != FALSE;
}

void show_button_feedback(HWND owner_hwnd, HWND btn_hwnd, const wchar_t* label, UINT duration_ms) noexcept {
    if (!btn_hwnd) return;
    SetWindowTextW(btn_hwnd, label);
    InvalidateRect(btn_hwnd, NULL, TRUE);
    SetTimer(owner_hwnd, reinterpret_cast<UINT_PTR>(btn_hwnd), duration_ms, NULL);
}

} // namespace stuttometer::gui

