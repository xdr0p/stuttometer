#pragma once

#include "stuttometer/frame_pacing_tracker.hpp"
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
    double present_threshold_ms{16.67};
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
    uint32_t buffer_slots{262144};
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
    double spike_multiplier{2.0};
    double min_spike_delta_ms{4.0};
    bool enable_judder{true};
    double judder_swing_ratio{0.35};
    std::string dump_events_path;
    size_t dump_max_mb{100};
    size_t dump_max_files{3};
    std::string export_csv_path;
    bool target_pid_manual{false};
    bool present_threshold_manual{false};
    bool smi_threshold_manual{false};
};

CliParseResult parse_cli_args(int argc, const char* const* argv, CliConfig& out_config, std::ostream& out, std::ostream& err);

inline CliParseResult parse_cli_args(int argc, char** argv, CliConfig& out_config, std::ostream& out, std::ostream& err) {
    return parse_cli_args(argc, const_cast<const char* const*>(argv), out_config, out, err);
}

} // namespace stuttometer
