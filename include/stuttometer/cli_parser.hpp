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
    OK,             // Parsing succeeded; continue into main() body
    EXIT_HANDLED,   // Handled non-monitoring execution (--help, --version, --dump-effective-config); return 0
    ERROR_USAGE     // CLI usage error / parse failure / bad argument; return 2
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
    bool quiet{false};
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

CliParseResult parse_cli_args(int argc, const char* const* argv, CliConfig& out_config, std::ostream& out, std::ostream& err);

inline CliParseResult parse_cli_args(int argc, char** argv, CliConfig& out_config, std::ostream& out, std::ostream& err) {
    return parse_cli_args(argc, const_cast<const char* const*>(argv), out_config, out, err);
}

} // namespace stuttometer
