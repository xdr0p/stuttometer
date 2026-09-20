#include "theme.hpp"
#include <vector>

namespace stuttometer::gui {

UINT g_current_dpi = 96;

HFONT g_font_title = nullptr;
HFONT g_font_ui = nullptr;
HFONT g_font_ui_bold = nullptr;
HFONT g_font_ui_sm_bold = nullptr;
HFONT g_font_mono = nullptr;

static std::vector<HFONT> s_retired_fonts;

GdiThemeCache g_theme;

using fnSetPreferredAppMode    = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using fnFlushMenuThemes        = void(WINAPI*)();

void init_process_dark_mode() {
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme) {
        auto set_app_mode = reinterpret_cast<fnSetPreferredAppMode>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
        if (set_app_mode) {
            set_app_mode(PreferredAppMode::ForceDark);
        }
        auto flush_menu = reinterpret_cast<fnFlushMenuThemes>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136)));
        if (flush_menu) {
            flush_menu();
        }
    }
}

void apply_control_dark_theme(HWND hwnd) {
    if (!hwnd) return;
    HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
    if (hUxtheme) {
        auto allow_window = reinterpret_cast<fnAllowDarkModeForWindow>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
        if (allow_window) {
            allow_window(hwnd, true);
        }
    }
    SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
}

void apply_window_dark_titlebar(HWND hwnd) {
    if (!hwnd) return;
    BOOL use_dark_mode = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_mode, sizeof(use_dark_mode)))) {
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &use_dark_mode, sizeof(use_dark_mode));
    }
    COLORREF caption_color = RGB(13, 17, 23);   // #0D1117 (Deeper charcoal canvas)
    COLORREF text_color    = RGB(226, 232, 240); // #E2E8F0 (Soft crisp white)
    COLORREF border_color  = RGB(36, 43, 61);   // #242B3D (Subtle dark separator border)
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption_color, sizeof(caption_color));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text_color, sizeof(text_color));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, sizeof(border_color));

    DWORD corner_pref = DWMWCP_ROUND;
    (void)DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_pref, sizeof(corner_pref));
}

