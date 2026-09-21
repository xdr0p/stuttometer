#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include <stuttometer/frame_pacing_tracker.hpp>

namespace stuttometer {

enum class OsdPosition : uint32_t {
    TOP_RIGHT = 0,
    BOTTOM_RIGHT = 1,
    TOP_LEFT = 2,
    BOTTOM_LEFT = 3
};

struct GuiConfig {
    // Window & Timing
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{16.67};
    double cooldown_ms{1000.0};
    bool enable_audio{true};
    std::string provider_tier{"standard"};
    uint32_t buffer_slots{262144};

    // Correlation Diagnostic Thresholds
    uint32_t dpc_threshold_us{1000};
    uint32_t isr_threshold_us{500};
    uint32_t disk_threshold_ms{20};
    uint32_t cswitch_preempt_ms{5};
    double smi_severity_threshold_ms{33.3};
    uint32_t d3d12_pso_threshold_ms{5};
    uint32_t vram_demoted_threshold_mb{8};
    uint32_t mem_alloc_threshold_mb{16};
    uint32_t mem_trim_threshold_mb{4};
    uint32_t mem_physical_latency_us{1000};

    // Session Filters & Redaction & Auto-Save
    uint32_t target_pid{0};
    std::string target_process_name;
    bool redact{false};
    std::wstring output_dir;

    // Frame Pacing & Dynamic Relative Trigger configuration
    FrameTriggerMode frame_trigger_mode{FrameTriggerMode::HYBRID};
    PacingProfile pacing_profile{PacingProfile::AUTO_ADAPTIVE};
    double spike_multiplier{2.0};
    double min_spike_delta_ms{4.0};
    bool enable_judder_detection{true};
    double judder_swing_ratio{0.35};

    // In-Game OSD Toast Configuration
    bool enable_osd{false};
    uint32_t osd_duration_ms{3500};
    OsdPosition osd_position{OsdPosition::TOP_RIGHT};

    // Manual threshold override tracking
    bool present_threshold_manual{false};
    bool smi_threshold_manual{false};

    bool operator==(const GuiConfig& other) const = default;
};

double fps_to_present_threshold_ms(double fps) noexcept;

nlohmann::json serialize_gui_settings_to_json(
    const GuiConfig& config,
    uint32_t hotkey_vk,
    uint32_t hotkey_mods,
    bool sound_cues,
    const std::string& last_target_process = ""
);

void deserialize_gui_settings_from_json(
    const nlohmann::json& j,
    GuiConfig& out_config,
    uint32_t& out_hotkey_vk,
    uint32_t& out_hotkey_mods,
    bool& out_sound_cues,
    std::string& out_last_target_process
);

} // namespace stuttometer
