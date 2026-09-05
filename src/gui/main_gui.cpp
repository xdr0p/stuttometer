#include "gui_controller.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <mutex>

#include <nlohmann/json.hpp>

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

namespace stuttometer::gui {

// Windows 10/11 Native Dark Mode Undocumented API Ordinals
enum class PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
};

using fnSetPreferredAppMode    = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
using fnFlushMenuThemes        = void(WINAPI*)();

static void init_process_dark_mode() {
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

static void apply_control_dark_theme(HWND hwnd) {
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
}

// Control IDs
constexpr int IDC_BTN_START          = 1001;
constexpr int IDC_BTN_STOP           = 1002;
constexpr int IDC_BTN_SETTINGS       = 1003;
constexpr int IDC_BTN_CLEAR          = 1004;
constexpr int IDC_BTN_EXPORT_JSON    = 1006;
constexpr int IDC_BTN_COPY_JSON      = 1007;
constexpr int IDC_COMBO_PROCESS      = 1008;
constexpr int IDC_BTN_REFRESH        = 1009;
constexpr int IDC_LIST_STUTTERS      = 1014;
constexpr int IDC_EDIT_INSPECTOR     = 1015;

// Settings Dialog Control IDs
constexpr int IDC_SET_HOTKEY_EDIT    = 2001;
constexpr int IDC_SET_CHK_SOUND      = 2002;
constexpr int IDC_SET_CHK_REDACT     = 2003;
constexpr int IDC_SET_CHK_AUDIO      = 2004;
constexpr int IDC_SET_COMBO_TIER     = 2005;
constexpr int IDC_SET_EDIT_PRE_WIN   = 2006;
constexpr int IDC_SET_EDIT_POST_WIN  = 2007;
constexpr int IDC_SET_EDIT_COOLDOWN  = 2008;
constexpr int IDC_SET_COMBO_BUFFER   = 2009;
constexpr int IDC_SET_EDIT_DPC       = 2010;
constexpr int IDC_SET_EDIT_ISR       = 2011;
constexpr int IDC_SET_EDIT_DISK      = 2012;
constexpr int IDC_SET_EDIT_CSWITCH   = 2013;
constexpr int IDC_SET_EDIT_SMI       = 2014;
constexpr int IDC_SET_BTN_RESET      = 2015;
constexpr int IDC_SET_BTN_CANCEL     = 2016;
constexpr int IDC_SET_BTN_SAVE       = 2017;
constexpr int IDC_SET_CHK_ADVANCED   = 2018;
constexpr int IDC_SET_EDIT_MEM_ALLOC = 2019;
constexpr int IDC_SET_EDIT_MEM_TRIM  = 2020;
constexpr int IDC_SET_EDIT_MEM_PHYS  = 2021;
constexpr int IDC_SET_COMBO_TRIG_MODE = 2022;
constexpr int IDC_SET_EDIT_SPIKE_MULT = 2023;
constexpr int IDC_SET_EDIT_MIN_DELTA  = 2024;
constexpr int IDC_SET_CHK_JUDDER      = 2025;
constexpr int IDC_SET_EDIT_TARGET_FPS = 2026;
constexpr int IDC_SET_CHK_AUTO_SAVE        = 2027;
constexpr int IDC_SET_EDIT_AUTO_SAVE       = 2028;
constexpr int IDC_SET_BTN_BROWSE_AUTO_SAVE = 2029;
constexpr int IDC_SET_EDIT_D3D12_PSO       = 2030;
constexpr int IDC_SET_EDIT_VRAM_DEMOTED    = 2031;

// Global Hotkeys
constexpr int ID_HOTKEY_TOGGLE_CAPTURE = 9001;

// Button Styling Categories
enum class BtnStyle : INT_PTR {
    PrimaryEmerald = 1,
    DangerRed = 2,
    SecondarySlate = 3,
    QuickAction = 4
};

// Fluent Zinc Dark Theme Palette
const COLORREF COLOR_BG              = RGB(17, 19, 23);   // Main Canvas (#111317)
const COLORREF COLOR_HEADER_BG       = RGB(22, 26, 34);   // Header (#161A22)
const COLORREF COLOR_HEADER_BORDER   = RGB(38, 45, 60);   // Header Separator (#262D3C)
const COLORREF COLOR_CARD_BG         = RGB(24, 28, 36);   // Card Panels (#181C24)
const COLORREF COLOR_CARD_BORDER     = RGB(40, 48, 66);   // Container Outline (#283042)
const COLORREF COLOR_CARD_DIVIDER    = RGB(36, 43, 58);   // Section Dividers (#242B3A)
const COLORREF COLOR_INPUT_BG        = RGB(19, 22, 29);   // Input / Inspector Background (#13161D)
const COLORREF COLOR_INPUT_BORDER    = RGB(48, 58, 78);   // Input Outline (#303A4E)

const COLORREF COLOR_LIST_BG         = RGB(17, 19, 23);   // ListView Canvas (#111317)
const COLORREF COLOR_LIST_ROW_ALT    = RGB(22, 25, 33);   // Alternating Row (#161921)
const COLORREF COLOR_LIST_SEL        = RGB(30, 58, 95);   // Selected Row Highlight (#1E3A5F)
const COLORREF COLOR_LIST_HDR_BG     = RGB(24, 28, 36);   // Header Background (#181C24)
const COLORREF COLOR_LIST_HDR_BORDER = RGB(40, 48, 66);   // Header Border (#283042)

// Refined Typography Colors
const COLORREF COLOR_TEXT_PRI        = RGB(255, 255, 255);// Pure White (#FFFFFF)
const COLORREF COLOR_TEXT_BRIGHT     = RGB(241, 245, 249);// Bright Text (#F1F5F9)
const COLORREF COLOR_TEXT_LABEL      = RGB(203, 213, 225);// Slate Label Text (#CBD5E1)
const COLORREF COLOR_TEXT_MUTED      = RGB(148, 163, 184);// Muted Slate (#94A3B8)
const COLORREF COLOR_TEXT_DIM        = RGB(100, 116, 139);// Dim / Hint Text (#64748B)

// Refined Semantic Accent Colors
const COLORREF COLOR_ACCENT_EMERALD  = RGB(16, 185, 129); // Fluent Emerald (#10B981)
const COLORREF COLOR_ACCENT_DANGER   = RGB(239, 68, 68);  // Refined Crimson (#EF4444)
const COLORREF COLOR_ACCENT_AMB      = RGB(245, 158, 11); // Amber / Warning (#F59E0B)
const COLORREF COLOR_ACCENT_CYAN     = RGB(56, 189, 248); // Sky / Info (#38BDF8)

// Stored Report Item
struct StutterRecord {
    uint32_t id{0};
    std::string timestamp;
    std::string process_name;
    std::string trigger_reason;
    double duration_ms{0.0};
    std::string top_hypothesis;
    double confidence{0.0};
    std::unique_ptr<DiagnosticReport> report;
};

// Global UI State
static HWND g_hwnd_main = nullptr;
static HWND g_h_settings_dlg = nullptr;
static bool g_is_admin = false;
static std::unique_ptr<GuiController> g_controller;
static std::deque<StutterRecord> g_stutters;
static uint32_t g_next_stutter_id = 1;
static int g_selected_stutter_index = -1;

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

    void init() {
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
            &br_btn_disabled
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

    void destroy() {
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
};

static GdiThemeCache g_theme;

// Cached Fonts
static HFONT g_font_title = nullptr;
static HFONT g_font_ui = nullptr;
static HFONT g_font_ui_bold = nullptr;
static HFONT g_font_ui_sm_bold = nullptr;
static HFONT g_font_mono = nullptr;

// Control Handles
static HWND g_h_lbl_target = nullptr;
static HWND g_h_combo_process = nullptr;

static HWND g_h_btn_start = nullptr;
static HWND g_h_btn_stop = nullptr;
static HWND g_h_btn_settings = nullptr;
static HWND g_h_btn_clear = nullptr;
static HWND g_h_btn_export = nullptr;
static HWND g_h_btn_copy = nullptr;
static HWND g_h_list_stutters = nullptr;
static HWND g_h_edit_inspector = nullptr;
static HWND g_h_list_header = nullptr;

// Centralized Settings & Preferences State (Single Source of Truth)
static GuiConfig g_settings_config;
static bool g_sound_cues_enabled = true;
static UINT g_hotkey_vk = VK_F11;
static UINT g_hotkey_mods = MOD_CONTROL;
static std::wstring g_settings_file_path;
static std::string g_settings_last_target_process;
static ProcessList g_cached_processes;
static std::wstring g_status_text = L"IDLE (Ready to monitor)";

static UINT g_current_dpi = 96;

// Strict DPI Helper
static inline int scale_dpi(int px) {
    return MulDiv(px, static_cast<int>(g_current_dpi), 96);
}

// Centralized FPS to Frame Latency Threshold (ms) Helper (Clamped 2.0ms - 200.0ms)
static inline double fps_to_present_threshold_ms(double fps) {
    if (fps <= 0.0) return 16.67; // Default 60 FPS
    return std::clamp(1000.0 / fps, 2.0, 200.0);
}

// Helper: Convert UTF-8 std::string to std::wstring
static std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int num_chars = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), NULL, 0);
    if (num_chars <= 0) return std::wstring();
    std::wstring result(num_chars, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), result.data(), num_chars);
    return result;
}

// Helper: Convert std::wstring to UTF-8 std::string
static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int num_bytes = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), NULL, 0, NULL, NULL);
    if (num_bytes <= 0) return std::string();
    std::string result(num_bytes, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), result.data(), num_bytes, NULL, NULL);
    return result;
}

static GuiConfig read_gui_config();
static void update_fonts_for_dpi(UINT dpi);
static void update_metrics_text();
static LRESULT CALLBACK DarkComboSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
static LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
static LRESULT CALLBACK EditCenteredSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

static std::deque<std::wstring> g_engine_logs;
static inline void append_engine_log(std::wstring log_msg) {
    g_engine_logs.push_back(std::move(log_msg));
    if (g_engine_logs.size() > 200) {
        g_engine_logs.erase(g_engine_logs.begin());
    }
}

// Format hotkey for human-readable UI display
static std::wstring format_hotkey_display(UINT fsModifiers, UINT vk) {
    std::wstring res;
    if (fsModifiers & MOD_CONTROL) res += L"Ctrl+";
    if (fsModifiers & MOD_SHIFT)   res += L"Shift+";
    if (fsModifiers & MOD_ALT)     res += L"Alt+";
    if (fsModifiers & MOD_WIN)     res += L"Win+";

    if (vk >= VK_F1 && vk <= VK_F24) {
        res += L"F" + std::to_wstring(vk - VK_F1 + 1);
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        res += static_cast<wchar_t>(vk);
    } else if (vk == VK_PAUSE) {
        res += L"Pause";
    } else if (vk == VK_SCROLL) {
        res += L"ScrollLock";
    } else if (vk == VK_INSERT) {
        res += L"Insert";
    } else if (vk == VK_DELETE) {
        res += L"Delete";
    } else if (vk == VK_HOME) {
        res += L"Home";
    } else if (vk == VK_END) {
        res += L"End";
    } else if (vk == VK_PRIOR) {
        res += L"PageUp";
    } else if (vk == VK_NEXT) {
        res += L"PageDown";
    } else {
        wchar_t key_name[64]{};
        UINT scan_code = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(scan_code << 16, key_name, 64) > 0) {
            res += key_name;
        } else if (vk != 0) {
            res += L"Key" + std::to_wstring(vk);
        } else {
            res += L"None";
        }
    }
    return res;
}

// Resolve AppData settings path (%LOCALAPPDATA%\Stuttometer\settings.json)
static void init_settings_path() {
    wchar_t appdata_path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata_path))) {
        std::wstring dir = std::wstring(appdata_path) + L"\\Stuttometer";
        CreateDirectoryW(dir.c_str(), NULL);
        g_settings_file_path = dir + L"\\settings.json";
    }
}

// Load saved user configuration
static void load_user_settings() {
    init_settings_path();
    if (g_settings_file_path.empty()) return;
    try {
        std::ifstream f(g_settings_file_path);
        if (!f.is_open()) return;
        nlohmann::json j;
        f >> j;

        if (j.contains("hotkey_vk") && j["hotkey_vk"].is_number_unsigned()) {
            g_hotkey_vk = j["hotkey_vk"].get<UINT>();
        }
        if (j.contains("hotkey_modifiers") && j["hotkey_modifiers"].is_number_unsigned()) {
            g_hotkey_mods = j["hotkey_modifiers"].get<UINT>();
        }
        if (j.contains("sound_cues_enabled") && j["sound_cues_enabled"].is_boolean()) {
            g_sound_cues_enabled = j["sound_cues_enabled"].get<bool>();
        }
        if (j.contains("min_fps_threshold") && j["min_fps_threshold"].is_number()) {
            double thresh = j["min_fps_threshold"].get<double>();
            if (thresh >= 5.0 && thresh <= 500.0) {
                g_settings_config.present_threshold_ms = fps_to_present_threshold_ms(thresh);
            }
        }
        if (j.contains("provider_tier") && j["provider_tier"].is_string()) {
            std::string tier = j["provider_tier"].get<std::string>();
            if (tier == "full" || tier == "standard" || tier == "minimal") {
                g_settings_config.provider_tier = tier;
            }
        }
        if (j.contains("enable_audio_glitch") && j["enable_audio_glitch"].is_boolean()) {
            g_settings_config.enable_audio = j["enable_audio_glitch"].get<bool>();
        }
        if (j.contains("enable_pii_redaction") && j["enable_pii_redaction"].is_boolean()) {
            g_settings_config.redact = j["enable_pii_redaction"].get<bool>();
        }
        if (j.contains("window_pre_ms") && j["window_pre_ms"].is_number()) {
            double v = j["window_pre_ms"].get<double>();
            if (v >= 50.0 && v <= 1000.0) g_settings_config.window_pre_ms = v;
        }
        if (j.contains("window_post_ms") && j["window_post_ms"].is_number()) {
            double v = j["window_post_ms"].get<double>();
            if (v >= 0.0 && v <= 200.0) g_settings_config.window_post_ms = v;
        }
        if (j.contains("cooldown_ms") && j["cooldown_ms"].is_number()) {
            double v = j["cooldown_ms"].get<double>();
            if (v >= 100.0 && v <= 10000.0) g_settings_config.cooldown_ms = v;
        }
        if (j.contains("buffer_slots") && j["buffer_slots"].is_number_unsigned()) {
            uint32_t v = j["buffer_slots"].get<uint32_t>();
            if (v >= 65536 && v <= 1048576) g_settings_config.buffer_slots = v;
        }
        if (j.contains("dpc_threshold_us") && j["dpc_threshold_us"].is_number_unsigned()) {
            uint32_t v = j["dpc_threshold_us"].get<uint32_t>();
            if (v >= 100 && v <= 50000) g_settings_config.dpc_threshold_us = v;
        }
        if (j.contains("isr_threshold_us") && j["isr_threshold_us"].is_number_unsigned()) {
            uint32_t v = j["isr_threshold_us"].get<uint32_t>();
            if (v >= 50 && v <= 50000) g_settings_config.isr_threshold_us = v;
        }
        if (j.contains("disk_threshold_ms") && j["disk_threshold_ms"].is_number_unsigned()) {
            uint32_t v = j["disk_threshold_ms"].get<uint32_t>();
            if (v >= 1 && v <= 1000) g_settings_config.disk_threshold_ms = v;
        }
        if (j.contains("cswitch_preempt_ms") && j["cswitch_preempt_ms"].is_number_unsigned()) {
            uint32_t v = j["cswitch_preempt_ms"].get<uint32_t>();
            if (v >= 1 && v <= 500) g_settings_config.cswitch_preempt_ms = v;
        }
        if (j.contains("smi_severity_threshold_ms") && j["smi_severity_threshold_ms"].is_number()) {
            double v = j["smi_severity_threshold_ms"].get<double>();
            if (v >= 10.0 && v <= 100.0) g_settings_config.smi_severity_threshold_ms = v;
        }
        if (j.contains("d3d12_pso_threshold_ms") && j["d3d12_pso_threshold_ms"].is_number_unsigned()) {
            uint32_t v = j["d3d12_pso_threshold_ms"].get<uint32_t>();
            if (v >= 1 && v <= 500) g_settings_config.d3d12_pso_threshold_ms = v;
        }
        if (j.contains("vram_demoted_threshold_mb") && j["vram_demoted_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["vram_demoted_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) g_settings_config.vram_demoted_threshold_mb = v;
        }
        if (j.contains("mem_alloc_threshold_mb") && j["mem_alloc_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["mem_alloc_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) g_settings_config.mem_alloc_threshold_mb = v;
        }
        if (j.contains("mem_trim_threshold_mb") && j["mem_trim_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["mem_trim_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) g_settings_config.mem_trim_threshold_mb = v;
        }
        if (j.contains("mem_physical_latency_us") && j["mem_physical_latency_us"].is_number_unsigned()) {
            uint32_t v = j["mem_physical_latency_us"].get<uint32_t>();
            if (v >= 50 && v <= 50000) g_settings_config.mem_physical_latency_us = v;
        }
        if (j.contains("frame_trigger_mode") && j["frame_trigger_mode"].is_string()) {
            std::string m = j["frame_trigger_mode"].get<std::string>();
            if (m == "dynamic") g_settings_config.frame_trigger_mode = FrameTriggerMode::DYNAMIC_ONLY;
            else if (m == "static") g_settings_config.frame_trigger_mode = FrameTriggerMode::STATIC_ONLY;
            else g_settings_config.frame_trigger_mode = FrameTriggerMode::HYBRID;
        }
        if (j.contains("spike_multiplier") && j["spike_multiplier"].is_number()) {
            double v = j["spike_multiplier"].get<double>();
            if (v >= 1.2 && v <= 10.0) g_settings_config.spike_multiplier = v;
        }
        if (j.contains("min_spike_delta_ms") && j["min_spike_delta_ms"].is_number()) {
            double v = j["min_spike_delta_ms"].get<double>();
            if (v >= 1.0 && v <= 50.0) g_settings_config.min_spike_delta_ms = v;
        }
        if (j.contains("enable_judder_detection") && j["enable_judder_detection"].is_boolean()) {
            g_settings_config.enable_judder_detection = j["enable_judder_detection"].get<bool>();
        }
        if (j.contains("auto_save_dir") && j["auto_save_dir"].is_string()) {
            g_settings_config.output_dir = utf8_to_wstring(j["auto_save_dir"].get<std::string>());
        }
        if (j.contains("last_target_process") && j["last_target_process"].is_string()) {
            g_settings_last_target_process = j["last_target_process"].get<std::string>();
        }
    } catch (...) {
        // Non-fatal, keep defaults
    }
}

