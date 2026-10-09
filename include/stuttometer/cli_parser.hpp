#pragma once

#include "stuttometer/frame_pacing_tracker.hpp"
#include "stuttometer/gui_config.hpp"
#include "stuttometer/constants.hpp"
#include <string>
#include <cstdint>
#include <cstddef>
#include <ostream>

namespace stuttometer {

enum class CliParseResult {
    SUCCESS,
    EXIT_OK,
    EXIT_ERROR
};

struct CliConfig {
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{DEFAULT_60HZ_VBLANK_MS};
    bool enable_audio{true};
    double cooldown_ms{1000.0};
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
    uint32_t buffer_slots{DEFAULT_BUFFER_SLOTS};
    uint32_t target_pid{0};
    std::string target_process_name;
    std::string output_file;
    std::string output_dir;
    uint32_t max_reports{0};
    std::string provider_tier{"standard"};
    bool redact{false};
    bool verbose{false};
    bool print_version{false};
    bool run_self_check{false};
    std::string trigger_mode_str{"hybrid"};
    PacingProfile pacing_profile{PacingProfile::AUTO_ADAPTIVE};
    double spike_multiplier{DEFAULT_SPIKE_MULTIPLIER};
    double min_spike_delta_ms{DEFAULT_MIN_SPIKE_DELTA_MS};
    bool enable_judder{true};
    double judder_swing_ratio{pacing_tuning::DEFAULT_JUDDER_SWING_RATIO};
    std::string dump_events_path;
    size_t dump_max_mb{100};
    size_t dump_max_files{3};
    std::string export_csv_path;
    bool target_pid_manual{false};
    bool present_threshold_manual{false};
    bool smi_threshold_manual{false};
    DetectionPreset preset{DetectionPreset::BALANCED};
    bool preset_manual{false};
    ReportSeverity min_report_severity{ReportSeverity::WARNING};
    bool min_report_severity_manual{false};
    ReportSeverity osd_min_severity{ReportSeverity::DANGER};
    bool osd_min_severity_manual{false};
    uint8_t judder_min_alternations{5};
    bool judder_min_alternations_manual{false};
    uint8_t dwm_min_missed_vblanks{1};
    uint8_t kernel_frame_stall_min_missed_vblanks{1};
};

struct CliRangeDef {
    const char* name;
    double min_val;
    double max_val;
    const char* unit;
    double (*getter)(const CliConfig&);
};

inline constexpr CliRangeDef CLI_RANGES[] = {
    {"--window-ms",               50.0,    1000.0,   "ms",  [](const CliConfig& c) { return c.window_pre_ms; }},
    {"--post-trigger-ms",         0.0,     200.0,    "ms",  [](const CliConfig& c) { return c.window_post_ms; }},
    {"--present-threshold-ms",    2.0,     200.0,    "ms",  [](const CliConfig& c) { return c.present_threshold_ms; }},
    {"--cooldown-ms",             100.0,   10000.0,  "ms",  [](const CliConfig& c) { return c.cooldown_ms; }},
    {"--dpc-threshold-us",        100.0,   50000.0,  "us",  [](const CliConfig& c) { return static_cast<double>(c.dpc_threshold_us); }},
    {"--isr-threshold-us",        50.0,    50000.0,  "us",  [](const CliConfig& c) { return static_cast<double>(c.isr_threshold_us); }},
    {"--disk-threshold-ms",       1.0,     1000.0,   "ms",  [](const CliConfig& c) { return static_cast<double>(c.disk_threshold_ms); }},
    {"--cswitch-threshold-ms",    1.0,     500.0,    "ms",  [](const CliConfig& c) { return static_cast<double>(c.cswitch_preempt_ms); }},
    {"--smi-threshold-ms",        10.0,    100.0,    "ms",  [](const CliConfig& c) { return c.smi_severity_threshold_ms; }},
    {"--d3d12-pso-threshold-ms",  1.0,     500.0,    "ms",  [](const CliConfig& c) { return static_cast<double>(c.d3d12_pso_threshold_ms); }},
    {"--vram-threshold-mb",       1.0,     1024.0,   "MB",  [](const CliConfig& c) { return static_cast<double>(c.vram_demoted_threshold_mb); }},
    {"--mem-alloc-threshold-mb",  1.0,     1024.0,   "MB",  [](const CliConfig& c) { return static_cast<double>(c.mem_alloc_threshold_mb); }},
    {"--mem-trim-threshold-mb",   1.0,     1024.0,   "MB",  [](const CliConfig& c) { return static_cast<double>(c.mem_trim_threshold_mb); }},
    {"--mem-physical-latency-us", 50.0,    50000.0,  "us",  [](const CliConfig& c) { return static_cast<double>(c.mem_physical_latency_us); }},
    {"--buffer-slots",            static_cast<double>(MIN_BUFFER_SLOTS), static_cast<double>(MAX_BUFFER_SLOTS), "",   [](const CliConfig& c) { return static_cast<double>(c.buffer_slots); }},
    {"--spike-multiplier",        1.2,     10.0,     "",    [](const CliConfig& c) { return c.spike_multiplier; }},
    {"--min-spike-delta-ms",      1.0,     50.0,     "ms",  [](const CliConfig& c) { return c.min_spike_delta_ms; }},
    {"--judder-swing-ratio",      0.1,     0.9,      "",    [](const CliConfig& c) { return c.judder_swing_ratio; }},
    {"--judder-min-alternations", 1.0,     50.0,     "",    [](const CliConfig& c) { return static_cast<double>(c.judder_min_alternations); }}
};

template <typename T>
inline bool validate_option_range(std::ostream& err, const char* name, T val, double min_val, double max_val, const char* unit = "") {
    if (static_cast<double>(val) < min_val || static_cast<double>(val) > max_val) {
        err << "[STUTTOMETER] Error: " << name << " must be between " << min_val << " and " << max_val;
        if (unit && unit[0] != '\0') err << " " << unit;
        err << ".\n";
        return false;
    }
    return true;
}

inline bool validate_all_cli_ranges(std::ostream& err, const CliConfig& config) {
    for (const auto& r : CLI_RANGES) {
        const double val = r.getter(config);
        if (!validate_option_range(err, r.name, val, r.min_val, r.max_val, r.unit)) {
            return false;
        }
    }
    return true;
}

CliParseResult parse_cli_args(int argc, const char* const* argv, CliConfig& out_config, std::ostream& out, std::ostream& err);

inline CliParseResult parse_cli_args(int argc, char** argv, CliConfig& out_config, std::ostream& out, std::ostream& err) {
    return parse_cli_args(argc, const_cast<const char* const*>(argv), out_config, out, err);
}

} // namespace stuttometer
