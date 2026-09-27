#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <locale.h>
#include <cmath>
#include <string>
#include <type_traits>
#include <utility>

#include "theme.hpp"
#include "dark_controls.hpp"

namespace stuttometer::gui {

template <typename T>
bool read_clamped_edit(HWND h_edit, T min_val, T max_val, T& out_val) noexcept {
    if (!h_edit) return false;
    wchar_t buf[64]{};
    if (GetWindowTextW(h_edit, buf, 64) <= 0) return false;

    std::wstring s(buf);
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    size_t last = s.find_last_not_of(L" \t\r\n");
    s = s.substr(first, last - first + 1);

    wchar_t* end = nullptr;

    if constexpr (std::is_floating_point_v<T>) {
        std::replace(s.begin(), s.end(), L',', L'.');
        _locale_t loc = _create_locale(LC_ALL, "C");
        const double v = _wcstod_l(s.c_str(), &end, loc);
        _free_locale(loc);
        if (end == s.c_str() || *end != L'\0' || std::isnan(v) || std::isinf(v)) {
            return false;
        }
        out_val = static_cast<T>(std::clamp(v,
            static_cast<double>(min_val), static_cast<double>(max_val)));
    } else {
        _locale_t loc = _create_locale(LC_ALL, "C");
        const long long v = _wcstoll_l(s.c_str(), &end, 10, loc);
        _free_locale(loc);
        if (end == s.c_str() || *end != L'\0') return false;
        out_val = static_cast<T>(std::clamp<long long>(v,
            static_cast<long long>(min_val),
            static_cast<long long>(max_val)));
    }
    return true;
}

struct ButtonColorPalette {
    HBRUSH   brush;
    HPEN     pen;
    COLORREF text_color;
};

[[nodiscard]] inline ButtonColorPalette resolve_button_palette(
    BtnStyle style, bool is_disabled, bool is_pressed, bool is_hovered) noexcept
{
    // Precondition: g_theme.init() must have run (guaranteed by
    // MainWindowProc::WM_CREATE ordering). The lookup returns handles
    // by value on every call so it self-heals after g_theme.destroy() if
    // the GUI is ever re-initialized in-process.
    if (is_disabled) {
        return { g_theme.br_btn_disabled, g_theme.pen_btn_disabled, COLOR_TEXT_DIM };
    }
    const bool hover = is_hovered && !is_pressed;
    switch (style) {
        case BtnStyle::PrimaryEmerald:
            return {
                is_pressed ? g_theme.br_btn_emerald_pressed
                           : (hover ? g_theme.br_btn_emerald_hover
                                    : g_theme.br_btn_emerald),
                is_pressed ? g_theme.pen_btn_emerald_pressed
                           : (hover ? g_theme.pen_btn_emerald_hover
                                    : g_theme.pen_btn_emerald),
                RGB(10, 24, 18)
            };
        case BtnStyle::DangerRed:
            return {
                is_pressed ? g_theme.br_btn_danger_pressed
                           : (hover ? g_theme.br_btn_danger_hover
                                    : g_theme.br_btn_danger),
                is_pressed ? g_theme.pen_btn_danger_pressed
                           : (hover ? g_theme.pen_btn_danger_hover
                                    : g_theme.pen_btn_danger),
                RGB(255, 255, 255)
            };
        case BtnStyle::QuickAction:
            return {
                is_pressed ? g_theme.br_btn_quick_pressed
                           : (hover ? g_theme.br_btn_quick_hover
                                    : g_theme.br_btn_quick),
                is_pressed ? g_theme.pen_btn_quick_pressed
                           : (hover ? g_theme.pen_btn_quick_hover
                                    : g_theme.pen_btn_quick),
                COLOR_TEXT_PRI
            };
        case BtnStyle::SecondarySlate:
        default:
            return {
                is_pressed ? g_theme.br_btn_slate_pressed
                           : (hover ? g_theme.br_btn_slate_hover
                                    : g_theme.br_btn_slate),
                is_pressed ? g_theme.pen_btn_slate_pressed
                           : (hover ? g_theme.pen_btn_slate_hover
                                    : g_theme.pen_btn_slate),
                COLOR_TEXT_PRI
            };
    }
}

struct ControlSpec {
    int            id;
    LPCWSTR        wnd_class;
    LPCWSTR        text; // Borrowed pointer. Safe: CreateWindowExW synchronously copies text into Win32 storage before returning.
    DWORD          style;
    HFONT          font;
    SUBCLASSPROC   subclass_proc;
    const wchar_t* prop_key;
    HANDLE         prop_value;
    DWORD_PTR      ref_data;
};

[[nodiscard]] HWND create_control(HWND parent, const ControlSpec& spec) noexcept;
[[nodiscard]] HWND create_checkbox(HWND parent, int id, bool checked) noexcept;
[[nodiscard]] HWND create_dropdown(HWND parent, int id,
                                   std::initializer_list<const wchar_t*> items,
                                   int initial_sel = 0) noexcept;
[[nodiscard]] HWND create_owner_button(HWND parent, int id,
                                       const wchar_t* text,
                                       BtnStyle style,
                                       bool on_card = false) noexcept;

struct NumericEditDef {
    int            id;
    double         value;
    const wchar_t* fmt;
};

[[nodiscard]] HWND create_numeric_edit(HWND parent, const NumericEditDef& def,
                                       HFONT font) noexcept;

} // namespace stuttometer::gui
