#include "gui_state.hpp"

#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace stuttometer::gui {

// Global UI State Handles and Variables
HWND g_hwnd_main = nullptr;
HWND g_h_settings_dlg = nullptr;
bool g_is_admin = false;
std::unique_ptr<GuiController> g_controller;
std::deque<StutterRecord> g_stutters;
uint32_t g_next_stutter_id = 1;
int g_selected_stutter_index = -1;

HWND g_h_lbl_target = nullptr;
HWND g_h_combo_process = nullptr;

HWND g_h_btn_start = nullptr;
HWND g_h_btn_stop = nullptr;
HWND g_h_btn_settings = nullptr;
HWND g_h_btn_session_summary = nullptr;
HWND g_h_btn_clear = nullptr;
HWND g_h_btn_export = nullptr;
HWND g_h_btn_copy = nullptr;
HWND g_h_btn_export_card = nullptr;
HWND g_h_btn_copy_card = nullptr;
HWND g_h_list_stutters = nullptr;
HWND g_h_edit_inspector = nullptr;
HWND g_h_list_header = nullptr;

OsdToast g_osd_toast;

GuiConfig g_settings_config;
bool g_sound_cues_enabled = true;
uint32_t g_hotkey_vk = VK_F11;
uint32_t g_hotkey_mods = MOD_CONTROL;
std::wstring g_settings_file_path;
std::string g_settings_last_target_process;
ProcessList g_cached_processes;
std::wstring g_status_text = L"IDLE (Ready to monitor)";

GuiSessionState g_current_session_state = GuiSessionState::IDLE;
uint64_t g_capture_start_tick = 0;
uint64_t g_capture_elapsed_seconds = 0;
bool g_has_received_data = false;
uint32_t g_session_stutter_count = 0;
uint32_t g_session_audio_count = 0;
std::wstring g_metrics_text = L"Time: 00:00  |  Stutters: 0  |  Audio: 0";

std::deque<std::wstring> g_engine_logs;
static std::mutex g_engine_logs_mutex;

void append_engine_log(std::wstring log_msg) {
    std::lock_guard<std::mutex> lock(g_engine_logs_mutex);
    g_engine_logs.push_back(std::move(log_msg));
    if (g_engine_logs.size() > 200) {
        g_engine_logs.erase(g_engine_logs.begin());
    }
}

std::wstring utf8_to_wstring(std::string_view str) {
    if (str.empty()) return std::wstring();
    int num_chars = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), NULL, 0);
    if (num_chars <= 0) return std::wstring();
    std::wstring result(num_chars, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), result.data(), num_chars);
    return result;
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int num_bytes = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), NULL, 0, NULL, NULL);
    if (num_bytes <= 0) return std::string();
    std::string result(num_bytes, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), result.data(), num_bytes, NULL, NULL);
    return result;
}

std::wstring format_hotkey_display(UINT fsModifiers, UINT vk) {
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

std::wstring format_duration(uint64_t total_seconds) {
    uint64_t hrs = total_seconds / 3600;
    uint64_t mins = (total_seconds % 3600) / 60;
    uint64_t secs = total_seconds % 60;
    wchar_t buf[32];
    if (hrs > 0) {
        swprintf_s(buf, L"%02llu:%02llu:%02llu", hrs, mins, secs);
    } else {
        swprintf_s(buf, L"%02llu:%02llu", mins, secs);
    }
    return std::wstring(buf);
}

void init_settings_path() {
    wchar_t appdata_path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata_path))) {
        std::wstring dir = std::wstring(appdata_path) + L"\\Stuttometer";
        CreateDirectoryW(dir.c_str(), NULL);
        g_settings_file_path = dir + L"\\settings.json";
    }
}

void load_user_settings() {
    init_settings_path();
    if (g_settings_file_path.empty()) return;
    try {
        std::ifstream f(g_settings_file_path);
        if (!f.is_open()) {
            return;
        }
        nlohmann::json j;
        f >> j;
        deserialize_gui_settings_from_json(
            j,
            g_settings_config,
            g_hotkey_vk,
            g_hotkey_mods,
            g_sound_cues_enabled,
            g_settings_last_target_process
        );
    } catch (...) {
        // Non-fatal, keep defaults
    }
}

void save_user_settings() {
    if (g_settings_file_path.empty()) return;
    try {
        GuiConfig active_cfg = read_gui_config();
        nlohmann::json j = serialize_gui_settings_to_json(
            g_settings_config,
            g_hotkey_vk,
            g_hotkey_mods,
            g_sound_cues_enabled,
            active_cfg.target_process_name
        );

        std::ofstream f(g_settings_file_path);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (...) {
        // Non-fatal
    }
}

void update_metrics_text() {
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

GuiConfig read_gui_config() {
    GuiConfig cfg = g_settings_config;

    if (g_h_combo_process && IsWindow(g_h_combo_process)) {
        int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
        if (cur_sel > 0) {
            LRESULT data = SendMessageW(g_h_combo_process, CB_GETITEMDATA, cur_sel, 0);
            if (data >= 0 && data < static_cast<LRESULT>(g_cached_processes.size())) {
                cfg.target_pid = g_cached_processes[data].pid;
                cfg.target_process_name = wstring_to_utf8(g_cached_processes[data].name);
            }
        } else if (cur_sel == 0) {
            cfg.target_pid = 0;
            cfg.target_process_name.clear();
        }
    }

    return cfg;
}

void apply_fonts_to_main_controls() {
    if (g_h_lbl_target) SendMessageW(g_h_lbl_target, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_combo_process) {
        SendMessageW(g_h_combo_process, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (g_h_btn_settings) SendMessageW(g_h_btn_settings, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_session_summary) SendMessageW(g_h_btn_session_summary, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_start) SendMessageW(g_h_btn_start, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_stop) SendMessageW(g_h_btn_stop, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_clear) SendMessageW(g_h_btn_clear, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_export) SendMessageW(g_h_btn_export, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_copy) SendMessageW(g_h_btn_copy, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_export_card) SendMessageW(g_h_btn_export_card, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_btn_copy_card) SendMessageW(g_h_btn_copy_card, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (g_h_list_stutters) SendMessageW(g_h_list_stutters, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (g_h_edit_inspector) SendMessageW(g_h_edit_inspector, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
}

} // namespace stuttometer::gui