// Persist user configuration
static void save_user_settings() {
    if (g_settings_file_path.empty()) return;
    try {
        nlohmann::json j;
        j["settings_version"] = 1;
        j["hotkey_vk"] = g_hotkey_vk;
        j["hotkey_modifiers"] = g_hotkey_mods;
        j["sound_cues_enabled"] = g_sound_cues_enabled;
        j["min_fps_threshold"] = (g_settings_config.present_threshold_ms > 0.0) ? (1000.0 / g_settings_config.present_threshold_ms) : 60.0;
        j["provider_tier"] = g_settings_config.provider_tier;
        j["enable_audio_glitch"] = g_settings_config.enable_audio;
        j["enable_pii_redaction"] = g_settings_config.redact;
        j["window_pre_ms"] = g_settings_config.window_pre_ms;
        j["window_post_ms"] = g_settings_config.window_post_ms;
        j["cooldown_ms"] = g_settings_config.cooldown_ms;
        j["buffer_slots"] = g_settings_config.buffer_slots;
        j["dpc_threshold_us"] = g_settings_config.dpc_threshold_us;
        j["isr_threshold_us"] = g_settings_config.isr_threshold_us;
        j["disk_threshold_ms"] = g_settings_config.disk_threshold_ms;
        j["cswitch_preempt_ms"] = g_settings_config.cswitch_preempt_ms;
        j["smi_severity_threshold_ms"] = g_settings_config.smi_severity_threshold_ms;
        j["d3d12_pso_threshold_ms"] = g_settings_config.d3d12_pso_threshold_ms;
        j["vram_demoted_threshold_mb"] = g_settings_config.vram_demoted_threshold_mb;
        j["mem_alloc_threshold_mb"] = g_settings_config.mem_alloc_threshold_mb;
        j["mem_trim_threshold_mb"] = g_settings_config.mem_trim_threshold_mb;
        j["mem_physical_latency_us"] = g_settings_config.mem_physical_latency_us;
        j["frame_trigger_mode"] = std::string(frame_trigger_mode_to_string(g_settings_config.frame_trigger_mode));
        j["spike_multiplier"] = g_settings_config.spike_multiplier;
        j["min_spike_delta_ms"] = g_settings_config.min_spike_delta_ms;
        j["enable_judder_detection"] = g_settings_config.enable_judder_detection;
        j["auto_save_dir"] = wstring_to_utf8(g_settings_config.output_dir);

        if (g_h_combo_process && IsWindow(g_h_combo_process)) {
            int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
            if (cur_sel > 0) {
                LRESULT data = SendMessageW(g_h_combo_process, CB_GETITEMDATA, cur_sel, 0);
                if (data >= 0 && data < static_cast<LRESULT>(g_cached_processes.size())) {
                    j["last_target_process"] = wstring_to_utf8(g_cached_processes[data].name);
                }
            } else {
                j["last_target_process"] = "";
            }
        }

        std::ofstream f(g_settings_file_path);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (...) {
        // Non-fatal
    }
}

// -----------------------------------------------------------------------------
// Settings Dialog Implementation
// -----------------------------------------------------------------------------
struct SettingsDialogState {
    UINT hotkey_vk{VK_F11};
    UINT hotkey_mods{MOD_CONTROL};
    UINT original_hotkey_vk{VK_F11};
    UINT original_hotkey_mods{MOD_CONTROL};
    bool advanced_unlocked{false};

    HWND h_hotkey_edit{nullptr};
    HWND h_chk_sound{nullptr};
    HWND h_chk_redact{nullptr};
    HWND h_chk_audio{nullptr};
    HWND h_chk_auto_save{nullptr};
    HWND h_edit_auto_save{nullptr};
    HWND h_btn_browse_auto_save{nullptr};
    HWND h_chk_advanced{nullptr};
    HWND h_combo_tier{nullptr};
    HWND h_edit_pre_win{nullptr};
    HWND h_edit_post_win{nullptr};
    HWND h_edit_cooldown{nullptr};
    HWND h_combo_buffer{nullptr};
    HWND h_edit_dpc{nullptr};
    HWND h_edit_isr{nullptr};
    HWND h_edit_disk{nullptr};
    HWND h_edit_cswitch{nullptr};
    HWND h_edit_smi{nullptr};
    HWND h_edit_mem_alloc{nullptr};
    HWND h_edit_mem_trim{nullptr};
    HWND h_edit_mem_phys{nullptr};
    HWND h_edit_d3d12_pso{nullptr};
    HWND h_edit_vram_demoted{nullptr};
    HWND h_combo_trig_mode{nullptr};
    HWND h_edit_target_fps{nullptr};
    HWND h_edit_spike_mult{nullptr};
    HWND h_edit_min_delta{nullptr};
    HWND h_chk_judder{nullptr};
    HWND h_btn_reset{nullptr};
    HWND h_btn_cancel{nullptr};
    HWND h_btn_save{nullptr};

    // Synchronized Hit-Testing Rectangles for Checkbox Labels
    RECT rc_lbl_snd{};
    RECT rc_lbl_rd{};
    RECT rc_lbl_aud{};
    RECT rc_lbl_auto_save{};
    RECT rc_lbl_adv{};
    RECT rc_lbl_judder{};
};

static LRESULT CALLBACK SettingsHotkeySubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* state = reinterpret_cast<SettingsDialogState*>(dwRefData);
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, SettingsHotkeySubclassProc, uIdSubclass);
            break;

        case WM_SETCURSOR: {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }

        case WM_SETFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (state) {
                state->original_hotkey_vk = state->hotkey_vk;
                state->original_hotkey_mods = state->hotkey_mods;
            }
            SetWindowTextW(hwnd, L"Ctrl/Alt + ...");
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0);
            HideCaret(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return res;
        }

        case WM_KILLFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (state) {
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(hwnd, hk_str.c_str());
            }
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0);
            HideCaret(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return res;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd);
            HideCaret(hwnd);
            return 0;
        }

        case WM_PAINT: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HideCaret(hwnd);
            return res;
        }

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;

        case WM_CHAR:
        case WM_UNICHAR:
        case WM_SYSCHAR:
            return 0; // Block raw typing

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (state) {
                    state->hotkey_vk = state->original_hotkey_vk;
                    state->hotkey_mods = state->original_hotkey_mods;
                    std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                    SetWindowTextW(hwnd, hk_str.c_str());
                }
                SetFocus(GetParent(hwnd));
                return 0;
            }

            if (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL) {
                SetWindowTextW(hwnd, L"Ctrl + ...");
                return 0;
            }
            if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) {
                SetWindowTextW(hwnd, L"Alt + ...");
                return 0;
            }
            if (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT) {
                SetWindowTextW(hwnd, L"Shift + ...");
                return 0;
            }

            // Note: Ctrl and Alt (along with standalone function/navigation keys) are the only
            // modifier hotkeys supported by design to ensure reliable global registration and prevent
            // conflicts with Windows shell shortcuts or accidental game interruption.
            UINT vk = static_cast<UINT>(wParam);
            bool ctrl_down  = ((GetKeyState(VK_CONTROL) & 0x8000) != 0) || ((GetKeyState(VK_LCONTROL) & 0x8000) != 0) || ((GetKeyState(VK_RCONTROL) & 0x8000) != 0);
            bool alt_down   = ((GetKeyState(VK_MENU) & 0x8000) != 0) || ((GetKeyState(VK_LMENU) & 0x8000) != 0) || ((GetKeyState(VK_RMENU) & 0x8000) != 0) || ((lParam & (1 << 29)) != 0);
            bool shift_down = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetKeyState(VK_LSHIFT) & 0x8000) != 0) || ((GetKeyState(VK_RSHIFT) & 0x8000) != 0);

            bool is_f_key = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL || vk == VK_INSERT;

            if ((vk == VK_F1 || vk == VK_F5 || vk == VK_F10) && !ctrl_down && !alt_down && !shift_down) {
                SetWindowTextW(hwnd, L"Ctrl/Alt + [Key]");
                return 0;
            }

            if (!ctrl_down && !alt_down && !is_f_key) {
                SetWindowTextW(hwnd, L"Ctrl/Alt + [Key]");
                return 0;
            }

            UINT fsModifiers = MOD_NOREPEAT;
            if (ctrl_down)  fsModifiers |= MOD_CONTROL;
            if (shift_down) fsModifiers |= MOD_SHIFT;
            if (alt_down)   fsModifiers |= MOD_ALT;

            if (state) {
                state->hotkey_vk = vk;
                state->hotkey_mods = fsModifiers;
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(hwnd, hk_str.c_str());
            }

            SetFocus(GetParent(hwnd));
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

static void update_advanced_controls_enablement(SettingsDialogState* state) {
    if (!state) return;
    BOOL enable = state->advanced_unlocked ? TRUE : FALSE;
    EnableWindow(state->h_combo_tier, enable);
    EnableWindow(state->h_edit_pre_win, enable);
    EnableWindow(state->h_edit_post_win, enable);
    EnableWindow(state->h_edit_cooldown, enable);
    EnableWindow(state->h_combo_buffer, enable);
    EnableWindow(state->h_edit_dpc, enable);
    EnableWindow(state->h_edit_isr, enable);
    EnableWindow(state->h_edit_disk, enable);
    EnableWindow(state->h_edit_cswitch, enable);
    EnableWindow(state->h_edit_smi, enable);
    EnableWindow(state->h_edit_mem_alloc, enable);
    EnableWindow(state->h_edit_mem_trim, enable);
    EnableWindow(state->h_edit_mem_phys, enable);
    EnableWindow(state->h_edit_d3d12_pso, enable);
    EnableWindow(state->h_edit_vram_demoted, enable);
    EnableWindow(state->h_combo_trig_mode, enable);
    EnableWindow(state->h_edit_target_fps, enable);
    EnableWindow(state->h_edit_spike_mult, enable);
    EnableWindow(state->h_edit_min_delta, enable);
    EnableWindow(state->h_chk_judder, enable);

    InvalidateRect(state->h_combo_tier, NULL, TRUE);
    InvalidateRect(state->h_combo_buffer, NULL, TRUE);
    InvalidateRect(state->h_edit_pre_win, NULL, TRUE);
    InvalidateRect(state->h_edit_post_win, NULL, TRUE);
    InvalidateRect(state->h_edit_cooldown, NULL, TRUE);
    InvalidateRect(state->h_edit_dpc, NULL, TRUE);
    InvalidateRect(state->h_edit_isr, NULL, TRUE);
    InvalidateRect(state->h_edit_disk, NULL, TRUE);
    InvalidateRect(state->h_edit_cswitch, NULL, TRUE);
    InvalidateRect(state->h_edit_smi, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_alloc, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_trim, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_phys, NULL, TRUE);
    InvalidateRect(state->h_edit_d3d12_pso, NULL, TRUE);
    InvalidateRect(state->h_edit_vram_demoted, NULL, TRUE);
    InvalidateRect(state->h_combo_trig_mode, NULL, TRUE);
    InvalidateRect(state->h_edit_target_fps, NULL, TRUE);
    InvalidateRect(state->h_edit_spike_mult, NULL, TRUE);
    InvalidateRect(state->h_edit_min_delta, NULL, TRUE);
    InvalidateRect(state->h_chk_judder, NULL, TRUE);
}

