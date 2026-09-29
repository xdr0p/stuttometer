#include "gui_helpers.hpp"

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
    apply_control_dark_theme(h);
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

} // namespace stuttometer::gui
