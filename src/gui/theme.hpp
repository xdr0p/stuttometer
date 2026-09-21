#pragma once

// Pulls in correlator.hpp for AttributionTag and MetricSeverity; keep theme.hpp free of non-color logic to minimize coupling drift.
#include "stuttometer/correlator.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace stuttometer::gui {

// Windows 10/11 Native Dark Mode Undocumented API Ordinals
enum class PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
};

void init_process_dark_mode();
void apply_control_dark_theme(HWND hwnd);
void apply_window_dark_titlebar(HWND hwnd);

// Fluent Zinc Dark Theme Palette
inline constexpr COLORREF COLOR_BG              = RGB(17, 19, 23);   // Main Canvas (#111317)
inline constexpr COLORREF COLOR_HEADER_BG       = RGB(22, 26, 34);   // Header (#161A22)
inline constexpr COLORREF COLOR_HEADER_BORDER   = RGB(38, 45, 60);   // Header Separator (#262D3C)
inline constexpr COLORREF COLOR_CARD_BG         = RGB(28, 33, 44);   // Card Panels (#1C212C - elevated contrast)
inline constexpr COLORREF COLOR_CARD_BORDER     = RGB(40, 48, 66);   // Container Outline (#283042)
inline constexpr COLORREF COLOR_CARD_DIVIDER    = RGB(36, 43, 58);   // Section Dividers (#242B3A)
inline constexpr COLORREF COLOR_INPUT_BG        = RGB(19, 22, 29);   // Input / Inspector Background (#13161D)
inline constexpr COLORREF COLOR_INPUT_BORDER    = RGB(48, 58, 78);   // Input Outline (#303A4E)

inline constexpr COLORREF COLOR_LIST_BG         = RGB(17, 19, 23);   // ListView Canvas (#111317)
inline constexpr COLORREF COLOR_LIST_ROW_ALT    = RGB(22, 25, 33);   // Alternating Row (#161921)
inline constexpr COLORREF COLOR_LIST_SEL        = RGB(30, 58, 95);   // Selected Row Highlight (#1E3A5F)
inline constexpr COLORREF COLOR_LIST_HDR_BG     = RGB(24, 28, 36);   // Header Background (#181C24)
inline constexpr COLORREF COLOR_LIST_HDR_BORDER = RGB(40, 48, 66);   // Header Border (#283042)

// Refined Typography Colors
inline constexpr COLORREF COLOR_TEXT_PRI        = RGB(255, 255, 255);// Pure White (#FFFFFF)
inline constexpr COLORREF COLOR_TEXT_BRIGHT     = RGB(241, 245, 249);// Bright Text (#F1F5F9)
inline constexpr COLORREF COLOR_TEXT_LABEL      = RGB(203, 213, 225);// Slate Label Text (#CBD5E1)
inline constexpr COLORREF COLOR_TEXT_MUTED      = RGB(148, 163, 184);// Muted Slate (#94A3B8)
inline constexpr COLORREF COLOR_TEXT_DIM        = RGB(100, 116, 139);// Dim / Hint Text (#64748B)

// Refined Semantic Accent Colors
inline constexpr COLORREF COLOR_ACCENT_EMERALD  = RGB(16, 185, 129); // Fluent Emerald (#10B981)
inline constexpr COLORREF COLOR_ACCENT_DANGER   = RGB(239, 68, 68);  // Refined Crimson (#EF4444)
inline constexpr COLORREF COLOR_ACCENT_AMB      = RGB(245, 158, 11); // Amber / Warning (#F59E0B)
inline constexpr COLORREF COLOR_ACCENT_CYAN     = RGB(56, 189, 248); // Sky / Info (#38BDF8)
inline constexpr COLORREF COLOR_ACCENT_PURPLE   = RGB(168, 85, 247); // Purple / DWM (#A855F7)

// Centralized Attribution Palette (unified 35% desaturated palette)
inline constexpr COLORREF COLOR_ATTR_GAME_ENGINE         = RGB(218, 161, 66);  // #daa142
inline constexpr COLORREF COLOR_ATTR_EXTERNAL_CONTENTION = RGB(197, 86, 86);   // #c55656
inline constexpr COLORREF COLOR_ATTR_DWM_COMPOSITION     = RGB(154, 100, 205); // #9a64cd
inline constexpr COLORREF COLOR_ATTR_UNKNOWN             = RGB(105, 115, 130); // #697382

// Centralized Severity Palette
inline constexpr COLORREF COLOR_SEV_NORMAL               = RGB(241, 245, 249); // #f1f5f9
inline constexpr COLORREF COLOR_SEV_WARNING              = RGB(245, 158, 11);  // #f59e0b
inline constexpr COLORREF COLOR_SEV_DANGER               = RGB(239, 68, 68);   // #ef4444

