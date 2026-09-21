#include "stuttometer/gui_config.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <string_view>

namespace stuttometer {

static std::wstring utf8_to_wstring_helper(std::string_view str) {
    if (str.empty()) return std::wstring();
    int num_chars = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), NULL, 0);
    if (num_chars <= 0) return std::wstring();
    std::wstring result(num_chars, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), result.data(), num_chars);
    return result;
}

static std::string wstring_to_utf8_helper(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int num_bytes = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), NULL, 0, NULL, NULL);
    if (num_bytes <= 0) return std::string();
    std::string result(num_bytes, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), result.data(), num_bytes, NULL, NULL);
    return result;
}

double fps_to_present_threshold_ms(double fps) noexcept {
    if (std::isnan(fps) || fps <= 0.0) return 16.67;
    return std::clamp(1000.0 / fps, 2.0, 200.0);
}

nlohmann::json serialize_gui_settings_to_json(
    const GuiConfig& config,
    uint32_t hotkey_vk,
    uint32_t hotkey_mods,
    bool sound_cues,
    const std::string& last_target_process
) {
    nlohmann::json j;
    j["settings_version"] = 1;
    j["hotkey_vk"] = hotkey_vk;
    j["hotkey_modifiers"] = hotkey_mods;
    j["sound_cues_enabled"] = sound_cues;

    j["present_threshold_manual"] = config.present_threshold_manual;
    if (config.present_threshold_manual) {
        double fps = (config.present_threshold_ms > 0.0 && !std::isnan(config.present_threshold_ms))
            ? (1000.0 / config.present_threshold_ms)
            : 60.0;
        j["min_fps_threshold"] = std::clamp(fps, 10.0, 500.0);
    } else {
        j["min_fps_threshold"] = nullptr;
    }

    j["smi_threshold_manual"] = config.smi_threshold_manual;
    j["provider_tier"] = config.provider_tier;
    j["enable_audio_glitch"] = config.enable_audio;
    j["enable_pii_redaction"] = config.redact;
    j["enable_osd"] = config.enable_osd;
    j["osd_duration_ms"] = config.osd_duration_ms;

    std::string pos_str = "top_right";
    switch (config.osd_position) {
        case OsdPosition::BOTTOM_RIGHT: pos_str = "bottom_right"; break;
        case OsdPosition::TOP_LEFT:     pos_str = "top_left"; break;
        case OsdPosition::BOTTOM_LEFT:  pos_str = "bottom_left"; break;
        case OsdPosition::TOP_RIGHT:
        default:                        pos_str = "top_right"; break;
    }
    j["osd_position"] = pos_str;

    j["window_pre_ms"] = config.window_pre_ms;
    j["window_post_ms"] = config.window_post_ms;
    j["cooldown_ms"] = config.cooldown_ms;
    j["buffer_slots"] = config.buffer_slots;
    j["dpc_threshold_us"] = config.dpc_threshold_us;
    j["isr_threshold_us"] = config.isr_threshold_us;
    j["disk_threshold_ms"] = config.disk_threshold_ms;
    j["cswitch_preempt_ms"] = config.cswitch_preempt_ms;
    j["smi_severity_threshold_ms"] = config.smi_severity_threshold_ms;
    j["d3d12_pso_threshold_ms"] = config.d3d12_pso_threshold_ms;
    j["vram_demoted_threshold_mb"] = config.vram_demoted_threshold_mb;
    j["mem_alloc_threshold_mb"] = config.mem_alloc_threshold_mb;
    j["mem_trim_threshold_mb"] = config.mem_trim_threshold_mb;
    j["mem_physical_latency_us"] = config.mem_physical_latency_us;
    j["frame_trigger_mode"] = std::string(frame_trigger_mode_to_string(config.frame_trigger_mode));
    j["pacing_profile"] = std::string(pacing_profile_to_string(config.pacing_profile));
    j["spike_multiplier"] = config.spike_multiplier;
    j["min_spike_delta_ms"] = config.min_spike_delta_ms;
    j["enable_judder_detection"] = config.enable_judder_detection;
    j["auto_save_dir"] = wstring_to_utf8_helper(config.output_dir);
    j["last_target_process"] = last_target_process;

    return j;
}