void create_theme_fonts(UINT dpi) {
    if (g_current_dpi == dpi && g_font_ui != nullptr) {
        return;
    }
    g_current_dpi = dpi;
    int scale_title = MulDiv(18, dpi, 96);
    int scale_ui = MulDiv(14, dpi, 96);
    int scale_ui_sm = MulDiv(12, dpi, 96);
    int scale_mono = MulDiv(13, dpi, 96);

    HFONT old_title = g_font_title;
    HFONT old_ui = g_font_ui;
    HFONT old_ui_bold = g_font_ui_bold;
    HFONT old_ui_sm_bold = g_font_ui_sm_bold;
    HFONT old_mono = g_font_mono;

    g_font_title = CreateFontW(scale_title, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_ui = CreateFontW(scale_ui, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_ui_bold = CreateFontW(scale_ui, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_ui_sm_bold = CreateFontW(scale_ui_sm, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_mono = CreateFontW(scale_mono, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    // Safely delete any older retired fonts that survived the previous transition
    for (HFONT f : s_retired_fonts) {
        if (f) DeleteObject(f);
    }
    s_retired_fonts.clear();

    // Stash current fonts into retired list so they remain valid while controls process WM_SETFONT
    if (old_title) s_retired_fonts.push_back(old_title);
    if (old_ui) s_retired_fonts.push_back(old_ui);
    if (old_ui_bold) s_retired_fonts.push_back(old_ui_bold);
    if (old_ui_sm_bold) s_retired_fonts.push_back(old_ui_sm_bold);
    if (old_mono) s_retired_fonts.push_back(old_mono);
}

void destroy_theme_fonts() {
    if (g_font_title) { DeleteObject(g_font_title); g_font_title = nullptr; }
    if (g_font_ui) { DeleteObject(g_font_ui); g_font_ui = nullptr; }
    if (g_font_ui_bold) { DeleteObject(g_font_ui_bold); g_font_ui_bold = nullptr; }
    if (g_font_ui_sm_bold) { DeleteObject(g_font_ui_sm_bold); g_font_ui_sm_bold = nullptr; }
    if (g_font_mono) { DeleteObject(g_font_mono); g_font_mono = nullptr; }

    for (HFONT f : s_retired_fonts) {
        if (f) DeleteObject(f);
    }
    s_retired_fonts.clear();
}

void GdiThemeCache::init() {
    br_bg = CreateSolidBrush(COLOR_BG);
    br_header = CreateSolidBrush(COLOR_HEADER_BG);
    br_card = CreateSolidBrush(COLOR_CARD_BG);
    br_input = CreateSolidBrush(COLOR_INPUT_BG);
    br_list_bg = CreateSolidBrush(COLOR_LIST_BG);
    br_list_alt = CreateSolidBrush(COLOR_LIST_ROW_ALT);
    br_list_sel = CreateSolidBrush(COLOR_LIST_SEL);
    br_pill = CreateSolidBrush(RGB(28, 33, 44));
    br_badge = CreateSolidBrush(RGB(24, 28, 38));
    br_list_hdr_bg = CreateSolidBrush(COLOR_LIST_HDR_BG);

    // Buttons: Primary Emerald
    br_btn_emerald = CreateSolidBrush(RGB(16, 185, 129));
    br_btn_emerald_hover = CreateSolidBrush(RGB(5, 150, 105));
    br_btn_emerald_pressed = CreateSolidBrush(RGB(4, 120, 87));
    pen_btn_emerald = CreatePen(PS_SOLID, 1, RGB(52, 211, 153));
    pen_btn_emerald_hover = CreatePen(PS_SOLID, 1, RGB(16, 185, 129));
    pen_btn_emerald_pressed = CreatePen(PS_SOLID, 1, RGB(5, 150, 105));

    // Buttons: Danger Red
    br_btn_danger = CreateSolidBrush(RGB(185, 28, 28));
    br_btn_danger_hover = CreateSolidBrush(RGB(220, 38, 38));
    br_btn_danger_pressed = CreateSolidBrush(RGB(153, 27, 27));
    pen_btn_danger = CreatePen(PS_SOLID, 1, RGB(220, 38, 38));
    pen_btn_danger_hover = CreatePen(PS_SOLID, 1, RGB(239, 68, 68));
    pen_btn_danger_pressed = CreatePen(PS_SOLID, 1, RGB(185, 28, 28));

    // Buttons: Secondary Slate
    br_btn_slate = CreateSolidBrush(RGB(28, 33, 46));
    br_btn_slate_hover = CreateSolidBrush(RGB(38, 45, 62));
    br_btn_slate_pressed = CreateSolidBrush(RGB(22, 26, 36));
    pen_btn_slate = CreatePen(PS_SOLID, 1, RGB(50, 60, 82));
    pen_btn_slate_hover = CreatePen(PS_SOLID, 1, RGB(65, 78, 105));
    pen_btn_slate_pressed = CreatePen(PS_SOLID, 1, RGB(44, 52, 72));

    // Buttons: Quick Action
    br_btn_quick = CreateSolidBrush(RGB(26, 31, 42));
    br_btn_quick_hover = CreateSolidBrush(RGB(36, 43, 60));
    br_btn_quick_pressed = CreateSolidBrush(RGB(20, 24, 34));
    pen_btn_quick = CreatePen(PS_SOLID, 1, RGB(48, 58, 78));
    pen_btn_quick_hover = CreatePen(PS_SOLID, 1, RGB(65, 78, 105));
    pen_btn_quick_pressed = CreatePen(PS_SOLID, 1, RGB(42, 50, 68));

    // Cached Attribution & Iconography Brushes (unified 35% desaturated palette)
    br_attr_game_engine = CreateSolidBrush(RGB(218, 161, 66));
    br_attr_dwm_composition = CreateSolidBrush(RGB(154, 100, 205));
    br_attr_external_contention = CreateSolidBrush(RGB(197, 86, 86));
    br_attr_unknown = CreateSolidBrush(RGB(105, 115, 130));
    br_beacon_idle = CreateSolidBrush(RGB(75, 85, 99));

    // Buttons: Disabled
    br_btn_disabled = CreateSolidBrush(RGB(20, 23, 31));
    pen_btn_disabled = CreatePen(PS_SOLID, 1, RGB(32, 38, 50));

    pen_card_border = CreatePen(PS_SOLID, 1, COLOR_CARD_BORDER);
    pen_card_divider = CreatePen(PS_SOLID, 1, COLOR_CARD_DIVIDER);
    pen_input_border = CreatePen(PS_SOLID, 1, COLOR_INPUT_BORDER);
    pen_pill_border = CreatePen(PS_SOLID, 1, RGB(48, 58, 78));
    pen_badge_border = CreatePen(PS_SOLID, 1, RGB(44, 52, 70));
    pen_list_hdr_border = CreatePen(PS_SOLID, 1, COLOR_LIST_HDR_BORDER);
    pen_focus_border = CreatePen(PS_SOLID, 1, COLOR_ACCENT_EMERALD);

    // Fallback for extreme GDI resource exhaustion
    HBRUSH default_brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    HPEN default_pen = static_cast<HPEN>(GetStockObject(BLACK_PEN));

    HBRUSH* brushes[] = {
        &br_bg, &br_header, &br_card, &br_input, &br_list_bg,
        &br_list_alt, &br_list_sel, &br_pill, &br_badge, &br_list_hdr_bg,
        &br_btn_emerald, &br_btn_emerald_hover, &br_btn_emerald_pressed,
        &br_btn_danger, &br_btn_danger_hover, &br_btn_danger_pressed,
        &br_btn_slate, &br_btn_slate_hover, &br_btn_slate_pressed,
        &br_btn_quick, &br_btn_quick_hover, &br_btn_quick_pressed,
        &br_btn_disabled,
        &br_attr_game_engine, &br_attr_dwm_composition,
        &br_attr_external_contention, &br_attr_unknown, &br_beacon_idle
    };
    for (auto* b : brushes) {
        if (!*b) *b = default_brush;
    }

    HPEN* pens[] = {
        &pen_header_border, &pen_card_border, &pen_card_divider,
        &pen_input_border, &pen_pill_border, &pen_badge_border,
        &pen_list_hdr_border, &pen_focus_border,
        &pen_btn_emerald, &pen_btn_emerald_hover, &pen_btn_emerald_pressed,
        &pen_btn_danger, &pen_btn_danger_hover, &pen_btn_danger_pressed,
        &pen_btn_slate, &pen_btn_slate_hover, &pen_btn_slate_pressed,
        &pen_btn_quick, &pen_btn_quick_hover, &pen_btn_quick_pressed,
        &pen_btn_disabled
    };
    for (auto* p : pens) {
        if (!*p) *p = default_pen;
    }
}

void GdiThemeCache::destroy() {
    if (br_bg) DeleteObject(br_bg);
    if (br_header) DeleteObject(br_header);
    if (br_card) DeleteObject(br_card);
    if (br_input) DeleteObject(br_input);
    if (br_list_bg) DeleteObject(br_list_bg);
    if (br_list_alt) DeleteObject(br_list_alt);
    if (br_list_sel) DeleteObject(br_list_sel);
    if (br_pill) DeleteObject(br_pill);
    if (br_badge) DeleteObject(br_badge);
    if (br_list_hdr_bg) DeleteObject(br_list_hdr_bg);

    if (br_btn_emerald) DeleteObject(br_btn_emerald);
    if (br_btn_emerald_hover) DeleteObject(br_btn_emerald_hover);
    if (br_btn_emerald_pressed) DeleteObject(br_btn_emerald_pressed);
    if (br_btn_danger) DeleteObject(br_btn_danger);
    if (br_btn_danger_hover) DeleteObject(br_btn_danger_hover);
    if (br_btn_danger_pressed) DeleteObject(br_btn_danger_pressed);
    if (br_btn_slate) DeleteObject(br_btn_slate);
    if (br_btn_slate_hover) DeleteObject(br_btn_slate_hover);
    if (br_btn_slate_pressed) DeleteObject(br_btn_slate_pressed);
    if (br_btn_quick) DeleteObject(br_btn_quick);
    if (br_btn_quick_hover) DeleteObject(br_btn_quick_hover);
    if (br_btn_quick_pressed) DeleteObject(br_btn_quick_pressed);
    if (br_btn_disabled) DeleteObject(br_btn_disabled);
    if (br_attr_game_engine) DeleteObject(br_attr_game_engine);
    if (br_attr_dwm_composition) DeleteObject(br_attr_dwm_composition);
    if (br_attr_external_contention) DeleteObject(br_attr_external_contention);
    if (br_attr_unknown) DeleteObject(br_attr_unknown);
    if (br_beacon_idle) DeleteObject(br_beacon_idle);

    if (pen_header_border) DeleteObject(pen_header_border);
    if (pen_card_border) DeleteObject(pen_card_border);
    if (pen_card_divider) DeleteObject(pen_card_divider);
    if (pen_input_border) DeleteObject(pen_input_border);
    if (pen_pill_border) DeleteObject(pen_pill_border);
    if (pen_badge_border) DeleteObject(pen_badge_border);
    if (pen_list_hdr_border) DeleteObject(pen_list_hdr_border);
    if (pen_focus_border) DeleteObject(pen_focus_border);

    if (pen_btn_emerald) DeleteObject(pen_btn_emerald);
    if (pen_btn_emerald_hover) DeleteObject(pen_btn_emerald_hover);
    if (pen_btn_emerald_pressed) DeleteObject(pen_btn_emerald_pressed);
    if (pen_btn_danger) DeleteObject(pen_btn_danger);
    if (pen_btn_danger_hover) DeleteObject(pen_btn_danger_hover);
    if (pen_btn_danger_pressed) DeleteObject(pen_btn_danger_pressed);
    if (pen_btn_slate) DeleteObject(pen_btn_slate);
    if (pen_btn_slate_hover) DeleteObject(pen_btn_slate_hover);
    if (pen_btn_slate_pressed) DeleteObject(pen_btn_slate_pressed);
    if (pen_btn_quick) DeleteObject(pen_btn_quick);
    if (pen_btn_quick_hover) DeleteObject(pen_btn_quick_hover);
    if (pen_btn_quick_pressed) DeleteObject(pen_btn_quick_pressed);
    if (pen_btn_disabled) DeleteObject(pen_btn_disabled);
}

} // namespace stuttometer::gui