[[nodiscard]] inline constexpr COLORREF get_attribution_color(AttributionTag tag) noexcept {
    switch (tag) {
        case AttributionTag::GAME_ENGINE:         return COLOR_ATTR_GAME_ENGINE;
        case AttributionTag::DWM_COMPOSITION:     return COLOR_ATTR_DWM_COMPOSITION;
        case AttributionTag::EXTERNAL_CONTENTION: return COLOR_ATTR_EXTERNAL_CONTENTION;
        case AttributionTag::UNKNOWN:
        default:                                  return COLOR_ATTR_UNKNOWN;
    }
}

[[nodiscard]] inline constexpr COLORREF get_severity_color(MetricSeverity sev) noexcept {
    switch (sev) {
        case MetricSeverity::DANGER:  return COLOR_SEV_DANGER;
        case MetricSeverity::WARNING: return COLOR_SEV_WARNING;
        case MetricSeverity::NORMAL:
        default:                      return COLOR_SEV_NORMAL;
    }
}

[[nodiscard]] HBRUSH get_attribution_brush(AttributionTag tag) noexcept;

// DPI Tracking & Scaling
extern UINT g_current_dpi;

inline int scale_dpi(int px) {
    return MulDiv(px, static_cast<int>(g_current_dpi), 96);
}

// Cached Theme Fonts
extern HFONT g_font_title;
extern HFONT g_font_ui;
extern HFONT g_font_ui_bold;
extern HFONT g_font_ui_sm_bold;
extern HFONT g_font_mono;

void create_theme_fonts(UINT dpi);
void destroy_theme_fonts();

// Static GDI Theme Resource Cache
struct GdiThemeCache {
    HBRUSH br_bg{nullptr};
    HBRUSH br_header{nullptr};
    HBRUSH br_card{nullptr};
    HBRUSH br_input{nullptr};
    HBRUSH br_list_bg{nullptr};
    HBRUSH br_list_alt{nullptr};
    HBRUSH br_list_sel{nullptr};
    HBRUSH br_pill{nullptr};
    HBRUSH br_badge{nullptr};
    HBRUSH br_list_hdr_bg{nullptr};

    // Cached Button Brushes
    HBRUSH br_btn_emerald{nullptr};
    HBRUSH br_btn_emerald_hover{nullptr};
    HBRUSH br_btn_emerald_pressed{nullptr};
    HBRUSH br_btn_danger{nullptr};
    HBRUSH br_btn_danger_hover{nullptr};
    HBRUSH br_btn_danger_pressed{nullptr};
    HBRUSH br_btn_slate{nullptr};
    HBRUSH br_btn_slate_hover{nullptr};
    HBRUSH br_btn_slate_pressed{nullptr};
    HBRUSH br_btn_quick{nullptr};
    HBRUSH br_btn_quick_hover{nullptr};
    HBRUSH br_btn_quick_pressed{nullptr};
    HBRUSH br_btn_disabled{nullptr};

    // Cached Attribution & Iconography Brushes
    HBRUSH br_attr_game_engine{nullptr};
    HBRUSH br_attr_dwm_composition{nullptr};
    HBRUSH br_attr_external_contention{nullptr};
    HBRUSH br_attr_unknown{nullptr};
    HBRUSH br_beacon_idle{nullptr};

    HPEN pen_header_border{nullptr};
    HPEN pen_card_border{nullptr};
    HPEN pen_card_divider{nullptr};
    HPEN pen_input_border{nullptr};
    HPEN pen_pill_border{nullptr};
    HPEN pen_badge_border{nullptr};
    HPEN pen_list_hdr_border{nullptr};
    HPEN pen_focus_border{nullptr};

    // Cached Button Pens
    HPEN pen_btn_emerald{nullptr};
    HPEN pen_btn_emerald_hover{nullptr};
    HPEN pen_btn_emerald_pressed{nullptr};
    HPEN pen_btn_danger{nullptr};
    HPEN pen_btn_danger_hover{nullptr};
    HPEN pen_btn_danger_pressed{nullptr};
    HPEN pen_btn_slate{nullptr};
    HPEN pen_btn_slate_hover{nullptr};
    HPEN pen_btn_slate_pressed{nullptr};
    HPEN pen_btn_quick{nullptr};
    HPEN pen_btn_quick_hover{nullptr};
    HPEN pen_btn_quick_pressed{nullptr};
    HPEN pen_btn_disabled{nullptr};

    void init();
    void destroy();
};

extern GdiThemeCache g_theme;

} // namespace stuttometer::gui
