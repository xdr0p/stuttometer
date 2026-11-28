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
    COLORREF text_color    = COLOR_TEXT_SOFT_WHITE; // #E2E8F0 (Soft crisp white)
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

namespace {

template <typename Fn>
void for_each_theme_brush(GdiThemeCache& t, Fn&& fn) {
    HBRUSH* brushes[] = {
        &t.br_bg, &t.br_header, &t.br_card, &t.br_input, &t.br_list_bg,
        &t.br_pill, &t.br_badge, &t.br_list_hdr_bg,
        &t.br_btn_emerald, &t.br_btn_emerald_hover, &t.br_btn_emerald_pressed,
        &t.br_btn_danger, &t.br_btn_danger_hover, &t.br_btn_danger_pressed,
        &t.br_btn_slate, &t.br_btn_slate_hover, &t.br_btn_slate_pressed,
        &t.br_btn_quick, &t.br_btn_quick_hover, &t.br_btn_quick_pressed,
        &t.br_btn_disabled,
        &t.br_sev_normal, &t.br_sev_warning, &t.br_sev_danger
    };
    for (auto* b : brushes) {
        fn(*b);
    }
}

template <typename Fn>
void for_each_theme_pen(GdiThemeCache& t, Fn&& fn) {
    HPEN* pens[] = {
        &t.pen_header_border, &t.pen_card_border, &t.pen_card_divider,
        &t.pen_input_border, &t.pen_pill_border, &t.pen_badge_border,
        &t.pen_list_hdr_border,
        &t.pen_btn_emerald, &t.pen_btn_emerald_hover, &t.pen_btn_emerald_pressed,
        &t.pen_btn_danger, &t.pen_btn_danger_hover, &t.pen_btn_danger_pressed,
        &t.pen_btn_slate, &t.pen_btn_slate_hover, &t.pen_btn_slate_pressed,
        &t.pen_btn_quick, &t.pen_btn_quick_hover, &t.pen_btn_quick_pressed,
        &t.pen_btn_disabled
    };
    for (auto* p : pens) {
        fn(*p);
    }
}

} // namespace

void GdiThemeCache::init() {
    br_bg = CreateSolidBrush(COLOR_BG);
    br_header = CreateSolidBrush(COLOR_HEADER_BG);
    br_card = CreateSolidBrush(COLOR_CARD_BG);
    br_input = CreateSolidBrush(COLOR_INPUT_BG);
    br_list_bg = CreateSolidBrush(COLOR_LIST_BG);
    br_pill = CreateSolidBrush(COLOR_CARD_BG);
    br_badge = CreateSolidBrush(COLOR_BADGE_BG);
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

    // Cached Severity Brushes
    br_sev_normal = CreateSolidBrush(COLOR_SEV_NORMAL);
    br_sev_warning = CreateSolidBrush(COLOR_SEV_WARNING);
    br_sev_danger = CreateSolidBrush(COLOR_SEV_DANGER);

    // Buttons: Disabled
    br_btn_disabled = CreateSolidBrush(RGB(20, 23, 31));
    pen_btn_disabled = CreatePen(PS_SOLID, 1, RGB(32, 38, 50));

    pen_header_border = CreatePen(PS_SOLID, 1, COLOR_HEADER_BORDER);
    pen_card_border = CreatePen(PS_SOLID, 1, COLOR_CARD_BORDER);
    pen_card_divider = CreatePen(PS_SOLID, 1, COLOR_CARD_DIVIDER);
    pen_input_border = CreatePen(PS_SOLID, 1, COLOR_INPUT_BORDER);
    pen_pill_border = CreatePen(PS_SOLID, 1, RGB(48, 58, 78));
    pen_badge_border = CreatePen(PS_SOLID, 1, COLOR_BADGE_BORDER);
    pen_list_hdr_border = CreatePen(PS_SOLID, 1, COLOR_LIST_HDR_BORDER);

    // Fallback for extreme GDI resource exhaustion
    HBRUSH default_brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    HPEN default_pen = static_cast<HPEN>(GetStockObject(BLACK_PEN));

    for_each_theme_brush(*this, [default_brush](HBRUSH& b) {
        if (!b) b = default_brush;
    });
    for_each_theme_pen(*this, [default_pen](HPEN& p) {
        if (!p) p = default_pen;
    });
}

void GdiThemeCache::destroy() {
    for_each_theme_brush(*this, [](HBRUSH& b) {
        if (b) { DeleteObject(b); b = nullptr; }
    });
    for_each_theme_pen(*this, [](HPEN& p) {
        if (p) { DeleteObject(p); p = nullptr; }
    });
}

HBRUSH get_severity_brush(MetricSeverity sev) noexcept {
    HBRUSH fallback = g_theme.br_sev_normal ? g_theme.br_sev_normal : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    switch (sev) {
        case MetricSeverity::DANGER:  return g_theme.br_sev_danger  ? g_theme.br_sev_danger  : fallback;
        case MetricSeverity::WARNING: return g_theme.br_sev_warning ? g_theme.br_sev_warning : fallback;
        case MetricSeverity::NORMAL:
        default:                      return fallback;
    }
}

} // namespace stuttometer::gui
