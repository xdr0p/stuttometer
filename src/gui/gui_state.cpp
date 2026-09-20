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
UINT g_hotkey_vk = VK_F11;
UINT g_hotkey_mods = MOD_CONTROL;
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

void append_engine_log(std::wstring log_msg) {
    g_engine_logs.push_back(std::move(log_msg));
    if (g_engine_logs.size() > 200) {
        g_engine_logs.erase(g_engine_logs.begin());
    }
}

double fps_to_present_threshold_ms(double fps) {
    if (fps <= 0.0) return 16.67; // Default 60 FPS
    return std::clamp(1000.0 / fps, 2.0, 200.0);
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
        if (j.contains("pacing_profile") && j["pacing_profile"].is_string()) {
            std::string p_str = j["pacing_profile"].get<std::string>();
            g_settings_config.pacing_profile = pacing_profile_from_string(p_str);
        } else {
            // Epsilon migration for legacy configs against TriggerConfig defaults
            TriggerConfig def_cfg{};
            bool is_custom = (std::abs(g_settings_config.spike_multiplier - def_cfg.spike_multiplier) > 1e-9 ||
                              std::abs(g_settings_config.min_spike_delta_ms - def_cfg.min_spike_delta_ms) > 1e-9);
            g_settings_config.pacing_profile = is_custom ? PacingProfile::CUSTOM : PacingProfile::AUTO_ADAPTIVE;
        }
        if (j.contains("enable_judder_detection") && j["enable_judder_detection"].is_boolean()) {
            g_settings_config.enable_judder_detection = j["enable_judder_detection"].get<bool>();
        }
        if (j.contains("auto_save_dir") && j["auto_save_dir"].is_string()) {
            g_settings_config.output_dir = utf8_to_wstring(j["auto_save_dir"].get<std::string>());
        }
        if (j.contains("enable_osd") && j["enable_osd"].is_boolean()) {
            g_settings_config.enable_osd = j["enable_osd"].get<bool>();
        }
        if (j.contains("osd_duration_ms") && j["osd_duration_ms"].is_number_unsigned()) {
            uint32_t v = j["osd_duration_ms"].get<uint32_t>();
            if (v >= 500 && v <= 10000) g_settings_config.osd_duration_ms = v;
        }
        if (j.contains("osd_position") && j["osd_position"].is_string()) {
            std::string pos = j["osd_position"].get<std::string>();
            if (pos == "bottom_right") g_settings_config.osd_position = OsdPosition::BOTTOM_RIGHT;
            else if (pos == "top_left") g_settings_config.osd_position = OsdPosition::TOP_LEFT;
            else if (pos == "bottom_left") g_settings_config.osd_position = OsdPosition::BOTTOM_LEFT;
            else g_settings_config.osd_position = OsdPosition::TOP_RIGHT;
        }
        if (j.contains("last_target_process") && j["last_target_process"].is_string()) {
            g_settings_last_target_process = j["last_target_process"].get<std::string>();
        }
    } catch (...) {
        // Non-fatal, keep defaults
    }
}

void save_user_settings() {
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
        j["enable_osd"] = g_settings_config.enable_osd;
        j["osd_duration_ms"] = g_settings_config.osd_duration_ms;
        std::string pos_str = "top_right";
        switch (g_settings_config.osd_position) {
            case OsdPosition::BOTTOM_RIGHT: pos_str = "bottom_right"; break;
            case OsdPosition::TOP_LEFT: pos_str = "top_left"; break;
            case OsdPosition::BOTTOM_LEFT: pos_str = "bottom_left"; break;
            case OsdPosition::TOP_RIGHT:
            default: pos_str = "top_right"; break;
        }
        j["osd_position"] = pos_str;
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
        j["pacing_profile"] = pacing_profile_to_string(g_settings_config.pacing_profile);
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
        } else {
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