static void layout_settings_controls(HWND hwnd, SettingsDialogState* state) {
    if (!state) return;

    RECT client_rc;
    GetClientRect(hwnd, &client_rc);
    int width = client_rc.right;

    const int margin = scale_dpi(16);
    const int gap = scale_dpi(14);
    const int col_w = (width - margin * 2 - gap) / 2;

    const int c_left_x = margin;
    const int c_right_x = c_left_x + col_w + gap;

    HDC hdc = GetDC(hwnd);
    HFONT old_font = (HFONT)SelectObject(hdc, g_font_ui_bold);

    const wchar_t* txt_snd = L"Enable Sound Cues";
    const wchar_t* txt_rd = L"Redact PII (Sanitize paths & usernames in reports & exports)";
    const wchar_t* txt_aud = L"Enable Audio Glitch Trigger (Microsoft-Windows-Audio ID 11)";
    const wchar_t* txt_as = L"Auto-save JSON reports to folder:";
    const wchar_t* txt_adv = L"Unlock Advanced Engine & Threshold Settings";
    const wchar_t* txt_jud = L"Enable Presentation Cadence Judder Detection (35% alternating swing)";

    SIZE sz_snd{}, sz_rd{}, sz_aud{}, sz_as{}, sz_adv{}, sz_jud{};
    GetTextExtentPoint32W(hdc, txt_snd, static_cast<int>(wcslen(txt_snd)), &sz_snd);
    GetTextExtentPoint32W(hdc, txt_rd, static_cast<int>(wcslen(txt_rd)), &sz_rd);
    GetTextExtentPoint32W(hdc, txt_aud, static_cast<int>(wcslen(txt_aud)), &sz_aud);
    GetTextExtentPoint32W(hdc, txt_as, static_cast<int>(wcslen(txt_as)), &sz_as);
    GetTextExtentPoint32W(hdc, txt_adv, static_cast<int>(wcslen(txt_adv)), &sz_adv);
    GetTextExtentPoint32W(hdc, txt_jud, static_cast<int>(wcslen(txt_jud)), &sz_jud);

    SelectObject(hdc, old_font);
    ReleaseDC(hwnd, hdc);

    // ==========================================
    // LEFT COLUMN: Card 1 (General Preferences)
    // ==========================================
    const int c1_x = c_left_x;
    const int c1_y = scale_dpi(14);
    const int c1_w = col_w;

    int r1_y = c1_y + scale_dpi(28);
    MoveWindow(state->h_hotkey_edit, c1_x + scale_dpi(116), r1_y, scale_dpi(105), scale_dpi(26), TRUE);
    RECT rc_hk = { scale_dpi(2), scale_dpi(4), scale_dpi(105) - scale_dpi(2), scale_dpi(26) };
    SendMessageW(state->h_hotkey_edit, EM_SETRECTNP, 0, (LPARAM)&rc_hk);

    MoveWindow(state->h_chk_sound, c1_x + scale_dpi(232), r1_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_snd = { c1_x + scale_dpi(254), r1_y, c1_x + c1_w - scale_dpi(8), r1_y + scale_dpi(26) };

    int r2_y = c1_y + scale_dpi(56);
    MoveWindow(state->h_chk_redact, c1_x + scale_dpi(14), r2_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_rd = { c1_x + scale_dpi(38), r2_y, c1_x + c1_w - scale_dpi(8), r2_y + scale_dpi(24) };

    int r3_y = c1_y + scale_dpi(84);
    MoveWindow(state->h_chk_audio, c1_x + scale_dpi(14), r3_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_aud = { c1_x + scale_dpi(38), r3_y, c1_x + c1_w - scale_dpi(8), r3_y + scale_dpi(24) };

    int r4_y = c1_y + scale_dpi(112);
    MoveWindow(state->h_chk_auto_save, c1_x + scale_dpi(14), r4_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_auto_save = { c1_x + scale_dpi(38), r4_y, c1_x + c1_w - scale_dpi(8), r4_y + scale_dpi(24) };

    int r5_y = c1_y + scale_dpi(142);
    int browse_w = scale_dpi(66);
    int edit_as_w = c1_w - scale_dpi(28) - browse_w - scale_dpi(8);
    MoveWindow(state->h_edit_auto_save, c1_x + scale_dpi(14), r5_y, edit_as_w, scale_dpi(24), TRUE);
    RECT rc_as = { scale_dpi(4), scale_dpi(3), edit_as_w - scale_dpi(4), scale_dpi(24) };
    SendMessageW(state->h_edit_auto_save, EM_SETRECTNP, 0, (LPARAM)&rc_as);
    MoveWindow(state->h_btn_browse_auto_save, c1_x + scale_dpi(14) + edit_as_w + scale_dpi(8), r5_y, browse_w, scale_dpi(24), TRUE);

    // ==========================================
    // LEFT COLUMN: Card 2 (Frame Pacing & Dynamic Triggers)
    // ==========================================
    const int c2_x = c_left_x;
    const int c2_y = scale_dpi(202);
    const int c2_w = col_w;

    int r6_y = c2_y + scale_dpi(32);
    MoveWindow(state->h_combo_trig_mode, c2_x + scale_dpi(110), r6_y + scale_dpi(2), c2_w - scale_dpi(124), scale_dpi(150), TRUE);

    int r7_y = c2_y + scale_dpi(68);
    MoveWindow(state->h_edit_target_fps, c2_x + scale_dpi(150), r7_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_fps = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_target_fps, EM_SETRECTNP, 0, (LPARAM)&rc_fps);

    int r8_y = c2_y + scale_dpi(102);
    MoveWindow(state->h_edit_spike_mult, c2_x + scale_dpi(150), r8_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_sm = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_spike_mult, EM_SETRECTNP, 0, (LPARAM)&rc_sm);

    int r9_y = c2_y + scale_dpi(136);
    MoveWindow(state->h_edit_min_delta, c2_x + scale_dpi(150), r9_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_md = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_min_delta, EM_SETRECTNP, 0, (LPARAM)&rc_md);

    int r10_y = c2_y + scale_dpi(170);
    MoveWindow(state->h_chk_judder, c2_x + scale_dpi(14), r10_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_judder = { c2_x + scale_dpi(38), r10_y, c2_x + c2_w - scale_dpi(8), r10_y + scale_dpi(26) };

    // ==========================================
    // RIGHT COLUMN: Card 3 (Advanced Engine Tuning)
    // ==========================================
    const int c3_x = c_right_x;
    const int c3_y = scale_dpi(14);
    const int c3_w = col_w;

    int r11_y = c3_y + scale_dpi(30);
    MoveWindow(state->h_chk_advanced, c3_x + scale_dpi(14), r11_y + scale_dpi(4), scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_adv = { c3_x + scale_dpi(38), r11_y, c3_x + c3_w - scale_dpi(8), r11_y + scale_dpi(26) };

    int r12_y = c3_y + scale_dpi(76);
    MoveWindow(state->h_combo_tier, c3_x + scale_dpi(110), r12_y + scale_dpi(2), c3_w - scale_dpi(124), scale_dpi(200), TRUE);

    int r13_y = c3_y + scale_dpi(108);
    MoveWindow(state->h_edit_pre_win, c3_x + scale_dpi(66), r13_y + scale_dpi(1), scale_dpi(46), scale_dpi(24), TRUE);
    RECT rc_pre = { scale_dpi(2), scale_dpi(3), scale_dpi(46) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_pre_win, EM_SETRECTNP, 0, (LPARAM)&rc_pre);

    MoveWindow(state->h_edit_post_win, c3_x + scale_dpi(150), r13_y + scale_dpi(1), scale_dpi(40), scale_dpi(24), TRUE);
    RECT rc_post = { scale_dpi(2), scale_dpi(3), scale_dpi(40) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_post_win, EM_SETRECTNP, 0, (LPARAM)&rc_post);

    MoveWindow(state->h_edit_cooldown, c3_x + scale_dpi(258), r13_y + scale_dpi(1), scale_dpi(48), scale_dpi(24), TRUE);
    RECT rc_cd = { scale_dpi(2), scale_dpi(3), scale_dpi(48) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_cooldown, EM_SETRECTNP, 0, (LPARAM)&rc_cd);

    int r14_y = c3_y + scale_dpi(138);
    MoveWindow(state->h_combo_buffer, c3_x + scale_dpi(110), r14_y + scale_dpi(2), c3_w - scale_dpi(124), scale_dpi(200), TRUE);

    // ==========================================
    // RIGHT COLUMN: Card 4 (Correlation Anomaly Thresholds)
    // ==========================================
    const int c4_x = c_right_x;
    const int c4_y = scale_dpi(202);

    int t1_y = c4_y + scale_dpi(30);
    MoveWindow(state->h_edit_dpc, c4_x + scale_dpi(160), t1_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_dpc = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_dpc, EM_SETRECTNP, 0, (LPARAM)&rc_dpc);

    int t2_y = c4_y + scale_dpi(63);
    MoveWindow(state->h_edit_isr, c4_x + scale_dpi(160), t2_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_isr = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_isr, EM_SETRECTNP, 0, (LPARAM)&rc_isr);

    int t3_y = c4_y + scale_dpi(96);
    MoveWindow(state->h_edit_disk, c4_x + scale_dpi(160), t3_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_disk = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_disk, EM_SETRECTNP, 0, (LPARAM)&rc_disk);

    int t4_y = c4_y + scale_dpi(129);
    MoveWindow(state->h_edit_cswitch, c4_x + scale_dpi(160), t4_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_cs = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_cswitch, EM_SETRECTNP, 0, (LPARAM)&rc_cs);

    int t5_y = c4_y + scale_dpi(162);
    MoveWindow(state->h_edit_smi, c4_x + scale_dpi(160), t5_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_smi = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_smi, EM_SETRECTNP, 0, (LPARAM)&rc_smi);

    int t6_y = c4_y + scale_dpi(195);
    MoveWindow(state->h_edit_mem_alloc, c4_x + scale_dpi(160), t6_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_mem_alloc = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_mem_alloc, EM_SETRECTNP, 0, (LPARAM)&rc_mem_alloc);

    int t7_y = c4_y + scale_dpi(228);
    MoveWindow(state->h_edit_mem_trim, c4_x + scale_dpi(160), t7_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_mem_trim = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_mem_trim, EM_SETRECTNP, 0, (LPARAM)&rc_mem_trim);

    int t8_y = c4_y + scale_dpi(261);
    MoveWindow(state->h_edit_mem_phys, c4_x + scale_dpi(160), t8_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_mem_phys = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_mem_phys, EM_SETRECTNP, 0, (LPARAM)&rc_mem_phys);

    int t9_y = c4_y + scale_dpi(294);
    MoveWindow(state->h_edit_d3d12_pso, c4_x + scale_dpi(160), t9_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_d3d12 = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_d3d12_pso, EM_SETRECTNP, 0, (LPARAM)&rc_d3d12);

    int t10_y = c4_y + scale_dpi(327);
    MoveWindow(state->h_edit_vram_demoted, c4_x + scale_dpi(160), t10_y + scale_dpi(1), scale_dpi(55), scale_dpi(24), TRUE);
    RECT rc_vram = { scale_dpi(2), scale_dpi(3), scale_dpi(55) - scale_dpi(2), scale_dpi(24) };
    SendMessageW(state->h_edit_vram_demoted, EM_SETRECTNP, 0, (LPARAM)&rc_vram);

    // ==========================================
    // FOOTER BUTTONS
    // ==========================================
    const int f_y = scale_dpi(620);
    const int f_h = scale_dpi(32);
    MoveWindow(state->h_btn_reset, margin, f_y, scale_dpi(130), f_h, TRUE);

    int save_w = scale_dpi(120);
    int cancel_w = scale_dpi(90);
    int save_x = width - margin - save_w;
    int cancel_x = save_x - scale_dpi(10) - cancel_w;

    MoveWindow(state->h_btn_cancel, cancel_x, f_y, cancel_w, f_h, TRUE);
    MoveWindow(state->h_btn_save, save_x, f_y, save_w, f_h, TRUE);
}

// Forward declare button draw
static void draw_custom_button(LPDRAWITEMSTRUCT pdis);
static void apply_control_dark_theme(HWND hwnd);

static void dismiss_settings_dialog(HWND hDlg) {
    HWND hParent = GetWindow(hDlg, GW_OWNER);
    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
    DestroyWindow(hDlg);
}

static LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_CLOSE: {
            dismiss_settings_dialog(hwnd);
            return 0;
        }
        case WM_CREATE: {
            state = new SettingsDialogState();
            state->hotkey_vk = g_hotkey_vk;
            state->hotkey_mods = g_hotkey_mods;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            // Preferences Controls
            std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
            state->h_hotkey_edit = CreateWindowExW(0, L"EDIT", hk_str.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER | ES_READONLY, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_HOTKEY_EDIT, NULL, NULL);
            SendMessageW(state->h_hotkey_edit, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_hotkey_edit);
            SetWindowSubclass(state->h_hotkey_edit, SettingsHotkeySubclassProc, IDC_SET_HOTKEY_EDIT, reinterpret_cast<DWORD_PTR>(state));

            state->h_chk_sound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_SOUND, NULL, NULL);
            SendMessageW(state->h_chk_sound, BM_SETCHECK, g_sound_cues_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_sound);

            state->h_chk_redact = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_REDACT, NULL, NULL);
            SendMessageW(state->h_chk_redact, BM_SETCHECK, g_settings_config.redact ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_redact);

            state->h_chk_audio = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_AUDIO, NULL, NULL);
            SendMessageW(state->h_chk_audio, BM_SETCHECK, g_settings_config.enable_audio ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_audio);

            state->h_chk_auto_save = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_AUTO_SAVE, NULL, NULL);
            SendMessageW(state->h_chk_auto_save, BM_SETCHECK, !g_settings_config.output_dir.empty() ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_auto_save);

            state->h_edit_auto_save = CreateWindowExW(0, L"EDIT", g_settings_config.output_dir.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_AUTO_SAVE, NULL, NULL);
            SendMessageW(state->h_edit_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            apply_control_dark_theme(state->h_edit_auto_save);

            state->h_btn_browse_auto_save = CreateWindowExW(0, L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_BROWSE_AUTO_SAVE, NULL, NULL);
            SetPropW(state->h_btn_browse_auto_save, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetWindowSubclass(state->h_btn_browse_auto_save, DarkButtonSubclassProc, IDC_SET_BTN_BROWSE_AUTO_SAVE, 0);

            // Advanced Settings Safeguard Checkbox
            state->h_chk_advanced = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_ADVANCED, NULL, NULL);
            SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_advanced);

            // ETW Trace & Buffer Controls
            state->h_combo_tier = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_TIER, NULL, NULL);
            SendMessageW(state->h_combo_tier, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Standard (Kernel DPC + ISR + Disk I/O) [Default]");
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Full (Kernel DPC + Disk + Context Switch)");
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Minimal (User DXGI Present only)");
            int tier_sel = (g_settings_config.provider_tier == "full") ? 1 : ((g_settings_config.provider_tier == "minimal") ? 2 : 0);
            SendMessageW(state->h_combo_tier, CB_SETCURSEL, tier_sel, 0);
            apply_control_dark_theme(state->h_combo_tier);
            SetWindowSubclass(state->h_combo_tier, DarkComboSubclassProc, IDC_SET_COMBO_TIER, 0);

            wchar_t num_buf[64]{};

            swprintf_s(num_buf, L"%.1f", g_settings_config.window_pre_ms);
            state->h_edit_pre_win = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_PRE_WIN, NULL, NULL);
            SendMessageW(state->h_edit_pre_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_pre_win);

            swprintf_s(num_buf, L"%.1f", g_settings_config.window_post_ms);
            state->h_edit_post_win = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_POST_WIN, NULL, NULL);
            SendMessageW(state->h_edit_post_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_post_win);

            swprintf_s(num_buf, L"%.0f", g_settings_config.cooldown_ms);
            state->h_edit_cooldown = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_COOLDOWN, NULL, NULL);
            SendMessageW(state->h_edit_cooldown, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_cooldown);

            state->h_combo_buffer = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_BUFFER, NULL, NULL);
            SendMessageW(state->h_combo_buffer, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));

            struct BufOption { const wchar_t* label; uint32_t slots; };
            BufOption buf_opts[] = {
                { L"65,536 slots (~4 MB RAM)", 65536 },
                { L"131,072 slots (~8 MB RAM)", 131072 },
                { L"262,144 slots (~16 MB RAM) [Default]", 262144 },
                { L"524,288 slots (~33 MB RAM)", 524288 },
                { L"1,048,576 slots (~67 MB RAM)", 1048576 }
            };
            int buf_sel_idx = 2;
            for (int i = 0; i < 5; ++i) {
                int idx = static_cast<int>(SendMessageW(state->h_combo_buffer, CB_ADDSTRING, 0, (LPARAM)buf_opts[i].label));
                SendMessageW(state->h_combo_buffer, CB_SETITEMDATA, idx, (LPARAM)buf_opts[i].slots);
                if (buf_opts[i].slots == g_settings_config.buffer_slots) buf_sel_idx = idx;
            }
            SendMessageW(state->h_combo_buffer, CB_SETCURSEL, buf_sel_idx, 0);
            apply_control_dark_theme(state->h_combo_buffer);
            SetWindowSubclass(state->h_combo_buffer, DarkComboSubclassProc, IDC_SET_COMBO_BUFFER, 0);

            // Correlation Cutoff Controls
            swprintf_s(num_buf, L"%u", g_settings_config.dpc_threshold_us);
            state->h_edit_dpc = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_DPC, NULL, NULL);
            SendMessageW(state->h_edit_dpc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_dpc);

            swprintf_s(num_buf, L"%u", g_settings_config.isr_threshold_us);
            state->h_edit_isr = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_ISR, NULL, NULL);
            SendMessageW(state->h_edit_isr, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_isr);

            swprintf_s(num_buf, L"%u", g_settings_config.disk_threshold_ms);
            state->h_edit_disk = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_DISK, NULL, NULL);
            SendMessageW(state->h_edit_disk, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_disk);

            swprintf_s(num_buf, L"%u", g_settings_config.cswitch_preempt_ms);
            state->h_edit_cswitch = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_CSWITCH, NULL, NULL);
            SendMessageW(state->h_edit_cswitch, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_cswitch);

            swprintf_s(num_buf, L"%.1f", g_settings_config.smi_severity_threshold_ms);
            state->h_edit_smi = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_SMI, NULL, NULL);
            SendMessageW(state->h_edit_smi, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_smi);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_alloc_threshold_mb);
            state->h_edit_mem_alloc = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_ALLOC, NULL, NULL);
            SendMessageW(state->h_edit_mem_alloc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_alloc);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_trim_threshold_mb);
            state->h_edit_mem_trim = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_TRIM, NULL, NULL);
            SendMessageW(state->h_edit_mem_trim, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_trim);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_physical_latency_us);
            state->h_edit_mem_phys = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_PHYS, NULL, NULL);
            SendMessageW(state->h_edit_mem_phys, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_phys);

            swprintf_s(num_buf, L"%u", g_settings_config.d3d12_pso_threshold_ms);
            state->h_edit_d3d12_pso = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_D3D12_PSO, NULL, NULL);
            SendMessageW(state->h_edit_d3d12_pso, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_d3d12_pso);
            SetWindowSubclass(state->h_edit_d3d12_pso, EditCenteredSubclassProc, IDC_SET_EDIT_D3D12_PSO, 0);

            swprintf_s(num_buf, L"%u", g_settings_config.vram_demoted_threshold_mb);
            state->h_edit_vram_demoted = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_VRAM_DEMOTED, NULL, NULL);
            SendMessageW(state->h_edit_vram_demoted, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_vram_demoted);
            SetWindowSubclass(state->h_edit_vram_demoted, EditCenteredSubclassProc, IDC_SET_EDIT_VRAM_DEMOTED, 0);

            // Frame Pacing & Dynamic Relative Trigger Controls
            state->h_combo_trig_mode = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_TRIG_MODE, NULL, NULL);
            SendMessageW(state->h_combo_trig_mode, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Hybrid (Relative Spike + Judder + Static Floor) [Default]");
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Dynamic Only (Pure Relative & Judder Triggers)");
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Static Only (Legacy Fixed Threshold)");
            int tm_sel = (g_settings_config.frame_trigger_mode == FrameTriggerMode::DYNAMIC_ONLY) ? 1 : ((g_settings_config.frame_trigger_mode == FrameTriggerMode::STATIC_ONLY) ? 2 : 0);
            SendMessageW(state->h_combo_trig_mode, CB_SETCURSEL, tm_sel, 0);
            apply_control_dark_theme(state->h_combo_trig_mode);
            SetWindowSubclass(state->h_combo_trig_mode, DarkComboSubclassProc, IDC_SET_COMBO_TRIG_MODE, 0);

            swprintf_s(num_buf, L"%.0f", (g_settings_config.present_threshold_ms > 0.0) ? (1000.0 / g_settings_config.present_threshold_ms) : 60.0);
            state->h_edit_target_fps = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_TARGET_FPS, NULL, NULL);
            SendMessageW(state->h_edit_target_fps, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_target_fps);

            swprintf_s(num_buf, L"%.1f", g_settings_config.spike_multiplier);
            state->h_edit_spike_mult = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_SPIKE_MULT, NULL, NULL);
            SendMessageW(state->h_edit_spike_mult, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_spike_mult);

            swprintf_s(num_buf, L"%.1f", g_settings_config.min_spike_delta_ms);
            state->h_edit_min_delta = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MIN_DELTA, NULL, NULL);
            SendMessageW(state->h_edit_min_delta, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_min_delta);

            state->h_chk_judder = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_JUDDER, NULL, NULL);
            SendMessageW(state->h_chk_judder, BM_SETCHECK, g_settings_config.enable_judder_detection ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_judder);

            // Action Buttons
            state->h_btn_reset = CreateWindowExW(0, L"BUTTON", L"Reset Defaults", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_RESET, NULL, NULL);
            SetPropW(state->h_btn_reset, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SetWindowSubclass(state->h_btn_reset, DarkButtonSubclassProc, IDC_SET_BTN_RESET, 0);

            state->h_btn_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_CANCEL, NULL, NULL);
            SetPropW(state->h_btn_cancel, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SetWindowSubclass(state->h_btn_cancel, DarkButtonSubclassProc, IDC_SET_BTN_CANCEL, 0);

            state->h_btn_save = CreateWindowExW(0, L"BUTTON", L"Save & Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_SAVE, NULL, NULL);
            SetPropW(state->h_btn_save, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::PrimaryEmerald));
            SetWindowSubclass(state->h_btn_save, DarkButtonSubclassProc, IDC_SET_BTN_SAVE, 0);

            if (!state->h_hotkey_edit || !state->h_chk_sound || !state->h_chk_redact ||
                !state->h_chk_audio || !state->h_btn_cancel || !state->h_btn_save) {
                std::cerr << "[GUI] Error: Failed to allocate essential controls for Settings dialog.\n";
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return -1;
            }

            layout_settings_controls(hwnd, state);
            update_advanced_controls_enablement(state);
            return 0;
        }

        case WM_SIZE: {
            layout_settings_controls(hwnd, state);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            if (state) {
                if (PtInRect(&state->rc_lbl_snd, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_sound, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_sound, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_rd, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_redact, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_redact, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_aud, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_audio, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_audio, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_auto_save, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_auto_save, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_auto_save, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_adv, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_advanced, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_advanced, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                    state->advanced_unlocked = !cur;
                    update_advanced_controls_enablement(state);
                    InvalidateRect(hwnd, NULL, TRUE);
                } else if (PtInRect(&state->rc_lbl_judder, pt) && state->advanced_unlocked) {
                    BOOL cur = (SendMessageW(state->h_chk_judder, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_judder, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                }
            }
            break;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (pdis->CtlType == ODT_BUTTON) {
                draw_custom_button(pdis);
                return TRUE;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            int width = client_rc.right;
            int height = client_rc.bottom;

            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bmp));
            HBRUSH old_br = static_cast<HBRUSH>(SelectObject(mem_dc, g_theme.br_card));
            HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc, g_theme.pen_card_border));
            HFONT old_font = static_cast<HFONT>(GetCurrentObject(mem_dc, OBJ_FONT));

            FillRect(mem_dc, &client_rc, g_theme.br_bg);

            const int margin = scale_dpi(16);
            const int gap = scale_dpi(14);
            const int col_w = (width - margin * 2 - gap) / 2;

            const int c_left_x = margin;
            const int c_right_x = c_left_x + col_w + gap;

            // Card 1: General Preferences (Left Top)
            const int c1_x = c_left_x;
            const int c1_y = scale_dpi(14);
            const int c1_w = col_w;
            const int c1_h = scale_dpi(176);
            RoundRect(mem_dc, c1_x, c1_y, c1_x + c1_w, c1_y + c1_h, scale_dpi(10), scale_dpi(10));

            // Card 2: Frame Pacing & Dynamic Triggers (Left Bottom)
            const int c2_x = c_left_x;
            const int c2_y = scale_dpi(202);
            const int c2_w = col_w;
            const int c2_h = scale_dpi(400);
            RoundRect(mem_dc, c2_x, c2_y, c2_x + c2_w, c2_y + c2_h, scale_dpi(10), scale_dpi(10));

            // Card 3: Advanced Engine Tuning (Right Top)
            const int c3_x = c_right_x;
            const int c3_y = scale_dpi(14);
            const int c3_w = col_w;
            const int c3_h = scale_dpi(176);
            RoundRect(mem_dc, c3_x, c3_y, c3_x + c3_w, c3_y + c3_h, scale_dpi(10), scale_dpi(10));

            // Card 4: Correlation Anomaly Thresholds (Right Bottom)
            const int c4_x = c_right_x;
            const int c4_y = scale_dpi(202);
            const int c4_w = col_w;
            const int c4_h = scale_dpi(400);
            RoundRect(mem_dc, c4_x, c4_y, c4_x + c4_w, c4_y + c4_h, scale_dpi(10), scale_dpi(10));

            // Section Headers
            SetBkMode(mem_dc, TRANSPARENT);
            SelectObject(mem_dc, g_font_ui_sm_bold);
            SetTextColor(mem_dc, COLOR_TEXT_LABEL);

            RECT t1 = { c1_x + scale_dpi(14), c1_y + scale_dpi(8), c1_x + c1_w, c1_y + scale_dpi(24) };
            DrawTextW(mem_dc, L"GENERAL PREFERENCES", -1, &t1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t2 = { c2_x + scale_dpi(14), c2_y + scale_dpi(8), c2_x + c2_w, c2_y + scale_dpi(24) };
            DrawTextW(mem_dc, L"FRAME PACING & DYNAMIC TRIGGERS", -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t3 = { c3_x + scale_dpi(14), c3_y + scale_dpi(8), c3_x + c3_w, c3_y + scale_dpi(24) };
            DrawTextW(mem_dc, L"ADVANCED ENGINE & BUFFER TUNING", -1, &t3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t4 = { c4_x + scale_dpi(14), c4_y + scale_dpi(8), c4_x + c4_w, c4_y + scale_dpi(24) };
            DrawTextW(mem_dc, L"CORRELATION ANOMALY THRESHOLDS", -1, &t4, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Card 1 Labels (General Preferences)
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, COLOR_TEXT_LABEL);

            int r1_y = c1_y + scale_dpi(32);
            RECT rc_lbl_hk = { c1_x + scale_dpi(14), r1_y, c1_x + scale_dpi(112), r1_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Capture Hotkey:", -1, &rc_lbl_hk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextW(mem_dc, L"Enable Sound Cues", -1, &state->rc_lbl_snd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextW(mem_dc, L"Redact PII (Sanitize paths & usernames in reports & exports)", -1, &state->rc_lbl_rd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextW(mem_dc, L"Enable Audio Glitch Trigger (Microsoft-Windows-Audio ID 11)", -1, &state->rc_lbl_aud, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextW(mem_dc, L"Auto-save JSON reports to folder:", -1, &state->rc_lbl_auto_save, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Card 2 Labels (Frame Pacing & Dynamic Triggers)
            COLORREF adv_lbl_color = state->advanced_unlocked ? COLOR_TEXT_LABEL : COLOR_TEXT_MUTED;
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);

            int r4_y = c2_y + scale_dpi(32);
            int r5_y = c2_y + scale_dpi(68);
            int r6_y = c2_y + scale_dpi(102);
            int r7_y = c2_y + scale_dpi(136);

            RECT rc_lbl_tm = { c2_x + scale_dpi(14), r4_y, c2_x + scale_dpi(105), r4_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Trigger Mode:", -1, &rc_lbl_tm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_lbl_fps = { c2_x + scale_dpi(14), r5_y, c2_x + scale_dpi(144), r5_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Target FPS Floor:", -1, &rc_lbl_fps, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_fps_unit = { c2_x + scale_dpi(212), r5_y, c2_x + c2_w - scale_dpi(8), r5_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"FPS (10 - 500, default: 60)", -1, &rc_lbl_fps_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_sm = { c2_x + scale_dpi(14), r6_y, c2_x + scale_dpi(144), r6_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Spike Multiplier:", -1, &rc_lbl_sm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_sm_unit = { c2_x + scale_dpi(212), r6_y, c2_x + c2_w - scale_dpi(8), r6_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"x (1.2 - 10.0x baseline avg frame time)", -1, &rc_lbl_sm_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_md = { c2_x + scale_dpi(14), r7_y, c2_x + scale_dpi(144), r7_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Min Spike Delta:", -1, &rc_lbl_md, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_md_unit = { c2_x + scale_dpi(212), r7_y, c2_x + c2_w - scale_dpi(8), r7_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms (1.0 - 50.0 ms, noise floor)", -1, &rc_lbl_md_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            DrawTextW(mem_dc, L"Enable Presentation Cadence Judder Detection (35% swing)", -1, &state->rc_lbl_judder, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Card 3: Advanced Unlock Label & Warning Banner
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, COLOR_TEXT_LABEL);
            DrawTextW(mem_dc, L"Unlock Advanced Engine & Threshold Settings", -1, &state->rc_lbl_adv, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_warn = { c3_x + scale_dpi(14), c3_y + scale_dpi(54), c3_x + c3_w - scale_dpi(10), c3_y + scale_dpi(72) };
            SelectObject(mem_dc, g_font_ui);
            if (state->advanced_unlocked) {
                SetTextColor(mem_dc, COLOR_ACCENT_AMB);
                DrawTextW(mem_dc, L"\u26A0 Caution: Alters buffer memory & cutoffs.", -1, &rc_warn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            } else {
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);
                DrawTextW(mem_dc, L"\u2014 Advanced settings locked to recommended defaults.", -1, &rc_warn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // Card 3 Controls Labels
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);

            int r10_y = c3_y + scale_dpi(76);
            int r11_y = c3_y + scale_dpi(108);
            int r12_y = c3_y + scale_dpi(138);

            RECT rc_lbl_tier = { c3_x + scale_dpi(14), r10_y, c3_x + scale_dpi(105), r10_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Provider Tier:", -1, &rc_lbl_tier, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_lbl_pre = { c3_x + scale_dpi(14), r11_y, c3_x + scale_dpi(64), r11_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Pre-Win:", -1, &rc_lbl_pre, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_lbl_post = { c3_x + scale_dpi(116), r11_y, c3_x + scale_dpi(148), r11_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Post:", -1, &rc_lbl_post, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_lbl_cd = { c3_x + scale_dpi(195), r11_y, c3_x + scale_dpi(254), r11_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Cooldown:", -1, &rc_lbl_cd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_cd_unit = { c3_x + scale_dpi(310), r11_y, c3_x + c3_w, r11_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms", -1, &rc_lbl_cd_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_buf = { c3_x + scale_dpi(14), r12_y, c3_x + scale_dpi(105), r12_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Buffer Capacity:", -1, &rc_lbl_buf, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Card 4 Labels (Correlation Anomaly Thresholds)
            int t1_y = c4_y + scale_dpi(30);
            int t2_y = c4_y + scale_dpi(63);
            int t3_y = c4_y + scale_dpi(96);
            int t4_y = c4_y + scale_dpi(129);
            int t5_y = c4_y + scale_dpi(162);
            int t6_y = c4_y + scale_dpi(195);
            int t7_y = c4_y + scale_dpi(228);
            int t8_y = c4_y + scale_dpi(261);

            RECT rc_lbl_dpc = { c4_x + scale_dpi(14), t1_y, c4_x + scale_dpi(155), t1_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Driver DPC Spike:", -1, &rc_lbl_dpc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_dpc_unit = { c4_x + scale_dpi(222), t1_y, c4_x + c4_w, t1_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"\u00B5s (100 - 50000 \u00B5s)", -1, &rc_lbl_dpc_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_isr = { c4_x + scale_dpi(14), t2_y, c4_x + scale_dpi(155), t2_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Driver ISR Spike:", -1, &rc_lbl_isr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_isr_unit = { c4_x + scale_dpi(222), t2_y, c4_x + c4_w, t2_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"\u00B5s (50 - 50000 \u00B5s)", -1, &rc_lbl_isr_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_disk = { c4_x + scale_dpi(14), t3_y, c4_x + scale_dpi(155), t3_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Disk Latency Stall:", -1, &rc_lbl_disk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_disk_unit = { c4_x + scale_dpi(222), t3_y, c4_x + c4_w, t3_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms (1 - 1000 ms)", -1, &rc_lbl_disk_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_cs = { c4_x + scale_dpi(14), t4_y, c4_x + scale_dpi(155), t4_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"CSwitch Preempt:", -1, &rc_lbl_cs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_cs_unit = { c4_x + scale_dpi(222), t4_y, c4_x + c4_w, t4_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms (1 - 500 ms)", -1, &rc_lbl_cs_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_smi = { c4_x + scale_dpi(14), t5_y, c4_x + scale_dpi(155), t5_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Hardware SMI Gap:", -1, &rc_lbl_smi, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_smi_unit = { c4_x + scale_dpi(222), t5_y, c4_x + c4_w, t5_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms (10.0 - 100.0 ms)", -1, &rc_lbl_smi_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_mem_alloc = { c4_x + scale_dpi(14), t6_y, c4_x + scale_dpi(155), t6_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"VirtualAlloc Stall:", -1, &rc_lbl_mem_alloc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_mem_alloc_unit = { c4_x + scale_dpi(222), t6_y, c4_x + c4_w, t6_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"MB (1 - 1024 MB)", -1, &rc_lbl_mem_alloc_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_mem_trim = { c4_x + scale_dpi(14), t7_y, c4_x + scale_dpi(155), t7_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"WorkingSet Trim:", -1, &rc_lbl_mem_trim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_mem_trim_unit = { c4_x + scale_dpi(222), t7_y, c4_x + c4_w, t7_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"MB (1 - 1024 MB)", -1, &rc_lbl_mem_trim_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_mem_phys = { c4_x + scale_dpi(14), t8_y, c4_x + scale_dpi(155), t8_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"Physical MDL Stall:", -1, &rc_lbl_mem_phys, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_mem_phys_unit = { c4_x + scale_dpi(222), t8_y, c4_x + c4_w, t8_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"\u00B5s (50 - 50000 \u00B5s)", -1, &rc_lbl_mem_phys_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            int t9_y = c4_y + scale_dpi(294);
            int t10_y = c4_y + scale_dpi(327);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_d3d12 = { c4_x + scale_dpi(14), t9_y, c4_x + scale_dpi(155), t9_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"D3D12 PSO Compile:", -1, &rc_lbl_d3d12, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_d3d12_unit = { c4_x + scale_dpi(222), t9_y, c4_x + c4_w, t9_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"ms (1 - 500 ms)", -1, &rc_lbl_d3d12_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            RECT rc_lbl_vram = { c4_x + scale_dpi(14), t10_y, c4_x + scale_dpi(155), t10_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"VRAM Demoted Limit:", -1, &rc_lbl_vram, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_vram_unit = { c4_x + scale_dpi(222), t10_y, c4_x + c4_w, t10_y + scale_dpi(26) };
            DrawTextW(mem_dc, L"MB (1 - 1024 MB)", -1, &rc_lbl_vram_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Rounded frames for edit controls
            auto draw_edit_frame = [&](HWND hEdit) {
                if (!hEdit) return;
                RECT rc_wnd;
                GetWindowRect(hEdit, &rc_wnd);
                POINT pt = { rc_wnd.left, rc_wnd.top };
                ScreenToClient(hwnd, &pt);
                int w = rc_wnd.right - rc_wnd.left;
                int h = rc_wnd.bottom - rc_wnd.top;
                RECT rc_frame = { pt.x - 1, pt.y - 1, pt.x + w + 1, pt.y + h + 1 };
                SelectObject(mem_dc, g_theme.br_input);
                SelectObject(mem_dc, g_theme.pen_input_border);
                RoundRect(mem_dc, rc_frame.left, rc_frame.top, rc_frame.right, rc_frame.bottom, scale_dpi(6), scale_dpi(6));
            };

            draw_edit_frame(state->h_hotkey_edit);
            draw_edit_frame(state->h_edit_auto_save);
            draw_edit_frame(state->h_edit_pre_win);
            draw_edit_frame(state->h_edit_post_win);
            draw_edit_frame(state->h_edit_cooldown);
            draw_edit_frame(state->h_edit_dpc);
            draw_edit_frame(state->h_edit_isr);
            draw_edit_frame(state->h_edit_disk);
            draw_edit_frame(state->h_edit_cswitch);
            draw_edit_frame(state->h_edit_smi);
            draw_edit_frame(state->h_edit_mem_alloc);
            draw_edit_frame(state->h_edit_mem_trim);
            draw_edit_frame(state->h_edit_mem_phys);
            draw_edit_frame(state->h_edit_d3d12_pso);
            draw_edit_frame(state->h_edit_vram_demoted);
            draw_edit_frame(state->h_edit_target_fps);
            draw_edit_frame(state->h_edit_spike_mult);
            draw_edit_frame(state->h_edit_min_delta);

            BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old_br);
            SelectObject(mem_dc, old_pen);
            SelectObject(mem_dc, old_font);
            SelectObject(mem_dc, old_bmp);
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetBkColor(hdcStatic, COLOR_CARD_BG);
            SetTextColor(hdcStatic, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_card;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetBkColor(hdcEdit, COLOR_INPUT_BG);
            if (!IsWindowEnabled(hCtl)) {
                SetTextColor(hdcEdit, COLOR_TEXT_MUTED);
            } else {
                SetTextColor(hdcEdit, COLOR_TEXT_BRIGHT);
            }
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcListBox = (HDC)wParam;
            SetBkColor(hdcListBox, COLOR_INPUT_BG);
            SetTextColor(hdcListBox, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORBTN: {
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)g_theme.br_card;
        }

        case WM_DPICHANGED: {
            UINT new_dpi = LOWORD(wParam);
            update_fonts_for_dpi(new_dpi);
            const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, NULL,
                prcNewWindow->left,
                prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            layout_settings_controls(hwnd, state);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);

            if (wmId == IDC_SET_CHK_ADVANCED) {
                state->advanced_unlocked = (SendMessageW(state->h_chk_advanced, BM_GETCHECK, 0, 0) == BST_CHECKED);
                update_advanced_controls_enablement(state);
                RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                return 0;
            }

            if (wmId == IDC_SET_BTN_BROWSE_AUTO_SAVE) {
                IFileOpenDialog* pFileOpen = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
                if (SUCCEEDED(hr)) {
                    DWORD dwOptions = 0;
                    if (SUCCEEDED(pFileOpen->GetOptions(&dwOptions))) {
                        pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                    }
                    pFileOpen->SetTitle(L"Select Stuttometer Auto-Save Reports Folder");
                    if (SUCCEEDED(pFileOpen->Show(hwnd))) {
                        IShellItem* pItem = nullptr;
                        if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                            PWSTR pszFilePath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                                SetWindowTextW(state->h_edit_auto_save, pszFilePath);
                                SendMessageW(state->h_chk_auto_save, BM_SETCHECK, BST_CHECKED, 0);
                                CoTaskMemFree(pszFilePath);
                            }
                            pItem->Release();
                        }
                    }
                    pFileOpen->Release();
                }
                return 0;
            }

            if (wmId == IDC_SET_BTN_RESET) {
                state->hotkey_vk = VK_F11;
                state->hotkey_mods = MOD_CONTROL;
                state->original_hotkey_vk = VK_F11;
                state->original_hotkey_mods = MOD_CONTROL;
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(state->h_hotkey_edit, hk_str.c_str());

                SendMessageW(state->h_chk_sound, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(state->h_chk_redact, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(state->h_chk_audio, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(state->h_chk_auto_save, BM_SETCHECK, BST_UNCHECKED, 0);
                SetWindowTextW(state->h_edit_auto_save, L"");
                SendMessageW(state->h_combo_tier, CB_SETCURSEL, 0, 0);

                SetWindowTextW(state->h_edit_pre_win, L"250.0");
                SetWindowTextW(state->h_edit_post_win, L"30.0");
                SetWindowTextW(state->h_edit_cooldown, L"1000");
                SendMessageW(state->h_combo_buffer, CB_SETCURSEL, 2, 0);

                SetWindowTextW(state->h_edit_dpc, L"1000");
                SetWindowTextW(state->h_edit_isr, L"500");
                SetWindowTextW(state->h_edit_disk, L"20");
                SetWindowTextW(state->h_edit_cswitch, L"5");
                SetWindowTextW(state->h_edit_smi, L"33.3");
                SetWindowTextW(state->h_edit_mem_alloc, L"16");
                SetWindowTextW(state->h_edit_mem_trim, L"4");
                SetWindowTextW(state->h_edit_mem_phys, L"1000");
                SetWindowTextW(state->h_edit_d3d12_pso, L"5");
                SetWindowTextW(state->h_edit_vram_demoted, L"8");

                SendMessageW(state->h_combo_trig_mode, CB_SETCURSEL, 0, 0);
                SetWindowTextW(state->h_edit_target_fps, L"60");
                SetWindowTextW(state->h_edit_spike_mult, L"2.0");
                SetWindowTextW(state->h_edit_min_delta, L"4.0");
                SendMessageW(state->h_chk_judder, BM_SETCHECK, BST_CHECKED, 0);

                g_settings_config.present_threshold_ms = fps_to_present_threshold_ms(60.0);

                SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
                state->advanced_unlocked = false;
                update_advanced_controls_enablement(state);
                RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                return 0;
            }

            if (wmId == IDC_SET_BTN_CANCEL) {
                dismiss_settings_dialog(hwnd);
                return 0;
            }

            if (wmId == IDC_SET_BTN_SAVE) {
                g_hotkey_vk = state->hotkey_vk;
                g_hotkey_mods = state->hotkey_mods;

                g_sound_cues_enabled = (SendMessageW(state->h_chk_sound, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.redact = (SendMessageW(state->h_chk_redact, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.enable_audio = (SendMessageW(state->h_chk_audio, BM_GETCHECK, 0, 0) == BST_CHECKED);

                bool auto_save_checked = (SendMessageW(state->h_chk_auto_save, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (auto_save_checked) {
                    int len = GetWindowTextLengthW(state->h_edit_auto_save);
                    if (len > 0) {
                        std::wstring dir_w(len + 1, 0);
                        GetWindowTextW(state->h_edit_auto_save, dir_w.data(), len + 1);
                        dir_w.resize(len);
                        while (!dir_w.empty() && (dir_w.back() == L' ' || dir_w.back() == L'\\' || dir_w.back() == L'/')) {
                            dir_w.pop_back();
                        }
                        g_settings_config.output_dir = dir_w;
                    } else {
                        g_settings_config.output_dir.clear();
                    }
                } else {
                    g_settings_config.output_dir.clear();
                }

                int t_idx = static_cast<int>(SendMessageW(state->h_combo_tier, CB_GETCURSEL, 0, 0));
                if (t_idx == 1) g_settings_config.provider_tier = "full";
                else if (t_idx == 2) g_settings_config.provider_tier = "minimal";
                else g_settings_config.provider_tier = "standard";

                wchar_t buf[64]{};

                GetWindowTextW(state->h_edit_pre_win, buf, 64);
                std::wstring w_pre(buf); std::replace(w_pre.begin(), w_pre.end(), L',', L'.');
                double v_pre = _wtof(w_pre.c_str());
                g_settings_config.window_pre_ms = std::clamp(v_pre, 50.0, 1000.0);

                GetWindowTextW(state->h_edit_post_win, buf, 64);
                std::wstring w_post(buf); std::replace(w_post.begin(), w_post.end(), L',', L'.');
                double v_post = _wtof(w_post.c_str());
                g_settings_config.window_post_ms = std::clamp(v_post, 0.0, 200.0);

                GetWindowTextW(state->h_edit_cooldown, buf, 64);
                std::wstring w_cd(buf); std::replace(w_cd.begin(), w_cd.end(), L',', L'.');
                double v_cd = _wtof(w_cd.c_str());
                g_settings_config.cooldown_ms = std::clamp(v_cd, 100.0, 10000.0);

                int b_sel = static_cast<int>(SendMessageW(state->h_combo_buffer, CB_GETCURSEL, 0, 0));
                LRESULT b_data = SendMessageW(state->h_combo_buffer, CB_GETITEMDATA, b_sel, 0);
                if (b_data >= 65536 && b_data <= 1048576) {
                    g_settings_config.buffer_slots = static_cast<uint32_t>(b_data);
                }

                GetWindowTextW(state->h_edit_dpc, buf, 64);
                long v_dpc = _wtol(buf);
                g_settings_config.dpc_threshold_us = static_cast<uint32_t>(std::clamp(v_dpc, 100L, 50000L));

                GetWindowTextW(state->h_edit_isr, buf, 64);
                long v_isr = _wtol(buf);
                g_settings_config.isr_threshold_us = static_cast<uint32_t>(std::clamp(v_isr, 50L, 50000L));

                GetWindowTextW(state->h_edit_disk, buf, 64);
                long v_disk = _wtol(buf);
                g_settings_config.disk_threshold_ms = static_cast<uint32_t>(std::clamp(v_disk, 1L, 1000L));

                GetWindowTextW(state->h_edit_cswitch, buf, 64);
                long v_cs = _wtol(buf);
                g_settings_config.cswitch_preempt_ms = static_cast<uint32_t>(std::clamp(v_cs, 1L, 500L));

                GetWindowTextW(state->h_edit_smi, buf, 64);
                std::wstring w_smi(buf); std::replace(w_smi.begin(), w_smi.end(), L',', L'.');
                double v_smi = _wtof(w_smi.c_str());
                g_settings_config.smi_severity_threshold_ms = std::clamp(v_smi, 10.0, 100.0);

                GetWindowTextW(state->h_edit_mem_alloc, buf, 64);
                long v_mem_alloc = _wtol(buf);
                g_settings_config.mem_alloc_threshold_mb = static_cast<uint32_t>(std::clamp(v_mem_alloc, 1L, 1024L));

                GetWindowTextW(state->h_edit_mem_trim, buf, 64);
                long v_mem_trim = _wtol(buf);
                g_settings_config.mem_trim_threshold_mb = static_cast<uint32_t>(std::clamp(v_mem_trim, 1L, 1024L));

                GetWindowTextW(state->h_edit_mem_phys, buf, 64);
                long v_mem_phys = _wtol(buf);
                g_settings_config.mem_physical_latency_us = static_cast<uint32_t>(std::clamp(v_mem_phys, 50L, 50000L));

                GetWindowTextW(state->h_edit_d3d12_pso, buf, 64);
                long v_d3d12 = _wtol(buf);
                g_settings_config.d3d12_pso_threshold_ms = static_cast<uint32_t>(std::clamp(v_d3d12, 1L, 500L));

                GetWindowTextW(state->h_edit_vram_demoted, buf, 64);
                long v_vram = _wtol(buf);
                g_settings_config.vram_demoted_threshold_mb = static_cast<uint32_t>(std::clamp(v_vram, 1L, 1024L));

                int tm_idx = static_cast<int>(SendMessageW(state->h_combo_trig_mode, CB_GETCURSEL, 0, 0));
                if (tm_idx == 1) g_settings_config.frame_trigger_mode = FrameTriggerMode::DYNAMIC_ONLY;
                else if (tm_idx == 2) g_settings_config.frame_trigger_mode = FrameTriggerMode::STATIC_ONLY;
                else g_settings_config.frame_trigger_mode = FrameTriggerMode::HYBRID;

                GetWindowTextW(state->h_edit_target_fps, buf, 64);
                std::wstring w_fps(buf); std::replace(w_fps.begin(), w_fps.end(), L',', L'.');
                double v_fps = _wtof(w_fps.c_str());
                double clamped_fps = std::clamp(v_fps, 10.0, 500.0);
                g_settings_config.present_threshold_ms = fps_to_present_threshold_ms(clamped_fps);

                GetWindowTextW(state->h_edit_spike_mult, buf, 64);
                std::wstring w_sm(buf); std::replace(w_sm.begin(), w_sm.end(), L',', L'.');
                double v_sm = _wtof(w_sm.c_str());
                g_settings_config.spike_multiplier = std::clamp(v_sm, 1.2, 10.0);

                GetWindowTextW(state->h_edit_min_delta, buf, 64);
                std::wstring w_md(buf); std::replace(w_md.begin(), w_md.end(), L',', L'.');
                double v_md = _wtof(w_md.c_str());
                g_settings_config.min_spike_delta_ms = std::clamp(v_md, 1.0, 50.0);

                g_settings_config.enable_judder_detection = (SendMessageW(state->h_chk_judder, BM_GETCHECK, 0, 0) == BST_CHECKED);

                if (g_hwnd_main && IsWindow(g_hwnd_main)) {
                    UnregisterHotKey(g_hwnd_main, ID_HOTKEY_TOGGLE_CAPTURE);
                    if (!RegisterHotKey(g_hwnd_main, ID_HOTKEY_TOGGLE_CAPTURE, g_hotkey_mods | MOD_NOREPEAT, g_hotkey_vk)) {
                        g_status_text = L"Hotkey Warning: " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another app";
                        append_engine_log(L"[WARN] Hotkey " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another application.");
                        MessageBoxW(hwnd, (L"Warning: The shortcut " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is currently in use by another application. Shortcut toggle capture will not be active until a different key is selected.").c_str(), L"Hotkey Conflict", MB_OK | MB_ICONWARNING);
                    }
                    update_metrics_text();
                    if (g_h_list_stutters) InvalidateRect(g_h_list_stutters, NULL, TRUE);
                    InvalidateRect(g_hwnd_main, NULL, TRUE);
                }

                save_user_settings();
                dismiss_settings_dialog(hwnd);
                return 0;
            }
            break;
        }

        case WM_NCDESTROY: {
            g_h_settings_dlg = nullptr;
            if (state) {
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void ShowSettingsDialog(HWND hParent) {
    if (!hParent) return;

    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(hParent, GWLP_HINSTANCE);

    static bool s_class_registered = false;
    if (!s_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsWindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"StuttometerSettingsWindowClass";
        ATOM atom = RegisterClassExW(&wc);
        s_class_registered = (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    }

    UINT dpi = GetDpiForWindow(hParent);
    if (dpi == 0) dpi = 96;

    int dlg_w = MulDiv(880, dpi, 96);
    int dlg_h = MulDiv(700, dpi, 96);

    RECT rc_parent{};
    GetWindowRect(hParent, &rc_parent);
    int pos_x = rc_parent.left + ((rc_parent.right - rc_parent.left) - dlg_w) / 2;
    int pos_y = rc_parent.top + ((rc_parent.bottom - rc_parent.top) - dlg_h) / 2;

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT rc = { 0, 0, dlg_w, dlg_h };
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);

    HWND hSettingsDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"StuttometerSettingsWindowClass",
        L"Stuttometer Settings",
        dwStyle,
        pos_x, pos_y,
        rc.right - rc.left, rc.bottom - rc.top,
        hParent, NULL, hInstance, NULL
    );

    if (!hSettingsDlg) return;
    g_h_settings_dlg = hSettingsDlg;

    apply_window_dark_titlebar(hSettingsDlg);
    EnableWindow(hParent, FALSE);
    ShowWindow(hSettingsDlg, SW_SHOW);
    UpdateWindow(hSettingsDlg);

    MSG msg{};
    while (IsWindow(hSettingsDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (hParent && !IsWindow(hParent)) {
            DestroyWindow(hSettingsDlg);
            break;
        }
        if (!IsDialogMessageW(hSettingsDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hSettingsDlg)) break;
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    SetFocus(hParent);

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
}


static GuiSessionState g_current_session_state = GuiSessionState::IDLE;
static uint64_t g_capture_start_tick = 0;
static uint64_t g_capture_elapsed_seconds = 0;
static bool g_has_received_data = false;
static uint32_t g_session_stutter_count = 0;
static uint32_t g_session_audio_count = 0;
static std::wstring g_metrics_text = L"Time: 00:00  |  Stutters: 0  |  Audio: 0";

// Pure Tone Synthesizer for High-Precision Audio Cues (Zero disk files, zero latency, in-memory WASAPI/PlaySound)
static std::vector<uint8_t> generate_sine_wav(float freq_hz, float duration_sec, float volume) {
    if (freq_hz <= 0.0f || duration_sec <= 0.0f || volume <= 0.0f) {
        return {};
    }
    volume = std::clamp(volume, 0.0f, 1.0f);

    uint32_t sample_rate = 44100;
    uint32_t total_samples = static_cast<uint32_t>(sample_rate * duration_sec);
    if (total_samples == 0) {
        return {};
    }
    uint32_t data_size = total_samples * sizeof(int16_t);
    uint32_t overall_size = 36 + data_size;

    std::vector<uint8_t> buffer(44 + data_size);
    uint8_t* p = buffer.data();

    // RIFF Header
    std::memcpy(p + 0, "RIFF", 4);
    std::memcpy(p + 4, &overall_size, 4);
    std::memcpy(p + 8, "WAVE", 4);

    // fmt chunk
    std::memcpy(p + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 1;
    uint32_t byte_rate = sample_rate * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    std::memcpy(p + 16, &fmt_size, 4);
    std::memcpy(p + 20, &audio_format, 2);
    std::memcpy(p + 22, &num_channels, 2);
    std::memcpy(p + 24, &sample_rate, 4);
    std::memcpy(p + 28, &byte_rate, 4);
    std::memcpy(p + 32, &block_align, 2);
    std::memcpy(p + 34, &bits_per_sample, 2);

    // data chunk
    std::memcpy(p + 36, "data", 4);
    std::memcpy(p + 40, &data_size, 4);

    int16_t* samples = reinterpret_cast<int16_t*>(p + 44);
    uint32_t fade_samples = std::min(static_cast<uint32_t>(sample_rate * 0.008f), total_samples / 2); // anti-click fade envelope

    constexpr float pi = std::numbers::pi_v<float>;

    for (uint32_t i = 0; i < total_samples; ++i) {
        float gain = 1.0f;
        if (i < fade_samples) {
            gain = static_cast<float>(i) / fade_samples;
        } else if (i > total_samples - fade_samples) {
            gain = static_cast<float>(total_samples - i) / fade_samples;
        }
        float t = static_cast<float>(i) / sample_rate;
        float sample_val = std::sin(2.0f * pi * freq_hz * t);
        float decayed_gain = gain * (1.0f - 0.20f * (static_cast<float>(i) / total_samples));
        int16_t sample_i16 = static_cast<int16_t>(sample_val * decayed_gain * volume * 32767.0f);
        samples[i] = sample_i16;
    }

    return buffer;
}

static std::vector<uint8_t> g_wav_start;
static std::vector<uint8_t> g_wav_stop;
static std::once_flag g_wav_init_once;

// Pure Tone Player via memory PlaySoundW (Non-blocking async GDI thread safe)
static void play_capture_sound(bool starting) {
    std::call_once(g_wav_init_once, []() {
        // Start: Crisp, comfortable continuous tone (1000 Hz, 100ms, 40% volume)
        g_wav_start = generate_sine_wav(1000.0f, 0.10f, 0.40f);
        // Stop: Softer, easily audible completion tone (700 Hz, 90ms, 28% volume)
        g_wav_stop  = generate_sine_wav(700.0f, 0.09f, 0.28f);
    });

    const auto& wav = starting ? g_wav_start : g_wav_stop;
    if (!wav.empty()) {
        // SND_MEMORY treats pszSound as a direct pointer to in-memory WAV byte data (cast via const void*)
        PlaySoundW(reinterpret_cast<LPCWSTR>(static_cast<const void*>(wav.data())), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

// Helper: Format seconds to MM:SS or HH:MM:SS
static std::wstring format_duration(uint64_t total_seconds) {
    uint64_t hrs = total_seconds / 3600;
    uint64_t mins = (total_seconds % 3600) / 60;
    uint64_t secs = total_seconds % 60;
    wchar_t buf[32];
    if (hrs > 0) {
        swprintf_s(buf, L"%02llu:%02llu:%02llu", hrs, mins, secs);
    } else {
        swprintf_s(buf, L"%02llu:%02llu", mins, secs);
    }
    return buf;
}

// Fixed-width geometry helper for the live telemetry badge on the Action Toolbar
static RECT get_telemetry_badge_rect(int client_width) {
    const int act_y = scale_dpi(116);
    const int act_h = scale_dpi(32);
    const int badge_w = g_settings_config.enable_audio ? scale_dpi(270) : scale_dpi(210);
    const int badge_x = client_width - scale_dpi(16) - badge_w;
    return { badge_x, act_y, badge_x + badge_w, act_y + act_h };
}

// Recompute the live telemetry badge text from active session counters
static void update_metrics_text() {
    uint64_t elapsed_secs = (g_capture_start_tick != 0) 
        ? (GetTickCount64() - g_capture_start_tick) / 1000 
        : g_capture_elapsed_seconds;
        
    std::wostringstream oss;
    oss << L"Time: " << format_duration(elapsed_secs)
        << L"  |  Stutters: " << g_session_stutter_count;
    if (g_settings_config.enable_audio) {
        oss << L"  |  Audio: " << g_session_audio_count;
    }
    g_metrics_text = oss.str();
}

// Forward declarations
static void update_inspector(int selected_index);
static void update_clear_button_state();

static void update_fonts_for_dpi(UINT dpi) {
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

    if (g_h_lbl_target) SendMessageW(g_h_lbl_target, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_combo_process) {
        SendMessageW(g_h_combo_process, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (g_h_btn_settings) SendMessageW(g_h_btn_settings, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_start) SendMessageW(g_h_btn_start, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_stop) SendMessageW(g_h_btn_stop, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_clear) SendMessageW(g_h_btn_clear, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_export) SendMessageW(g_h_btn_export, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_copy) SendMessageW(g_h_btn_copy, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_list_stutters) SendMessageW(g_h_list_stutters, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (g_h_edit_inspector) SendMessageW(g_h_edit_inspector, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

    // Propagate to open Settings Dialog controls BEFORE deleting old font objects
    if (g_h_settings_dlg && IsWindow(g_h_settings_dlg)) {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(g_h_settings_dlg, GWLP_USERDATA));
        if (state) {
            if (state->h_hotkey_edit) SendMessageW(state->h_hotkey_edit, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_chk_sound) SendMessageW(state->h_chk_sound, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_chk_redact) SendMessageW(state->h_chk_redact, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_chk_audio) SendMessageW(state->h_chk_audio, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_chk_auto_save) SendMessageW(state->h_chk_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_edit_auto_save) SendMessageW(state->h_edit_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_btn_browse_auto_save) SendMessageW(state->h_btn_browse_auto_save, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_chk_advanced) SendMessageW(state->h_chk_advanced, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_combo_tier) {
                SendMessageW(state->h_combo_tier, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
                SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            }
            if (state->h_edit_pre_win) SendMessageW(state->h_edit_pre_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_post_win) SendMessageW(state->h_edit_post_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_cooldown) SendMessageW(state->h_edit_cooldown, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_combo_buffer) {
                SendMessageW(state->h_combo_buffer, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
                SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            }
            if (state->h_edit_dpc) SendMessageW(state->h_edit_dpc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_isr) SendMessageW(state->h_edit_isr, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_disk) SendMessageW(state->h_edit_disk, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_cswitch) SendMessageW(state->h_edit_cswitch, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_smi) SendMessageW(state->h_edit_smi, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_mem_alloc) SendMessageW(state->h_edit_mem_alloc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_mem_trim) SendMessageW(state->h_edit_mem_trim, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_mem_phys) SendMessageW(state->h_edit_mem_phys, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_d3d12_pso) SendMessageW(state->h_edit_d3d12_pso, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_vram_demoted) SendMessageW(state->h_edit_vram_demoted, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_combo_trig_mode) {
                SendMessageW(state->h_combo_trig_mode, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
                SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            }
            if (state->h_edit_target_fps) SendMessageW(state->h_edit_target_fps, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_spike_mult) SendMessageW(state->h_edit_spike_mult, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_edit_min_delta) SendMessageW(state->h_edit_min_delta, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_chk_judder) SendMessageW(state->h_chk_judder, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            if (state->h_btn_reset) SendMessageW(state->h_btn_reset, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_btn_cancel) SendMessageW(state->h_btn_cancel, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            if (state->h_btn_save) SendMessageW(state->h_btn_save, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
        }
    }

    if (old_title) DeleteObject(old_title);
    if (old_ui) DeleteObject(old_ui);
    if (old_ui_bold) DeleteObject(old_ui_bold);
    if (old_ui_sm_bold) DeleteObject(old_ui_sm_bold);
    if (old_mono) DeleteObject(old_mono);
}

// Subclassed Edit Control Procedure: Keeps text centered with vertical padding
static LRESULT CALLBACK EditCenteredSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, EditCenteredSubclassProc, uIdSubclass);
            break;
        case WM_SETFONT: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            RECT rc_client;
            GetClientRect(hwnd, &rc_client);
            int top_pad = (rc_client.bottom > scale_dpi(20)) ? scale_dpi(4) : scale_dpi(2);
            RECT rc = { scale_dpi(2), top_pad, rc_client.right - scale_dpi(2), rc_client.bottom };
            SendMessageW(hwnd, EM_SETRECTNP, 0, (LPARAM)&rc);
            return res;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int top_pad = (h > scale_dpi(20)) ? scale_dpi(4) : scale_dpi(2);
            RECT rc = { scale_dpi(2), top_pad, w - scale_dpi(2), h };
            SendMessageW(hwnd, EM_SETRECTNP, 0, (LPARAM)&rc);
            break;
        }
        case WM_CHAR: {
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                SetFocus(GetParent(hwnd));
                return 0;
            }
            break;
        }
        case WM_KILLFOCUS: {
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0); // Clear text selection when focus is lost
            break;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}
// Subclassed Report Inspector Edit Control: Hides blinking caret for polished static display
static LRESULT CALLBACK ReportInspectorSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
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

static LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
    switch (uMsg) {
        case WM_NCDESTROY:
            RemovePropW(hwnd, L"Hovered");
            RemovePropW(hwnd, L"BtnStyle");
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

// Subclassed Header Procedure for Dark ListView Header
static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
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
            SelectObject(hdc, g_font_ui_bold);

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
                hdi.mask = HDI_TEXT;
                hdi.pszText = text;
                hdi.cchTextMax = 128;
                Header_GetItem(hwnd, i, &hdi);

                RECT text_rc = item_rc;
                text_rc.left += scale_dpi(8);
                text_rc.right -= scale_dpi(8);
                DrawTextW(hdc, text, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            SelectObject(hdc, old_pen);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclassed ListView for Custom Empty State Rendering
static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
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

                    // Watermark Subtitle
                    SelectObject(hdc, g_font_ui);
                    SetTextColor(hdc, COLOR_TEXT_DIM);
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

// Subclassed ComboBox Procedure
static LRESULT CALLBACK DarkComboSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/) {
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

// Custom Draw Owner-Drawn Buttons with Complete State Matrix (Zero per-frame allocations)
static void draw_custom_button(LPDRAWITEMSTRUCT pdis) {
    HDC hdc = pdis->hDC;
    RECT rc = pdis->rcItem;
    int ctl_id = static_cast<int>(pdis->CtlID);

    // Pre-fill bounding rectangle with parent container background to eliminate black corner edges
    HBRUSH bg_parent = g_theme.br_bg;
    if (ctl_id == IDC_BTN_COPY_JSON || ctl_id == IDC_BTN_EXPORT_JSON) {
        bg_parent = g_theme.br_card;
    }
    FillRect(hdc, &rc, bg_parent);

    bool is_disabled = (pdis->itemState & ODS_DISABLED) != 0;
    bool is_pressed = (pdis->itemState & ODS_SELECTED) != 0;
    bool is_hovered = (GetPropW(pdis->hwndItem, L"Hovered") != nullptr) && !is_disabled;

    BtnStyle style = static_cast<BtnStyle>(reinterpret_cast<INT_PTR>(GetPropW(pdis->hwndItem, L"BtnStyle")));
    if (style == static_cast<BtnStyle>(0)) {
        style = BtnStyle::SecondarySlate;
    }

    HBRUSH br = g_theme.br_btn_slate;
    HPEN pen = g_theme.pen_btn_slate;
    COLORREF text_color = COLOR_TEXT_BRIGHT;

    if (is_disabled) {
        br = g_theme.br_btn_disabled;
        pen = g_theme.pen_btn_disabled;
        text_color = COLOR_TEXT_DIM;
    } else {
        switch (style) {
            case BtnStyle::PrimaryEmerald:
                if (is_pressed) {
                    br = g_theme.br_btn_emerald_pressed;
                    pen = g_theme.pen_btn_emerald_pressed;
                } else if (is_hovered) {
                    br = g_theme.br_btn_emerald_hover;
                    pen = g_theme.pen_btn_emerald_hover;
                } else {
                    br = g_theme.br_btn_emerald;
                    pen = g_theme.pen_btn_emerald;
                }
                text_color = RGB(255, 255, 255);
                break;

            case BtnStyle::DangerRed:
                if (is_pressed) {
                    br = g_theme.br_btn_danger_pressed;
                    pen = g_theme.pen_btn_danger_pressed;
                } else if (is_hovered) {
                    br = g_theme.br_btn_danger_hover;
                    pen = g_theme.pen_btn_danger_hover;
                } else {
                    br = g_theme.br_btn_danger;
                    pen = g_theme.pen_btn_danger;
                }
                text_color = RGB(255, 255, 255);
                break;

            case BtnStyle::QuickAction:
                if (is_pressed) {
                    br = g_theme.br_btn_quick_pressed;
                    pen = g_theme.pen_btn_quick_pressed;
                } else if (is_hovered) {
                    br = g_theme.br_btn_quick_hover;
                    pen = g_theme.pen_btn_quick_hover;
                } else {
                    br = g_theme.br_btn_quick;
                    pen = g_theme.pen_btn_quick;
                }
                text_color = COLOR_TEXT_PRI;
                break;

            case BtnStyle::SecondarySlate:
            default:
                if (is_pressed) {
                    br = g_theme.br_btn_slate_pressed;
                    pen = g_theme.pen_btn_slate_pressed;
                } else if (is_hovered) {
                    br = g_theme.br_btn_slate_hover;
                    pen = g_theme.pen_btn_slate_hover;
                } else {
                    br = g_theme.br_btn_slate;
                    pen = g_theme.pen_btn_slate;
                }
                text_color = COLOR_TEXT_PRI;
                break;
        }
    }

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

    RECT text_rc = { start_x, rc.top + offset_y, start_x + text_w, rc.bottom + offset_y };
    DrawTextW(hdc, btn_text, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, old_font);
}

// Update UI Inspector with details of selected stutter report
static void update_inspector(int selected_index) {
    if (selected_index < 0 || selected_index >= static_cast<int>(g_stutters.size())) {
        SCROLLINFO si{ sizeof(SCROLLINFO), SIF_POS | SIF_RANGE };
        GetScrollInfo(g_h_edit_inspector, SB_VERT, &si);
        int prev_pos = si.nPos;

        std::wostringstream oss;
        oss << L"\r\n  No stutter event selected.\r\n\r\n";
        oss << L"  Select an entry from the list above to inspect root-cause analysis, confidence factors, and supporting evidence timeline.\r\n";
        if (!g_engine_logs.empty()) {
            oss << L"\r\n  ------------------------------------------------------------------------------\r\n";
            oss << L"   ENGINE LOGS & DIAGNOSTIC MESSAGES:\r\n";
            oss << L"  ------------------------------------------------------------------------------\r\n";
            for (const auto& log : g_engine_logs) {
                oss << L"   " << log << L"\r\n";
            }
        }
        SetWindowTextW(g_h_edit_inspector, oss.str().c_str());
        if (prev_pos > 0) {
            SendMessageW(g_h_edit_inspector, EM_LINESCROLL, 0, prev_pos);
        }
        EnableWindow(g_h_btn_export, FALSE);
        EnableWindow(g_h_btn_copy, FALSE);
        return;
    }

    const auto& item = g_stutters[selected_index];
    const auto& r = *item.report;

    std::wostringstream oss;
    oss << L"\r\n";
    oss << L"  ==============================================================================\r\n";
    oss << L"   [DIAGNOSTIC REPORT INSPECTOR] Event #" << item.id << L" | " << utf8_to_wstring(r.timestamp_utc) << L"\r\n";
    oss << L"  ==============================================================================\r\n\r\n";

    if (r.redacted) {
        oss << L"  TARGET PROCESS:      [REDACTED] (PID: 0, TID: 0)\r\n";
    } else {
        oss << L"  TARGET PROCESS:      " << utf8_to_wstring(r.target_process) << L" (PID: " << r.trigger.target_pid << L", TID: " << r.trigger.target_tid << L")\r\n";
    }
    oss << L"  TRIGGER CAUSE:       " << utf8_to_wstring(std::string(trigger_source_to_string(r.trigger.source)))
        << L" (" << utf8_to_wstring(std::string(trigger_reason_to_string(r.trigger.reason))) << L")\r\n";
    if (r.trigger.baseline_avg_ms > 0.0) {
        oss << L"  BASELINE DELIVERY:   " << std::fixed << std::setprecision(1) << r.trigger.baseline_fps << L" FPS (" 
            << r.trigger.baseline_avg_ms << L" ms/frame, " << r.trigger.spike_ratio << L"x spike)\r\n";
    }
    const double eff_thresh = r.present_threshold_ms + std::max(0.5, r.present_threshold_ms * 0.05);
    oss << L"  STUTTER DURATION:    " << std::fixed << std::setprecision(2) << r.trigger.duration_ms << L" ms (Nominal: " << r.present_threshold_ms << L" ms, Effective: " << eff_thresh << L" ms)\r\n";
    oss << L"  CAPTURE WINDOW:      " << r.window_pre_ms << L" ms pre-trigger / " << r.window_post_ms << L" ms post-trigger\r\n";
    oss << L"  PROVIDER TIER:       " << utf8_to_wstring(r.provider_tier) << (r.redacted ? L" [REDACTED]" : L"") << L"\r\n\r\n";

    if (r.diagnoses.empty()) {
        oss << L"  ------------------------------------------------------------------------------\r\n";
        oss << L"   ROOT-CAUSE DIAGNOSIS: No anomalies detected exceeding thresholds.\r\n";
        oss << L"  ------------------------------------------------------------------------------\r\n";
    } else {
        oss << L"  ------------------------------------------------------------------------------\r\n";
        oss << L"   RANKED ROOT-CAUSE DIAGNOSES:\r\n";
        oss << L"  ------------------------------------------------------------------------------\r\n";

        for (const auto& diag : r.diagnoses) {
            oss << L"   Rank #" << diag.rank << L": " << utf8_to_wstring(diag.hypothesis) << L"\r\n";
            oss << L"   Confidence: " << std::fixed << std::setprecision(1) << (diag.confidence * 100.0) << L"%\r\n";
            oss << L"   Summary:    " << utf8_to_wstring(diag.summary) << L"\r\n\r\n";

            if (!diag.evidence.empty()) {
                oss << L"     Supporting Evidence Timeline (" << diag.evidence.size() << L" items):\r\n";
                for (size_t i = 0; i < diag.evidence.size(); ++i) {
                    const auto& ev = diag.evidence[i];
                    oss << L"     [" << (i + 1) << L"] " << utf8_to_wstring(ev.event_type) << L" | Module: " << utf8_to_wstring(ev.driver_module);
                    if (ev.duration_us > 0) {
                        oss << L" | Duration: " << std::fixed << std::setprecision(2) << (ev.duration_us / 1000.0) << L" ms";
                    }
                    oss << L" | Core: " << static_cast<int>(ev.cpu_core);
                    oss << L" | Offset: " << std::showpos << std::fixed << std::setprecision(2) << ev.offset_from_trigger_ms << L" ms" << std::noshowpos;
                    if (!ev.extra_info.empty()) {
                        oss << L" | " << utf8_to_wstring(ev.extra_info);
                    }
                    oss << L"\r\n";
                }
                oss << L"\r\n";
            }
        }
    }

    oss << L"  ------------------------------------------------------------------------------\r\n";
    oss << L"   FLIGHT RECORDER METRICS:\r\n";
    oss << L"  ------------------------------------------------------------------------------\r\n";
    oss << L"   Total Events: " << r.total_events << L"\r\n";
    oss << L"   DPC: " << r.event_counts.dpc << L" | ISR: " << r.event_counts.isr << L" | Disk I/O: " << r.event_counts.disk << L" | CSwitch: " << r.event_counts.cswitch << L"\r\n";
    oss << L"   DXGI Present: " << r.event_counts.dxgi << L" | Audio: " << r.event_counts.audio << L"\r\n";
    oss << L"   Buffer Drops: " << r.dropped_events << L" | Upstream Lost: " << r.etw_events_lost << L"\r\n";
    oss << L"   Evictions: " << r.unpaired_evictions << L" | Ins Failures: " << r.insertion_failures << L"\r\n";

    SetWindowTextW(g_h_edit_inspector, oss.str().c_str());
    EnableWindow(g_h_btn_export, TRUE);
    EnableWindow(g_h_btn_copy, TRUE);
}

// Copy JSON string to clipboard
static void copy_selected_report_json(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    JsonReporter reporter;
    bool redact = g_settings_config.redact;
    std::string json = reporter.to_json_string(*item.report, redact, 2);
    std::wstring wjson = utf8_to_wstring(json);

    if (!OpenClipboard(hwnd)) {
        MessageBoxW(hwnd, L"Failed to open clipboard.", L"Clipboard Error", MB_OK | MB_ICONERROR);
        return;
    }

    EmptyClipboard();
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (wjson.size() + 1) * sizeof(wchar_t));
    if (hGlob) {
        wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hGlob));
        if (pMem) {
            wcscpy_s(pMem, wjson.size() + 1, wjson.c_str());
            GlobalUnlock(hGlob);
            if (SetClipboardData(CF_UNICODETEXT, hGlob)) {
                CloseClipboard();
                MessageBoxW(hwnd, L"JSON report copied to clipboard successfully!", L"Copied", MB_OK | MB_ICONINFORMATION);
                return;
            }
        }
        GlobalFree(hGlob);
    }
    CloseClipboard();
    MessageBoxW(hwnd, L"Failed to copy JSON report to clipboard.", L"Clipboard Error", MB_OK | MB_ICONERROR);
}

// Export JSON report to file
static void export_selected_report_json(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    JsonReporter reporter;
    bool redact = g_settings_config.redact;

    wchar_t filename_buf[MAX_PATH] = L"stutto_report.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn)) {
        std::string path = wstring_to_utf8(filename_buf);
        if (reporter.save_to_file(*item.report, path, redact)) {
            MessageBoxW(hwnd, L"Report exported successfully!", L"Export Complete", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd, L"Failed to save JSON report file.", L"Export Error", MB_OK | MB_ICONERROR);
        }
    }
}

// Insert new report item into UI
static void handle_new_report(std::unique_ptr<DiagnosticReport> report) {
    if (!report) return;

    StutterRecord rec{};
    rec.id = g_next_stutter_id++;
    rec.timestamp = report->timestamp_utc;
    rec.process_name = report->target_process.empty() ? ("PID:" + std::to_string(report->trigger.target_pid)) : report->target_process;
    std::string trig_str = std::string(trigger_source_to_string(report->trigger.source));
    if (report->trigger.reason != TriggerReason::NONE && report->trigger.reason != TriggerReason::STATIC_THRESHOLD) {
        trig_str += " (" + std::string(trigger_reason_to_string(report->trigger.reason)) + ")";
    }
    rec.trigger_reason = trig_str;
    rec.duration_ms = report->trigger.duration_ms;

    if (!report->diagnoses.empty()) {
        rec.top_hypothesis = report->diagnoses[0].hypothesis;
        rec.confidence = report->diagnoses[0].confidence;
    } else {
        rec.top_hypothesis = "none";
        rec.confidence = 0.0;
    }

    if (report->trigger.source == TriggerSource::AUDIO_GLITCH) {
        g_session_audio_count++;
    } else {
        g_session_stutter_count++;
    }

    rec.report = std::move(report);
    g_stutters.push_back(std::move(rec));

    constexpr size_t MAX_STUTTER_HISTORY = 500;
    if (g_stutters.size() > MAX_STUTTER_HISTORY) {
        ListView_DeleteItem(g_h_list_stutters, 0);
        g_stutters.pop_front();
        if (g_selected_stutter_index > 0) {
            --g_selected_stutter_index;
            if (g_h_list_stutters && IsWindow(g_h_list_stutters)) {
                ListView_SetItemState(g_h_list_stutters, g_selected_stutter_index,
                                      LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        } else if (g_selected_stutter_index == 0) {
            g_selected_stutter_index = -1;
            update_inspector(-1);
        }
    }

    int new_index = static_cast<int>(g_stutters.size() - 1);
    g_has_received_data = true;

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = new_index;
    lvi.iSubItem = 0;
    std::wstring id_str = std::to_wstring(g_stutters[new_index].id);
    lvi.pszText = const_cast<LPWSTR>(id_str.c_str());
    lvi.lParam = static_cast<LPARAM>(g_stutters[new_index].id);

    ListView_InsertItem(g_h_list_stutters, &lvi);

    std::wstring w_time = utf8_to_wstring(g_stutters[new_index].timestamp);
    std::wstring w_proc = utf8_to_wstring(g_stutters[new_index].process_name);
    std::wstring w_trig = utf8_to_wstring(g_stutters[new_index].trigger_reason);

    std::wstring w_dur;
    if (g_stutters[new_index].report && g_stutters[new_index].report->trigger.source == TriggerSource::AUDIO_GLITCH) {
        w_dur = L"Glitch (x" + std::to_wstring(g_stutters[new_index].report->trigger.glitch_count) + L")";
    } else {
        std::wostringstream oss_dur;
        oss_dur << std::fixed << std::setprecision(1) << g_stutters[new_index].duration_ms << L" ms";
        w_dur = oss_dur.str();
    }

    std::wstring w_diag = utf8_to_wstring(g_stutters[new_index].top_hypothesis);
    std::wstring w_conf = std::to_wstring(static_cast<int>(g_stutters[new_index].confidence * 100.0 + 0.5)) + L"%";

    ListView_SetItemText(g_h_list_stutters, new_index, 1, const_cast<LPWSTR>(w_time.c_str()));
    ListView_SetItemText(g_h_list_stutters, new_index, 2, const_cast<LPWSTR>(w_proc.c_str()));
    ListView_SetItemText(g_h_list_stutters, new_index, 3, const_cast<LPWSTR>(w_trig.c_str()));
    ListView_SetItemText(g_h_list_stutters, new_index, 4, const_cast<LPWSTR>(w_dur.c_str()));
    ListView_SetItemText(g_h_list_stutters, new_index, 5, const_cast<LPWSTR>(w_diag.c_str()));
    ListView_SetItemText(g_h_list_stutters, new_index, 6, const_cast<LPWSTR>(w_conf.c_str()));

    const bool should_follow = (g_selected_stutter_index < 0 || g_selected_stutter_index == new_index - 1);
    if (should_follow) {
        ListView_SetItemState(g_h_list_stutters, new_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_h_list_stutters, new_index, FALSE);
        g_selected_stutter_index = new_index;
        update_inspector(new_index);
    }
    update_clear_button_state();
    update_metrics_text();

    g_status_text = L"Stutter Captured (" + w_proc + L")";
    RECT client_rc;
    GetClientRect(g_hwnd_main, &client_rc);
    int pill_w = scale_dpi(280);
    int pill_left = (client_rc.right - pill_w) / 2;
    int pill_top = scale_dpi(9);
    int pill_bottom = pill_top + scale_dpi(30);
    RECT rc_pill = { pill_left - scale_dpi(2), pill_top - scale_dpi(2), pill_left + pill_w + scale_dpi(2), pill_bottom + scale_dpi(2) };
    InvalidateRect(g_hwnd_main, &rc_pill, FALSE);

    RECT rc_b = get_telemetry_badge_rect(client_rc.right - client_rc.left);
    RECT rc_inv = { rc_b.left - scale_dpi(2), rc_b.top - scale_dpi(2), rc_b.right + scale_dpi(2), rc_b.bottom + scale_dpi(2) };
    InvalidateRect(g_hwnd_main, &rc_inv, FALSE);
}

static void update_clear_button_state() {
    bool is_idle = (g_current_session_state == GuiSessionState::IDLE);
    bool has_data = g_has_received_data || !g_stutters.empty() || !g_engine_logs.empty();
    EnableWindow(g_h_btn_clear, (is_idle && has_data) ? TRUE : FALSE);
}

// Clear history with explicit ListView invalidation
static void clear_stutter_history() {
    ListView_DeleteAllItems(g_h_list_stutters);
    g_stutters.clear();
    g_engine_logs.clear();
    g_has_received_data = false;
    g_selected_stutter_index = -1;
    g_session_stutter_count = 0;
    g_session_audio_count = 0;
    g_capture_start_tick = 0;
    g_capture_elapsed_seconds = 0;
    update_metrics_text();
    update_inspector(-1);
    update_clear_button_state();
    InvalidateRect(g_h_list_stutters, NULL, TRUE);
    InvalidateRect(g_hwnd_main, NULL, FALSE);
}

// Populate Process Combobox
static void handle_processes_updated(std::unique_ptr<ProcessList> procs) {
    if (!procs) return;

    if (*procs == g_cached_processes) {
        return; // List is identical, zero UI churn or listbox flicker
    }

    std::wstring selected_proc_name;
    int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
    if (cur_sel > 0) {
        LRESULT data = SendMessageW(g_h_combo_process, CB_GETITEMDATA, cur_sel, 0);
        if (data >= 0 && data < static_cast<LRESULT>(g_cached_processes.size())) {
            selected_proc_name = g_cached_processes[data].name;
        }
    }

    g_cached_processes = *procs;

    SendMessageW(g_h_combo_process, CB_RESETCONTENT, 0, 0);
    int all_idx = static_cast<int>(SendMessageW(g_h_combo_process, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All Processes (System-Wide)")));
    SendMessageW(g_h_combo_process, CB_SETITEMDATA, all_idx, static_cast<LPARAM>(-1));

    std::wstring last_target_w = utf8_to_wstring(g_settings_last_target_process);
    int found_idx = CB_ERR;
    for (size_t i = 0; i < g_cached_processes.size(); ++i) {
        const auto& p = g_cached_processes[i];
        std::wstring item = p.name;
        if (!p.window_title.empty()) {
            item += L" (" + p.window_title.substr(0, 24) + L")";
        }
        int item_idx = static_cast<int>(SendMessageW(g_h_combo_process, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str())));
        SendMessageW(g_h_combo_process, CB_SETITEMDATA, item_idx, static_cast<LPARAM>(i));

        if (!selected_proc_name.empty() && _wcsicmp(p.name.c_str(), selected_proc_name.c_str()) == 0) {
            found_idx = item_idx;
        } else if (found_idx == CB_ERR && !last_target_w.empty() && _wcsicmp(p.name.c_str(), last_target_w.c_str()) == 0) {
            found_idx = item_idx;
        }
    }

    if (found_idx != CB_ERR) {
        SendMessageW(g_h_combo_process, CB_SETCURSEL, static_cast<WPARAM>(found_idx), 0);
        if (g_controller && g_controller->is_capturing() && !g_settings_last_target_process.empty()) {
            auto cfg = read_gui_config();
            g_controller->update_target_filter(cfg.target_pid, cfg.target_process_name);
        }
    } else {
        SendMessageW(g_h_combo_process, CB_SETCURSEL, 0, 0);
    }
    g_settings_last_target_process.clear();
    InvalidateRect(g_h_combo_process, NULL, TRUE);

    // If the dropdown list is currently dropped open, dynamically resize the listbox window
    // so no black unpainted gap or incorrect scroll height remains
    if (SendMessageW(g_h_combo_process, CB_GETDROPPEDSTATE, 0, 0)) {
        COMBOBOXINFO cbi = { sizeof(COMBOBOXINFO) };
        if (GetComboBoxInfo(g_h_combo_process, &cbi) && cbi.hwndList && IsWindow(cbi.hwndList)) {
            RECT rc_list;
            GetWindowRect(cbi.hwndList, &rc_list);
            int item_h = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETITEMHEIGHT, 0, 0));
            if (item_h <= 0) item_h = scale_dpi(22);
            int total_items = static_cast<int>(g_cached_processes.size()) + 1; // +1 for "All Processes"
            int max_visible = 10;
            int visible_items = (total_items > max_visible) ? max_visible : total_items;
            int new_h = visible_items * item_h + scale_dpi(2);
            SetWindowPos(cbi.hwndList, NULL, 0, 0, rc_list.right - rc_list.left, new_h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(cbi.hwndList, NULL, TRUE);
        }
    }
}

// Read current user config
static GuiConfig read_gui_config() {
    GuiConfig cfg = g_settings_config;

    if (g_h_combo_process && IsWindow(g_h_combo_process)) {
        int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
        if (cur_sel > 0) {
            LRESULT data = SendMessageW(g_h_combo_process, CB_GETITEMDATA, cur_sel, 0);
            if (data >= 0 && data < static_cast<LRESULT>(g_cached_processes.size())) {
                cfg.target_pid = g_cached_processes[data].pid;
                cfg.target_process_name = wstring_to_utf8(g_cached_processes[data].name);
            }
        } else {
            cfg.target_pid = 0;
            cfg.target_process_name.clear();
        }
    }

    return cfg;
}

// Update session state
static void update_session_ui_state(GuiSessionState state) {
    GuiSessionState prev_state = g_current_session_state;
    g_current_session_state = state;

    // Play non-blocking synthesized audible confirmation cue if sound cues are enabled
    if (g_sound_cues_enabled) {
        if ((state == GuiSessionState::RUNNING || state == GuiSessionState::DEGRADED_USER_ONLY || state == GuiSessionState::DEGRADED_KERNEL_ONLY) &&
            (prev_state == GuiSessionState::IDLE || prev_state == GuiSessionState::STARTING)) {
            play_capture_sound(true);
        } else if (state == GuiSessionState::IDLE && prev_state != GuiSessionState::IDLE) {
            play_capture_sound(false);
        }
    }

    if (state == GuiSessionState::STARTING || state == GuiSessionState::RUNNING || state == GuiSessionState::DEGRADED_USER_ONLY || state == GuiSessionState::DEGRADED_KERNEL_ONLY) {
        if (g_capture_start_tick == 0) {
            g_capture_start_tick = GetTickCount64();
            g_capture_elapsed_seconds = 0;
            g_session_stutter_count = 0;
            g_session_audio_count = 0;
            update_metrics_text();
        }
    } else if (state == GuiSessionState::IDLE || state == GuiSessionState::STOPPING) {
        if (g_capture_start_tick != 0) {
            g_capture_elapsed_seconds = (GetTickCount64() - g_capture_start_tick) / 1000;
            g_capture_start_tick = 0;
            update_metrics_text();
        }
    }

    switch (state) {
        case GuiSessionState::IDLE:
            EnableWindow(g_h_btn_start, TRUE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, TRUE);
            EnableWindow(g_h_combo_process, TRUE);
            g_status_text = L"IDLE (Ready to monitor)";
            break;

        case GuiSessionState::STARTING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"Starting Trace Sessions...";
            break;

        case GuiSessionState::RUNNING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"CAPTURING (Kernel + User Active)";
            break;

        case GuiSessionState::DEGRADED_USER_ONLY:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"DEGRADED (User-Only DXGI/Audio)";
            break;

        case GuiSessionState::DEGRADED_KERNEL_ONLY:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"DEGRADED (Kernel-Only DPC/CSwitch)";
            break;

        case GuiSessionState::STOPPING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"Stopping Trace Sessions...";
            break;
    }
    update_clear_button_state();
    if (g_h_list_stutters && ListView_GetItemCount(g_h_list_stutters) == 0) {
        InvalidateRect(g_h_list_stutters, NULL, TRUE);
    }
    InvalidateRect(g_hwnd_main, NULL, FALSE);
}

// Auto-fit ListView Column Widths
static void auto_fit_listview_columns(HWND hList, int list_width) {
    if (!hList || list_width <= 0) return;

    const int col_id_w     = scale_dpi(42);
    const int col_time_w   = scale_dpi(140);
    const int col_proc_w   = scale_dpi(145);
    const int col_trig_w   = scale_dpi(140);
    const int col_dur_w    = scale_dpi(120);
    const int col_conf_w   = scale_dpi(90);

    int total_fixed = col_id_w + col_time_w + col_proc_w + col_trig_w + col_dur_w + col_conf_w;
    int scrollbar_w = GetSystemMetrics(SM_CXVSCROLL);
    int col_diag_w = std::max(scale_dpi(200), list_width - total_fixed - scrollbar_w - scale_dpi(4));

    ListView_SetColumnWidth(hList, 0, col_id_w);
    ListView_SetColumnWidth(hList, 1, col_time_w);
    ListView_SetColumnWidth(hList, 2, col_proc_w);
    ListView_SetColumnWidth(hList, 3, col_trig_w);
    ListView_SetColumnWidth(hList, 4, col_dur_w);
    ListView_SetColumnWidth(hList, 5, col_diag_w);
    ListView_SetColumnWidth(hList, 6, col_conf_w);
}

// Layout child controls with strict DPI scaling and dynamic sizing
static void layout_controls(HWND /*hwnd*/, int width, int height) {
    int min_w = scale_dpi(1000);
    int min_h = scale_dpi(640);
    if (width < min_w) width = min_w;
    if (height < min_h) height = min_h;

    int margin = scale_dpi(16);

    // 0. Header Settings Button (Y: 9, Height: 30)
    int elem_h = scale_dpi(30);
    int elem_y = scale_dpi(9);
    int btn_set_w = scale_dpi(105);
    int btn_set_x = width - margin - btn_set_w;
    MoveWindow(g_h_btn_settings, btn_set_x, elem_y, btn_set_w, elem_h, TRUE);

    // 1. Configuration Card (Y: 54, Height: 52)
    const int card_y = scale_dpi(54);
    const int card_h = scale_dpi(52);
    const int ctrl_h = scale_dpi(26);
    const int ctrl_y = card_y + (card_h - ctrl_h) / 2; // Precise vertical center (Y: 67)
    const int lbl_y = ctrl_y;
    const int lbl_h = ctrl_h;
    const int gap_lbl_box = scale_dpi(8);

    // Target Process controls (Balanced dropdown width)
    const int col1_x = margin + scale_dpi(12);
    const int lbl_target_w = scale_dpi(48);
    const int combo_proc_w = scale_dpi(235);

    MoveWindow(g_h_lbl_target, col1_x, lbl_y, lbl_target_w, lbl_h, TRUE);
    MoveWindow(g_h_combo_process, col1_x + lbl_target_w + gap_lbl_box, ctrl_y + scale_dpi(2), combo_proc_w, scale_dpi(250), TRUE);

    // 2. Action Toolbar (Y: 116, Height: 32)
    int act_y = scale_dpi(116);
    int act_h = scale_dpi(32);
    int bx = margin;
    const int btn_w = scale_dpi(88);
    const int btn_gap = scale_dpi(8);

    MoveWindow(g_h_btn_start, bx, act_y, btn_w, act_h, TRUE);
    bx += btn_w + btn_gap;
    MoveWindow(g_h_btn_stop, bx, act_y, btn_w, act_h, TRUE);
    bx += btn_w + btn_gap;
    MoveWindow(g_h_btn_clear, bx, act_y, scale_dpi(65), act_h, TRUE);

    // 3. Stutter Events Table (ListView) and Diagnostic Inspector Card
    int content_top = act_y + act_h + scale_dpi(10);
    int content_bottom = height - margin;
    int total_avail = content_bottom - content_top;

    int min_table_h = scale_dpi(110);
    int min_insp_h = scale_dpi(150);

    int list_w = width - margin * 2;
    int list_h = (total_avail * 44) / 100;
    if (list_h < min_table_h) list_h = min_table_h;

    MoveWindow(g_h_list_stutters, margin, content_top, list_w, list_h, TRUE);
    auto_fit_listview_columns(g_h_list_stutters, list_w);

    // Inspector Card Section (with Integrated Header)
    int insp_card_y = content_top + list_h + scale_dpi(10);
    int insp_card_h = content_bottom - insp_card_y;
    if (insp_card_h < min_insp_h) insp_card_h = min_insp_h;

    int insp_hdr_h = scale_dpi(34);
    int btn_quick_w = scale_dpi(92);
    int btn_quick_h = scale_dpi(24);
    int btn_quick_y = insp_card_y + (insp_hdr_h - btn_quick_h) / 2;

    // Position Copy and Export buttons on the top-right of the Inspector Header
    int btn_exp_x = margin + list_w - scale_dpi(8) - btn_quick_w;
    int btn_cpy_x = btn_exp_x - scale_dpi(8) - btn_quick_w;
    MoveWindow(g_h_btn_export, btn_exp_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);
    MoveWindow(g_h_btn_copy, btn_cpy_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);

    // Inset the multi-line edit control inside the Inspector card
    int edit_margin = scale_dpi(8);
    int insp_edit_x = margin + edit_margin;
    int insp_edit_y = insp_card_y + insp_hdr_h;
    int insp_edit_w = list_w - edit_margin * 2;
    int insp_edit_h = insp_card_h - insp_hdr_h - edit_margin;

    MoveWindow(g_h_edit_inspector, insp_edit_x, insp_edit_y, insp_edit_w, insp_edit_h, TRUE);
}

// Window Procedure
LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        // INTENTIONAL DESIGN DECISION: Fixed Window Dimensions
        // The GUI uses a carefully crafted, high-density dashboard layout. Window resizing is 
        // deliberately locked to the exact DPI-scaled base size (1020x660 @ 96 DPI) using ptMinTrackSize 
        // and ptMaxTrackSize to prevent layout distortion and maintain pixel-perfect visual aesthetics.
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
            UINT dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;
            int base_w = MulDiv(1020, dpi, 96);
            int base_h = MulDiv(660, dpi, 96);
            RECT rc = { 0, 0, base_w, base_h };
            DWORD dwStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
            DWORD dwExStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle);
            int win_w = rc.right - rc.left;
            int win_h = rc.bottom - rc.top;
            mmi->ptMinTrackSize.x = win_w;
            mmi->ptMinTrackSize.y = win_h;
            mmi->ptMaxTrackSize.x = win_w;
            mmi->ptMaxTrackSize.y = win_h;
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (GetFocus() != hwnd) {
                SetFocus(hwnd);
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1; // Double buffering handled cleanly in WM_PAINT

        case WM_CREATE: {
            g_hwnd_main = hwnd;
            g_is_admin = is_running_as_admin();
            apply_window_dark_titlebar(hwnd);
            apply_control_dark_theme(hwnd);

            // Initialize GDI Theme Cache
            g_theme.init();

            // Initialize DPI and Fonts
            UINT init_dpi = GetDpiForWindow(hwnd);
            if (init_dpi == 0) init_dpi = 96;
            update_fonts_for_dpi(init_dpi);

            g_controller = std::make_unique<GuiController>(hwnd);

            // Header Settings Button
            g_h_btn_settings = CreateWindowExW(0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, NULL, NULL);
            SetPropW(g_h_btn_settings, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SendMessageW(g_h_btn_settings, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            SetWindowSubclass(g_h_btn_settings, DarkButtonSubclassProc, IDC_BTN_SETTINGS, 0);

            // Configuration Controls
            g_h_lbl_target = CreateWindowExW(0, L"STATIC", L"Target:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_h_lbl_target, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(g_h_lbl_target);

            g_h_combo_process = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, scale_dpi(235), scale_dpi(250), hwnd, (HMENU)(INT_PTR)IDC_COMBO_PROCESS, NULL, NULL);
            SendMessageW(g_h_combo_process, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(g_h_combo_process, CB_SETMINVISIBLE, 10, 0);
            apply_control_dark_theme(g_h_combo_process);
            SetWindowSubclass(g_h_combo_process, DarkComboSubclassProc, 0, 0);

            // Action Buttons
            g_h_btn_start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_START, NULL, NULL);
            SetPropW(g_h_btn_start, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::PrimaryEmerald));

            g_h_btn_stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_STOP, NULL, NULL);
            SetPropW(g_h_btn_stop, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::DangerRed));
            EnableWindow(g_h_btn_stop, FALSE);

            g_h_btn_clear = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_CLEAR, NULL, NULL);
            SetPropW(g_h_btn_clear, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            EnableWindow(g_h_btn_clear, FALSE);

            // Inspector Header Action Buttons
            g_h_btn_copy = CreateWindowExW(0, L"BUTTON", L"Copy JSON", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_COPY_JSON, NULL, NULL);
            SetPropW(g_h_btn_copy, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            EnableWindow(g_h_btn_copy, FALSE);

            g_h_btn_export = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_EXPORT_JSON, NULL, NULL);
            SetPropW(g_h_btn_export, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            EnableWindow(g_h_btn_export, FALSE);

            SetWindowSubclass(g_h_btn_start, DarkButtonSubclassProc, IDC_BTN_START, 0);
            SetWindowSubclass(g_h_btn_stop, DarkButtonSubclassProc, IDC_BTN_STOP, 0);
            SetWindowSubclass(g_h_btn_clear, DarkButtonSubclassProc, IDC_BTN_CLEAR, 0);
            SetWindowSubclass(g_h_btn_copy, DarkButtonSubclassProc, IDC_BTN_COPY_JSON, 0);
            SetWindowSubclass(g_h_btn_export, DarkButtonSubclassProc, IDC_BTN_EXPORT_JSON, 0);

            // Stutter Events ListView (LVS_EX_DOUBLEBUFFER: High throughput, flicker-free)
            g_h_list_stutters = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST_STUTTERS, NULL, NULL);
            SendMessageW(g_h_list_stutters, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            apply_control_dark_theme(g_h_list_stutters);
            SetWindowSubclass(g_h_list_stutters, ListViewSubclassProc, IDC_LIST_STUTTERS, 0);

            ListView_SetBkColor(g_h_list_stutters, COLOR_LIST_BG);
            ListView_SetTextBkColor(g_h_list_stutters, COLOR_LIST_BG);
            ListView_SetTextColor(g_h_list_stutters, COLOR_TEXT_PRI);
            ListView_SetExtendedListViewStyle(g_h_list_stutters, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            g_h_list_header = ListView_GetHeader(g_h_list_stutters);
            if (g_h_list_header) {
                apply_control_dark_theme(g_h_list_header);
                SetWindowSubclass(g_h_list_header, HeaderSubclassProc, 0, 0);
            }

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<LPWSTR>(L"#"); col.cx = scale_dpi(42); ListView_InsertColumn(g_h_list_stutters, 0, &col);
            col.pszText = const_cast<LPWSTR>(L"Time (UTC)"); col.cx = scale_dpi(145); ListView_InsertColumn(g_h_list_stutters, 1, &col);
            col.pszText = const_cast<LPWSTR>(L"Target Process"); col.cx = scale_dpi(150); ListView_InsertColumn(g_h_list_stutters, 2, &col);
            col.pszText = const_cast<LPWSTR>(L"Trigger Reason"); col.cx = scale_dpi(145); ListView_InsertColumn(g_h_list_stutters, 3, &col);
            col.pszText = const_cast<LPWSTR>(L"Duration"); col.cx = scale_dpi(85); ListView_InsertColumn(g_h_list_stutters, 4, &col);
            col.pszText = const_cast<LPWSTR>(L"Primary Culprit / Hypothesis"); col.cx = scale_dpi(280); ListView_InsertColumn(g_h_list_stutters, 5, &col);
            col.pszText = const_cast<LPWSTR>(L"Confidence"); col.cx = scale_dpi(95); ListView_InsertColumn(g_h_list_stutters, 6, &col);

            // Inspector Multi-line Viewer (Clean High-Contrast Monospace Dark Pane)
            g_h_edit_inspector = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EDIT_INSPECTOR, NULL, NULL);
            SendMessageW(g_h_edit_inspector, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
            SendMessageW(g_h_edit_inspector, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
            apply_control_dark_theme(g_h_edit_inspector);
            SetWindowSubclass(g_h_edit_inspector, ReportInspectorSubclassProc, IDC_EDIT_INSPECTOR, 0);

            // Load persistent user configuration after controls are created
            load_user_settings();
            update_metrics_text();

            // Register Hotkey with loaded configuration
            UnregisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE);
            if (!RegisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE, g_hotkey_mods | MOD_NOREPEAT, g_hotkey_vk)) {
                g_status_text = L"Hotkey Warning: " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another app";
                append_engine_log(L"[WARN] Hotkey " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another application.");
            }

            RECT init_rc{};
            GetClientRect(hwnd, &init_rc);
            layout_controls(hwnd, init_rc.right, init_rc.bottom);

            update_inspector(-1);
            g_controller->enumerate_graphical_processes_async();
            return 0;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            layout_controls(hwnd, width, height);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (pdis->CtlType == ODT_BUTTON) {
                draw_custom_button(pdis);
                return TRUE;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            int width = client_rc.right;
            int height = client_rc.bottom;

            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bmp));
            HBRUSH old_br = static_cast<HBRUSH>(GetCurrentObject(mem_dc, OBJ_BRUSH));
            HFONT old_font = static_cast<HFONT>(GetCurrentObject(mem_dc, OBJ_FONT));
            HPEN old_pen = static_cast<HPEN>(GetCurrentObject(mem_dc, OBJ_PEN));

            RECT dummy{};

            // 1. Fill Main Canvas Background
            FillRect(mem_dc, &client_rc, g_theme.br_bg);

            // 2. Top Header Strip (Y: 0 to 48)
            const int hdr_h = scale_dpi(48);
            RECT header_rc = { 0, 0, width, hdr_h };
            if (IntersectRect(&dummy, &header_rc, &ps.rcPaint)) {
                FillRect(mem_dc, &header_rc, g_theme.br_header);

                SelectObject(mem_dc, g_theme.pen_header_border);
                MoveToEx(mem_dc, 0, hdr_h, NULL);
                LineTo(mem_dc, width, hdr_h);

                SetBkMode(mem_dc, TRANSPARENT);

                // Brand Title & Tagline
                SelectObject(mem_dc, g_font_title);
                SetTextColor(mem_dc, COLOR_TEXT_PRI);
                RECT title_rc = { scale_dpi(16), scale_dpi(6), scale_dpi(280), scale_dpi(28) };
                DrawTextW(mem_dc, L"STUTTOMETER", -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                SelectObject(mem_dc, g_font_ui_sm_bold);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);
                RECT subtitle_rc = { scale_dpi(16), scale_dpi(27), scale_dpi(280), scale_dpi(45) };
                DrawTextW(mem_dc, L"REAL-TIME ETW DIAGNOSTIC", -1, &subtitle_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                // Real-Time Status Pill (True Horizontal Center)
                int elem_h = scale_dpi(30);
                int elem_y = scale_dpi(9);
                int pill_w = scale_dpi(280);
                int pill_left = (width - pill_w) / 2;
                int pill_right = pill_left + pill_w;
                RECT pill_rc = { pill_left, elem_y, pill_right, elem_y + elem_h };

                SelectObject(mem_dc, g_theme.br_pill);
                SelectObject(mem_dc, g_theme.pen_pill_border);
                RoundRect(mem_dc, pill_rc.left, pill_rc.top, pill_rc.right, pill_rc.bottom, scale_dpi(12), scale_dpi(12));

                // Status Dot: Universal Unicode Filled Circle via ClearType
                COLORREF dot_color = COLOR_TEXT_MUTED;
                if (g_current_session_state == GuiSessionState::RUNNING) {
                    dot_color = COLOR_ACCENT_EMERALD;
                } else if (g_current_session_state == GuiSessionState::DEGRADED_USER_ONLY || g_current_session_state == GuiSessionState::DEGRADED_KERNEL_ONLY || g_current_session_state == GuiSessionState::STARTING || g_current_session_state == GuiSessionState::STOPPING) {
                    dot_color = COLOR_ACCENT_AMB;
                }

                SelectObject(mem_dc, g_font_ui_bold);

                // True horizontal centering of combined (dot + gap + text) group
                RECT calc_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, g_status_text.c_str(), -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE);
                int text_w = calc_rc.right - calc_rc.left;
                int dot_w = scale_dpi(14);
                int gap = scale_dpi(6);
                int total_content_w = dot_w + gap + text_w;
                int centered_x = pill_rc.left + (pill_w - total_content_w) / 2;
                int start_x = std::max<int>(static_cast<int>(pill_rc.left) + scale_dpi(10), centered_x);

                SetTextColor(mem_dc, dot_color);
                RECT dot_rc = { start_x, pill_rc.top, start_x + dot_w, pill_rc.bottom };
                DrawTextW(mem_dc, L"\u25CF", -1, &dot_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SetTextColor(mem_dc, COLOR_TEXT_PRI);
                RECT status_text_rc = { start_x + dot_w + gap, pill_rc.top, pill_rc.right - scale_dpi(10), pill_rc.bottom };
                DrawTextW(mem_dc, g_status_text.c_str(), -1, &status_text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            }

            // 3. Unified Configuration Command Bar (Y: 54, Height: 52)
            int card_y = scale_dpi(54);
            int card_h = scale_dpi(52);
            RECT cfg_card_rc = { scale_dpi(16), card_y, width - scale_dpi(16), card_y + card_h };
            if (IntersectRect(&dummy, &cfg_card_rc, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, cfg_card_rc.left, cfg_card_rc.top, cfg_card_rc.right, cfg_card_rc.bottom, scale_dpi(12), scale_dpi(12));

                // Right-aligned Trigger Mode badge pill inside Configuration Card
                std::wstring mode_status_text;
                if (g_settings_config.frame_trigger_mode == FrameTriggerMode::HYBRID) {
                    mode_status_text = L"Trigger Mode: Hybrid (Auto Pacing & Judder)";
                } else if (g_settings_config.frame_trigger_mode == FrameTriggerMode::DYNAMIC_ONLY) {
                    mode_status_text = L"Trigger Mode: Dynamic Only (Relative & Judder)";
                } else {
                    int fps_val = static_cast<int>(std::round((g_settings_config.present_threshold_ms > 0.0) ? (1000.0 / g_settings_config.present_threshold_ms) : 60.0));
                    mode_status_text = L"Trigger Mode: Static (" + std::to_wstring(fps_val) + L" FPS Floor)";
                }

                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui_bold);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);

                RECT calc_mode_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, mode_status_text.c_str(), -1, &calc_mode_rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                int mode_text_w = calc_mode_rc.right - calc_mode_rc.left;
                int badge_mode_w = mode_text_w + scale_dpi(38);
                int badge_mode_h = scale_dpi(28);
                int badge_mode_y = card_y + (card_h - badge_mode_h) / 2;
                int badge_mode_x = cfg_card_rc.right - scale_dpi(12) - badge_mode_w;
                RECT badge_mode_rc = { badge_mode_x, badge_mode_y, badge_mode_x + badge_mode_w, badge_mode_y + badge_mode_h };

                SelectObject(mem_dc, g_theme.br_badge);
                SelectObject(mem_dc, g_theme.pen_badge_border);
                RoundRect(mem_dc, badge_mode_rc.left, badge_mode_rc.top, badge_mode_rc.right, badge_mode_rc.bottom, scale_dpi(8), scale_dpi(8));

                int mode_start_x = badge_mode_rc.left + (badge_mode_w - mode_text_w) / 2;
                RECT text_mode_rc = { mode_start_x, badge_mode_rc.top, mode_start_x + mode_text_w, badge_mode_rc.bottom };
                DrawTextW(mem_dc, mode_status_text.c_str(), -1, &text_mode_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // 4. Live Telemetry Badge (Right Aligned on Action Toolbar)
            RECT badge_rc = get_telemetry_badge_rect(width);
            if (IntersectRect(&dummy, &badge_rc, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, badge_rc.left, badge_rc.top, badge_rc.right, badge_rc.bottom, scale_dpi(10), scale_dpi(10));

                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui_bold);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);

                RECT calc_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, g_metrics_text.c_str(), -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                int text_w = calc_rc.right - calc_rc.left;
                int badge_w = badge_rc.right - badge_rc.left;
                int start_x = badge_rc.left + (badge_w - text_w) / 2;
                RECT text_rc = { start_x, badge_rc.top, start_x + text_w, badge_rc.bottom };
                DrawTextW(mem_dc, g_metrics_text.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // 5. Diagnostic Report Inspector Card Background
            RECT inspector_rect;
            GetWindowRect(g_h_edit_inspector, &inspector_rect);
            POINT pt_insp = { inspector_rect.left, inspector_rect.top };
            ScreenToClient(hwnd, &pt_insp);

            int insp_card_outer_y = pt_insp.y - scale_dpi(34);
            int insp_card_outer_bottom = client_rc.bottom - scale_dpi(16);
            RECT insp_card_box = { scale_dpi(16), insp_card_outer_y, width - scale_dpi(16), insp_card_outer_bottom };

            if (IntersectRect(&dummy, &insp_card_box, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, insp_card_box.left, insp_card_box.top, insp_card_box.right, insp_card_box.bottom, scale_dpi(12), scale_dpi(12));

                // Inspector Header Divider Line
                SelectObject(mem_dc, g_theme.pen_card_divider);
                MoveToEx(mem_dc, insp_card_box.left, pt_insp.y - scale_dpi(4), NULL);
                LineTo(mem_dc, insp_card_box.right, pt_insp.y - scale_dpi(4));

                // Inspector Header Title & Status
                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui_bold);
                SetTextColor(mem_dc, COLOR_TEXT_LABEL);
                std::wstring insp_label = L"DIAGNOSTIC REPORT INSPECTOR";
                if (g_selected_stutter_index >= 0 && g_selected_stutter_index < static_cast<int>(g_stutters.size())) {
                    insp_label += L" \u2014 Event #" + std::to_wstring(g_stutters[g_selected_stutter_index].id) + L" (" + utf8_to_wstring(g_stutters[g_selected_stutter_index].process_name) + L")";
                }
                RECT insp_hdr_text_rc = { insp_card_box.left + scale_dpi(12), insp_card_outer_y, insp_card_box.right - scale_dpi(220), pt_insp.y - scale_dpi(4) };
                DrawTextW(mem_dc, insp_label.c_str(), -1, &insp_hdr_text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            SelectObject(mem_dc, old_pen);
            SelectObject(mem_dc, old_font);
            SelectObject(mem_dc, old_br);

            BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old_bmp);
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ACTIVATEAPP: {
            if (wParam /* activating */ && g_controller && !g_controller->is_capturing()) {
                static auto last_scan = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan).count() > 1500) {
                    last_scan = now;
                    g_controller->enumerate_graphical_processes_async();
                }
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            if ((HWND)lParam == g_h_combo_process && wmEvent == CBN_SELCHANGE) {
                InvalidateRect((HWND)lParam, NULL, TRUE);
            }

            switch (wmId) {
                case IDC_BTN_SETTINGS: {
                    ShowSettingsDialog(hwnd);
                    break;
                }

                case IDC_BTN_START: {
                    auto cfg = read_gui_config();
                    if (!g_controller->start_session_async(cfg)) {
                        MessageBoxW(hwnd, L"Capture session is already active or starting.", L"Capture Notice", MB_OK | MB_ICONINFORMATION);
                    }
                    break;
                }

                case IDC_BTN_STOP: {
                    g_controller->stop_session_async();
                    break;
                }

                case IDC_BTN_CLEAR: {
                    clear_stutter_history();
                    break;
                }

                case IDC_BTN_REFRESH: {
                    g_controller->enumerate_graphical_processes_async();
                    break;
                }

                case IDC_BTN_EXPORT_JSON: {
                    export_selected_report_json(hwnd);
                    break;
                }

                case IDC_BTN_COPY_JSON: {
                    copy_selected_report_json(hwnd);
                    break;
                }

                case IDC_COMBO_PROCESS: {
                    if (wmEvent == CBN_SELCHANGE) {
                        auto cfg = read_gui_config();
                        g_controller->update_target_filter(cfg.target_pid, cfg.target_process_name);
                    } else if (wmEvent == CBN_DROPDOWN) {
                        if (g_controller && !g_controller->is_capturing()) {
                            g_controller->enumerate_graphical_processes_async();
                        }
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);
            if (pnm->idFrom == IDC_LIST_STUTTERS) {
                if (pnm->code == LVN_ITEMCHANGED) {
                    LPNMLISTVIEW pnmlv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                    if ((pnmlv->uChanged & LVIF_STATE) && (pnmlv->uNewState & LVIS_SELECTED)) {
                        g_selected_stutter_index = pnmlv->iItem;
                        update_inspector(g_selected_stutter_index);
                        if (g_h_edit_inspector && IsWindow(g_h_edit_inspector)) {
                            RECT client_rc{};
                            GetClientRect(hwnd, &client_rc);
                            RECT insp_wnd_rc{};
                            GetWindowRect(g_h_edit_inspector, &insp_wnd_rc);
                            POINT pt_insp = { insp_wnd_rc.left, insp_wnd_rc.top };
                            ScreenToClient(hwnd, &pt_insp);
                            RECT rc_hdr = { scale_dpi(16), pt_insp.y - scale_dpi(34), client_rc.right - scale_dpi(16), pt_insp.y };
                            InvalidateRect(hwnd, &rc_hdr, TRUE);
                        }
                    }
                } else if (pnm->code == NM_CUSTOMDRAW) {
                    LPNMLVCUSTOMDRAW pCustomDraw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                    switch (pCustomDraw->nmcd.dwDrawStage) {
                        case CDDS_PREPAINT:
                            return CDRF_NOTIFYITEMDRAW;

                        case CDDS_ITEMPREPAINT:
                            return CDRF_NOTIFYSUBITEMDRAW;

                        case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                            int item_idx = static_cast<int>(pCustomDraw->nmcd.dwItemSpec);
                            bool is_selected = (ListView_GetItemState(g_h_list_stutters, item_idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                            if (is_selected) {
                                pCustomDraw->clrTextBk = COLOR_LIST_SEL;
                                pCustomDraw->clrText = RGB(255, 255, 255);
                            } else {
                                pCustomDraw->clrTextBk = (item_idx % 2 == 0) ? COLOR_LIST_BG : COLOR_LIST_ROW_ALT;
                                pCustomDraw->clrText = COLOR_TEXT_BRIGHT;
                            }
                            return CDRF_DODEFAULT;
                        }
                    }
                }
            }
            break;
        }

        case WM_DPICHANGED: {
            UINT new_dpi = LOWORD(wParam);
            update_fonts_for_dpi(new_dpi);
            const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, NULL,
                prcNewWindow->left,
                prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            layout_controls(hwnd, client_rc.right - client_rc.left, client_rc.bottom - client_rc.top);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_HOTKEY: {
            if (g_h_settings_dlg != nullptr && IsWindow(g_h_settings_dlg)) {
                return 0; // Ignore hotkey while modal settings dialog is active
            }
            if (wParam == ID_HOTKEY_TOGGLE_CAPTURE && g_controller) {
                auto cur_state = g_controller->current_state();
                if (cur_state == GuiSessionState::IDLE) {
                    auto cfg = read_gui_config();
                    g_controller->start_session_async(cfg);
                } else if (g_controller->is_capturing()) {
                    g_controller->stop_session_async();
                }
            }
            return 0;
        }

        // Custom Stuttometer Notifications
        case WM_STUTTO_STATE_CHANGE: {
            auto state = static_cast<GuiSessionState>(wParam);
            update_session_ui_state(state);
            return 0;
        }

        case WM_STUTTO_TRIGGER: {
            std::unique_ptr<DiagnosticReport> report(reinterpret_cast<DiagnosticReport*>(lParam));
            handle_new_report(std::move(report));
            return 0;
        }

        case WM_STUTTO_METRICS: {
            std::unique_ptr<GuiMetrics> metrics(reinterpret_cast<GuiMetrics*>(lParam));
            if (metrics) {
                if (metrics->total_events_recorded > 0) {
                    g_has_received_data = true;
                }
                update_metrics_text();
                update_clear_button_state();

                // Invalidate the telemetry badge region with inflation
                RECT client_rc;
                GetClientRect(hwnd, &client_rc);
                RECT rc_b = get_telemetry_badge_rect(client_rc.right - client_rc.left);
                RECT rc_inv = { rc_b.left - scale_dpi(2), rc_b.top - scale_dpi(2), rc_b.right + scale_dpi(2), rc_b.bottom + scale_dpi(2) };
                InvalidateRect(hwnd, &rc_inv, FALSE);
            }
            return 0;
        }

        case WM_STUTTO_PROCESSES_UPDATED: {
            std::unique_ptr<ProcessList> procs(reinterpret_cast<ProcessList*>(lParam));
            handle_processes_updated(std::move(procs));
            return 0;
        }

        case WM_STUTTO_LOG: {
            std::unique_ptr<std::string> log_line(reinterpret_cast<std::string*>(lParam));
            if (log_line && !log_line->empty()) {
                g_has_received_data = true;
                std::wstring wline = utf8_to_wstring(*log_line);
                g_engine_logs.push_back(std::move(wline));
                if (g_engine_logs.size() > 200) {
                    g_engine_logs.erase(g_engine_logs.begin());
                }
                if (g_selected_stutter_index < 0) {
                    update_inspector(-1);
                }
                update_clear_button_state();
            }
            return 0;
        }

        // High-Contrast Control Coloring Handlers
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_h_edit_inspector) {
                SetBkColor(hdcStatic, COLOR_INPUT_BG);
                SetTextColor(hdcStatic, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_input;
            }
            if (hCtl == g_h_lbl_target) {
                SetBkColor(hdcStatic, COLOR_CARD_BG);
                SetTextColor(hdcStatic, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_card;
            }
            SetBkColor(hdcStatic, COLOR_BG);
            SetTextColor(hdcStatic, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_bg;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_h_edit_inspector) {
                SetBkColor(hdcEdit, COLOR_INPUT_BG);
                SetTextColor(hdcEdit, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_input;
            }
            SetBkColor(hdcEdit, COLOR_INPUT_BG);
            SetTextColor(hdcEdit, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcListBox = (HDC)wParam;
            SetBkColor(hdcListBox, COLOR_INPUT_BG);
            SetTextColor(hdcListBox, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_QUERYENDSESSION: {
            save_user_settings();
            return TRUE;
        }

        case WM_ENDSESSION: {
            if (wParam == TRUE) {
                if (g_controller) {
                    g_controller->shutdown();
                }
            }
            return 0;
        }

        case WM_CLOSE: {
            save_user_settings();
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            UnregisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE);

            if (g_controller) {
                g_controller->shutdown();
            }

            MSG msg;
            while (PeekMessageW(&msg, hwnd, WM_STUTTO_STATE_CHANGE, WM_STUTTO_LOG, PM_REMOVE)) {
                if (msg.message == WM_STUTTO_TRIGGER && msg.lParam) {
                    delete reinterpret_cast<DiagnosticReport*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_METRICS && msg.lParam) {
                    delete reinterpret_cast<GuiMetrics*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_PROCESSES_UPDATED && msg.lParam) {
                    delete reinterpret_cast<ProcessList*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_LOG && msg.lParam) {
                    delete reinterpret_cast<std::string*>(msg.lParam);
                }
            }

            g_controller.reset();
            g_stutters.clear();
            g_engine_logs.clear();

            PostQuitMessage(0);
            return 0;
        }

        case WM_NCDESTROY: {
            // Destroy cached GDI objects & fonts after all child windows are destroyed
            g_theme.destroy();

            if (g_font_title) DeleteObject(g_font_title);
            if (g_font_ui) DeleteObject(g_font_ui);
            if (g_font_ui_bold) DeleteObject(g_font_ui_bold);
            if (g_font_ui_sm_bold) DeleteObject(g_font_ui_sm_bold);
            if (g_font_mono) DeleteObject(g_font_mono);
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace stuttometer::gui

// Win32 Application Entry Point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    HRESULT hr_com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&icex);

    stuttometer::gui::init_process_dark_mode();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = stuttometer::gui::MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // Handled in WM_PAINT to eliminate flicker
    wc.lpszClassName = L"StuttometerMainWindowClass";

    HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconBig) hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!hIconSmall) hIconSmall = hIconBig;

    wc.hIcon = hIconBig ? hIconBig : LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = hIconSmall ? hIconSmall : wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(hr_com)) CoUninitialize();
        return 1;
    }

    UINT init_dpi = GetDpiForSystem();
    if (init_dpi == 0) {
        init_dpi = 96;
        HDC screen = GetDC(NULL);
        if (screen) {
            init_dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(NULL, screen);
        }
    }
    int base_w = MulDiv(1020, init_dpi, 96);
    int base_h = MulDiv(660, init_dpi, 96);

    RECT rc = { 0, 0, base_w, base_h };
    // INTENTIONAL DESIGN DECISION:
    // Fixed window style (no WS_THICKFRAME / WS_MAXIMIZEBOX) guarantees a locked, clean dashboard window.
    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);

    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    // Center window on screen work area (accounting for Windows taskbar)
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int screen_w = work_area.right - work_area.left;
    int screen_h = work_area.bottom - work_area.top;
    int pos_x = work_area.left + (screen_w > win_w ? (screen_w - win_w) / 2 : 0);
    int pos_y = work_area.top + (screen_h > win_h ? (screen_h - win_h) / 2 : 0);

    HWND hwnd = CreateWindowExW(
        0,
        L"StuttometerMainWindowClass",
        L"Stuttometer - Real-Time ETW Stutter & Glitch Diagnostic Utility",
        dwStyle,
        pos_x, pos_y,
        win_w, win_h,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main application window.", L"Error", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(hr_com)) CoUninitialize();
        return 1;
    }

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    stuttometer::gui::apply_window_dark_titlebar(hwnd);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!hwnd || !IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hIconBig) DestroyIcon(hIconBig);
    if (hIconSmall && hIconSmall != hIconBig) DestroyIcon(hIconSmall);

    if (SUCCEEDED(hr_com)) CoUninitialize();

    return static_cast<int>(msg.wParam);
}