void deserialize_gui_settings_from_json(
    const nlohmann::json& j,
    GuiConfig& out_config,
    uint32_t& out_hotkey_vk,
    uint32_t& out_hotkey_mods,
    bool& out_sound_cues,
    std::string& out_last_target_process
) {
    try {
        if (j.contains("hotkey_vk") && j["hotkey_vk"].is_number_unsigned()) {
            out_hotkey_vk = j["hotkey_vk"].get<uint32_t>();
        }
        if (j.contains("hotkey_modifiers") && j["hotkey_modifiers"].is_number_unsigned()) {
            out_hotkey_mods = j["hotkey_modifiers"].get<uint32_t>();
        }
        if (j.contains("sound_cues_enabled") && j["sound_cues_enabled"].is_boolean()) {
            out_sound_cues = j["sound_cues_enabled"].get<bool>();
        }

        bool manual = false;
        if (j.contains("present_threshold_manual") && j["present_threshold_manual"].is_boolean()) {
            manual = j["present_threshold_manual"].get<bool>();
        }

        if (!manual) {
            out_config.present_threshold_manual = false;
            out_config.present_threshold_ms = 16.67;
        } else {
            if (j.contains("min_fps_threshold") && j["min_fps_threshold"].is_number()) {
                double fps = j["min_fps_threshold"].get<double>();
                if (fps >= 10.0 && fps <= 500.0) {
                    out_config.present_threshold_manual = true;
                    out_config.present_threshold_ms = fps_to_present_threshold_ms(fps);
                } else {
                    out_config.present_threshold_manual = false;
                    out_config.present_threshold_ms = 16.67;
                }
            } else {
                out_config.present_threshold_manual = false;
                out_config.present_threshold_ms = 16.67;
            }
        }

        if (j.contains("smi_threshold_manual") && j["smi_threshold_manual"].is_boolean()) {
            out_config.smi_threshold_manual = j["smi_threshold_manual"].get<bool>();
        } else if (j.contains("smi_severity_threshold_ms") && j["smi_severity_threshold_ms"].is_number()) {
            out_config.smi_threshold_manual = (std::abs(j["smi_severity_threshold_ms"].get<double>() - 33.3) > 0.1);
        } else {
            out_config.smi_threshold_manual = false;
        }

        if (j.contains("smi_severity_threshold_ms") && j["smi_severity_threshold_ms"].is_number()) {
            double v = j["smi_severity_threshold_ms"].get<double>();
            if (v >= 10.0 && v <= 100.0) out_config.smi_severity_threshold_ms = v;
        }

        if (j.contains("provider_tier") && j["provider_tier"].is_string()) {
            std::string tier = j["provider_tier"].get<std::string>();
            if (tier == "full" || tier == "standard" || tier == "minimal") {
                out_config.provider_tier = tier;
            }
        }

        if (j.contains("enable_audio_glitch") && j["enable_audio_glitch"].is_boolean()) {
            out_config.enable_audio = j["enable_audio_glitch"].get<bool>();
        }

        if (j.contains("enable_pii_redaction") && j["enable_pii_redaction"].is_boolean()) {
            out_config.redact = j["enable_pii_redaction"].get<bool>();
        }

        if (j.contains("window_pre_ms") && j["window_pre_ms"].is_number()) {
            double v = j["window_pre_ms"].get<double>();
            if (v >= 50.0 && v <= 1000.0) out_config.window_pre_ms = v;
        }

        if (j.contains("window_post_ms") && j["window_post_ms"].is_number()) {
            double v = j["window_post_ms"].get<double>();
            if (v >= 0.0 && v <= 200.0) out_config.window_post_ms = v;
        }

        if (j.contains("cooldown_ms") && j["cooldown_ms"].is_number()) {
            double v = j["cooldown_ms"].get<double>();
            if (v >= 100.0 && v <= 10000.0) out_config.cooldown_ms = v;
        }

        if (j.contains("buffer_slots") && j["buffer_slots"].is_number_unsigned()) {
            uint32_t v = j["buffer_slots"].get<uint32_t>();
            if (v >= 65536 && v <= 1048576) out_config.buffer_slots = v;
        }

        if (j.contains("dpc_threshold_us") && j["dpc_threshold_us"].is_number_unsigned()) {
            uint32_t v = j["dpc_threshold_us"].get<uint32_t>();
            if (v >= 100 && v <= 50000) out_config.dpc_threshold_us = v;
        }

        if (j.contains("isr_threshold_us") && j["isr_threshold_us"].is_number_unsigned()) {
            uint32_t v = j["isr_threshold_us"].get<uint32_t>();
            if (v >= 50 && v <= 50000) out_config.isr_threshold_us = v;
        }

        if (j.contains("disk_threshold_ms") && j["disk_threshold_ms"].is_number_unsigned()) {
            uint32_t v = j["disk_threshold_ms"].get<uint32_t>();
            if (v >= 1 && v <= 1000) out_config.disk_threshold_ms = v;
        }

        if (j.contains("cswitch_preempt_ms") && j["cswitch_preempt_ms"].is_number_unsigned()) {
            uint32_t v = j["cswitch_preempt_ms"].get<uint32_t>();
            if (v >= 1 && v <= 500) out_config.cswitch_preempt_ms = v;
        }

        if (j.contains("d3d12_pso_threshold_ms") && j["d3d12_pso_threshold_ms"].is_number_unsigned()) {
            uint32_t v = j["d3d12_pso_threshold_ms"].get<uint32_t>();
            if (v >= 1 && v <= 500) out_config.d3d12_pso_threshold_ms = v;
        }

        if (j.contains("vram_demoted_threshold_mb") && j["vram_demoted_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["vram_demoted_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) out_config.vram_demoted_threshold_mb = v;
        }

        if (j.contains("mem_alloc_threshold_mb") && j["mem_alloc_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["mem_alloc_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) out_config.mem_alloc_threshold_mb = v;
        }

        if (j.contains("mem_trim_threshold_mb") && j["mem_trim_threshold_mb"].is_number_unsigned()) {
            uint32_t v = j["mem_trim_threshold_mb"].get<uint32_t>();
            if (v >= 1 && v <= 1024) out_config.mem_trim_threshold_mb = v;
        }

        if (j.contains("mem_physical_latency_us") && j["mem_physical_latency_us"].is_number_unsigned()) {
            uint32_t v = j["mem_physical_latency_us"].get<uint32_t>();
            if (v >= 50 && v <= 50000) out_config.mem_physical_latency_us = v;
        }

        if (j.contains("frame_trigger_mode") && j["frame_trigger_mode"].is_string()) {
            std::string m = j["frame_trigger_mode"].get<std::string>();
            if (m == "dynamic") out_config.frame_trigger_mode = FrameTriggerMode::DYNAMIC_ONLY;
            else if (m == "static") out_config.frame_trigger_mode = FrameTriggerMode::STATIC_ONLY;
            else out_config.frame_trigger_mode = FrameTriggerMode::HYBRID;
        }

        if (j.contains("spike_multiplier") && j["spike_multiplier"].is_number()) {
            double v = j["spike_multiplier"].get<double>();
            if (v >= 1.2 && v <= 10.0) out_config.spike_multiplier = v;
        }

        if (j.contains("min_spike_delta_ms") && j["min_spike_delta_ms"].is_number()) {
            double v = j["min_spike_delta_ms"].get<double>();
            if (v >= 1.0 && v <= 50.0) out_config.min_spike_delta_ms = v;
        }

        if (j.contains("pacing_profile") && j["pacing_profile"].is_string()) {
            std::string p_str = j["pacing_profile"].get<std::string>();
            out_config.pacing_profile = pacing_profile_from_string(p_str);
        } else {
            bool is_custom = (std::abs(out_config.spike_multiplier - 2.0) > 1e-9 ||
                              std::abs(out_config.min_spike_delta_ms - 4.0) > 1e-9);
            out_config.pacing_profile = is_custom ? PacingProfile::CUSTOM : PacingProfile::AUTO_ADAPTIVE;
        }

        if (j.contains("enable_judder_detection") && j["enable_judder_detection"].is_boolean()) {
            out_config.enable_judder_detection = j["enable_judder_detection"].get<bool>();
        }

        if (j.contains("auto_save_dir") && j["auto_save_dir"].is_string()) {
            out_config.output_dir = utf8_to_wstring_helper(j["auto_save_dir"].get<std::string>());
        }

        if (j.contains("enable_osd") && j["enable_osd"].is_boolean()) {
            out_config.enable_osd = j["enable_osd"].get<bool>();
        }

        if (j.contains("osd_duration_ms") && j["osd_duration_ms"].is_number_unsigned()) {
            uint32_t v = j["osd_duration_ms"].get<uint32_t>();
            if (v >= 500 && v <= 10000) out_config.osd_duration_ms = v;
        }

        if (j.contains("osd_position") && j["osd_position"].is_string()) {
            std::string pos = j["osd_position"].get<std::string>();
            if (pos == "bottom_right") out_config.osd_position = OsdPosition::BOTTOM_RIGHT;
            else if (pos == "top_left") out_config.osd_position = OsdPosition::TOP_LEFT;
            else if (pos == "bottom_left") out_config.osd_position = OsdPosition::BOTTOM_LEFT;
            else out_config.osd_position = OsdPosition::TOP_RIGHT;
        }

        if (j.contains("last_target_process") && j["last_target_process"].is_string()) {
            out_last_target_process = j["last_target_process"].get<std::string>();
        }
    } catch (...) {
        // Non-fatal, retain defaults
    }
}

} // namespace stuttometer
