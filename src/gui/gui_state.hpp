#pragma once

#include "theme.hpp"
#include "gui_controller.hpp"
#include "osd_toast.hpp"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <cstdint>
#include <string_view>

namespace stuttometer::gui {

// Main Control IDs
constexpr int IDC_BTN_START          = 1001;
constexpr int IDC_BTN_STOP           = 1002;
constexpr int IDC_BTN_SETTINGS       = 1003;
constexpr int IDC_BTN_CLEAR          = 1004;
constexpr int IDC_BTN_EXPORT_JSON    = 1006;
constexpr int IDC_BTN_COPY_JSON      = 1007;
constexpr int IDC_COMBO_PROCESS      = 1008;
constexpr int IDC_LIST_STUTTERS      = 1014;
constexpr int IDC_EDIT_INSPECTOR     = 1015;
constexpr int IDC_BTN_COPY_CARD      = 1016;
constexpr int IDC_BTN_EXPORT_CARD    = 1017;
constexpr int IDC_BTN_SESSION_SUMMARY = 1018;

// Global Hotkeys
constexpr int ID_HOTKEY_TOGGLE_CAPTURE = 9001;

// Button Styling Categories
enum class BtnStyle : INT_PTR {
    PrimaryEmerald = 1,
    DangerRed = 2,
    SecondarySlate = 3,
    QuickAction = 4
};

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

// Global UI State & Handles
extern HWND g_hwnd_main;
extern HWND g_h_settings_dlg;
extern bool g_is_admin;
extern std::unique_ptr<GuiController> g_controller;
extern std::deque<StutterRecord> g_stutters;
extern uint32_t g_next_stutter_id;
extern int g_selected_stutter_index;

extern HWND g_h_lbl_target;
extern HWND g_h_combo_process;

extern HWND g_h_btn_start;
extern HWND g_h_btn_stop;
extern HWND g_h_btn_settings;
extern HWND g_h_btn_session_summary;
extern HWND g_h_btn_clear;
extern HWND g_h_btn_export;
extern HWND g_h_btn_copy;
extern HWND g_h_btn_export_card;
extern HWND g_h_btn_copy_card;
extern HWND g_h_list_stutters;
extern HWND g_h_edit_inspector;
extern HWND g_h_list_header;

extern OsdToast g_osd_toast;

// Centralized Settings & Preferences State
extern GuiConfig g_settings_config;
extern bool g_sound_cues_enabled;
extern UINT g_hotkey_vk;
extern UINT g_hotkey_mods;
extern std::wstring g_settings_file_path;
extern std::string g_settings_last_target_process;
extern ProcessList g_cached_processes;
extern std::wstring g_status_text;

extern GuiSessionState g_current_session_state;
extern uint64_t g_capture_start_tick;
extern uint64_t g_capture_elapsed_seconds;
extern bool g_has_received_data;
extern uint32_t g_session_stutter_count;
extern uint32_t g_session_audio_count;
extern std::wstring g_metrics_text;

extern std::deque<std::wstring> g_engine_logs;

void append_engine_log(std::wstring log_msg);

double fps_to_present_threshold_ms(double fps);
std::wstring utf8_to_wstring(std::string_view str);
std::string wstring_to_utf8(const std::wstring& wstr);
std::wstring format_hotkey_display(UINT fsModifiers, UINT vk);
std::wstring format_duration(uint64_t total_seconds);
void init_settings_path();
void load_user_settings();
void save_user_settings();
void update_metrics_text();
GuiConfig read_gui_config();
void apply_fonts_to_main_controls();

} // namespace stuttometer::gui
