#include "stuttometer/cli_parser.hpp"
#include "stuttometer/version.hpp"
#include <CLI/CLI.hpp>
#include <set>
#include <iostream>
#include <cstdio>
#include <cmath>

namespace stuttometer {

CliParseResult parse_cli_args(int argc, const char* const* argv, CliConfig& out_config, std::ostream& out, std::ostream& err) {
    CLI::App app{"Stuttometer - Real-Time Windows ETW Stutter & Glitch Diagnostic Utility"};

    double window_pre_ms = 250.0;
    double window_post_ms = 30.0;
    double present_threshold_ms = 16.67;
    bool enable_audio = true;
    double cooldown_ms = 1000.0;
    uint32_t dpc_threshold_us = 1000;
    uint32_t isr_threshold_us = 500;
    uint32_t disk_threshold_ms = 20;
    uint32_t cswitch_preempt_ms = 5;
    double smi_severity_threshold_ms = 33.3;
    uint32_t d3d12_pso_threshold_ms = 5;
    uint32_t vram_demoted_threshold_mb = 8;
    uint32_t mem_alloc_threshold_mb = 16;
    uint32_t mem_trim_threshold_mb = 4;
    uint32_t mem_physical_latency_us = 1000;
    uint32_t buffer_slots = 262144;
    uint32_t target_pid = 0;
    std::string target_process_name;
    std::string output_file;
    std::string output_dir;
    uint32_t max_reports = 0;
    std::string provider_tier = "standard";
    bool redact = false;
    bool verbose = false;
    bool print_version = false;
    bool run_self_check = false;
    std::string trigger_mode_str = "hybrid";
    std::string pacing_profile_str;
    bool high_refresh_preset = false;
    double spike_multiplier = 2.0;
    double min_spike_delta_ms = 4.0;
    bool enable_judder = true;
    double judder_swing_ratio = 0.35;
    std::string dump_events_path;
    size_t dump_max_mb = 100;
    size_t dump_max_files = 3;
    std::string export_csv_path;

    app.add_option("--window-ms", window_pre_ms, "Pre-trigger window duration in ms (50-1000, default: 250)");
    app.add_option("--post-trigger-ms", window_post_ms, "Post-trigger capture duration in ms (0-200, default: 30)");
    app.add_option("--present-threshold-ms", present_threshold_ms, "DXGI Present stutter threshold in ms (2.0-200.0, default: 16.67)");
    app.add_option("--trigger-mode", trigger_mode_str, "Frame trigger mode: hybrid, dynamic, static (default: hybrid)");
    app.add_option("--pacing-profile", pacing_profile_str, "Pacing sensitivity profile: auto, high-refresh, conservative (default: auto)");
    app.add_flag("--high-refresh", high_refresh_preset, "Alias for --pacing-profile high-refresh");
    app.add_option("--spike-multiplier", spike_multiplier, "Relative stutter spike multiplier (1.2-10.0, default: 2.0)");
    app.add_option("--min-spike-delta-ms", min_spike_delta_ms, "Minimum absolute spike delta in ms (1.0-50.0, default: 4.0)");
    app.add_flag("--judder-detection,!--no-judder", enable_judder, "Enable/disable cadence judder detection (default: enabled)");
    app.add_option("--judder-swing-ratio", judder_swing_ratio, "Judder cadence swing threshold ratio (0.1-0.9, default: 0.35)");
    app.add_flag("--audio-trigger,!--no-audio", enable_audio, "Enable/disable AudioGlitch Event ID 11 trigger");
    app.add_option("--cooldown-ms", cooldown_ms, "Minimum cooldown between reports in ms (100-10000, default: 1000)");
    app.add_option("--dpc-threshold-us", dpc_threshold_us, "DPC anomaly threshold in microseconds (100-50000, default: 1000)");
    app.add_option("--isr-threshold-us", isr_threshold_us, "ISR anomaly threshold in microseconds (50-50000, default: 500)");
    app.add_option("--disk-threshold-ms", disk_threshold_ms, "Disk latency anomaly threshold in ms (1-1000, default: 20)");
    app.add_option("--cswitch-threshold-ms", cswitch_preempt_ms, "Context switch preemption threshold in ms (1-500, default: 5)");
    app.add_option("--smi-threshold-ms", smi_severity_threshold_ms, "Hardware SMI stall threshold in ms (10-100, default: 33.3)");
    app.add_option("--d3d12-pso-threshold-ms", d3d12_pso_threshold_ms, "D3D12 PSO compilation threshold in ms (1-500, default: 5)");
    app.add_option("--vram-threshold-mb", vram_demoted_threshold_mb, "GPU VRAM demotion anomaly threshold in MB (1-1024, default: 8)");
    app.add_option("--mem-alloc-threshold-mb", mem_alloc_threshold_mb, "VirtualAlloc commit stall threshold in MB (1-1024, default: 16)");
    app.add_option("--mem-trim-threshold-mb", mem_trim_threshold_mb, "Working set out-swap trim threshold in MB (1-1024, default: 4)");
    app.add_option("--mem-physical-latency-us", mem_physical_latency_us, "Physical memory / MDL allocation latency threshold in us (50-50000, default: 1000)");
    app.add_option("--buffer-slots", buffer_slots, "Ring buffer capacity in slots (65536-1048576, default: 262144)");
    app.add_option("--target-pid", target_pid, "Target Process ID to monitor (default: 0 = monitor all)");
    app.add_option("--target-process", target_process_name, "Target process name substring (e.g. Game.exe)");
    app.add_option("--output", output_file, "Output file path for JSON reports (overwritten on each trigger if max-reports != 1; use --output-dir to save all reports)");
    app.add_option("--output-dir", output_dir, "Directory to save individual trigger reports");
    app.add_option("--max-reports", max_reports, "Maximum number of reports before exiting (0 = continuous)");
    app.add_option("--tier", provider_tier, "Provider tier: minimal, standard, full (default: standard)");
    app.add_option("--dump-events", dump_events_path, "Stream real-time ETW events to NDJSON file (or - for stdout)");
    app.add_option("--dump-max-mb", dump_max_mb, "Maximum size per NDJSON file before rotation in MB (10-1024, default: 100; ignored when --dump-events is '-')")
       ->check(CLI::Range(10ull, 1024ull))
       ->needs("--dump-events");
    app.add_option("--dump-max-files", dump_max_files, "Maximum number of rotated NDJSON files to retain (1-10, default: 3; ignored when --dump-events is '-')")
       ->check(CLI::Range(1ull, 10ull))
       ->needs("--dump-events");
    app.add_option("--export-csv", export_csv_path, "Export frame pacing timeline to CSV (overwritten on each trigger; use --output-dir for per-trigger files)");
    app.add_flag("--redact", redact, "Redact process names, file paths, and user identifiers");
    app.add_flag("--verbose", verbose, "Print detailed event stream metrics to console");
    app.add_flag("--version", print_version, "Print version information and exit");
    app.add_flag("--self-check", run_self_check, "Run non-destructive environment diagnostics & ETW provider checks, then exit");

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        out << app.help() << "\n";
        return CliParseResult::EXIT_OK;
    } catch (const CLI::CallForAllHelp&) {
        out << app.help("", CLI::AppFormatMode::All) << "\n";
        return CliParseResult::EXIT_OK;
    } catch (const CLI::ParseError& e) {
        err << e.what() << "\n";
        return CliParseResult::EXIT_ERROR;
    }

    // Check conflict between --dump-events - and --version
    if (dump_events_path == "-" && print_version) {
        err << "[STUTTOMETER] Error: --dump-events - cannot be combined with --version.\n";
        return CliParseResult::EXIT_ERROR;
    }

    // Precedence: --version beats --self-check
    if (print_version) {
        out << TOOL_NAME << " v" << TOOL_VERSION << "\n";
        return CliParseResult::EXIT_OK;
    }

    // Pacing profile resolution, alias, and overrides
    bool has_pacing_profile = (app.count("--pacing-profile") > 0);
    bool has_high_refresh = (app.count("--high-refresh") > 0);
    bool has_spike_mult = (app.count("--spike-multiplier") > 0);
    bool has_min_delta = (app.count("--min-spike-delta-ms") > 0);

    PacingProfile resolved_profile = PacingProfile::AUTO_ADAPTIVE;

    if (has_pacing_profile) {
        auto opt = pacing_profile_from_cli_string(pacing_profile_str);
        if (!opt.has_value()) {
            err << "[STUTTOMETER] Error: Invalid --pacing-profile '" << pacing_profile_str << "'. Must be 'auto', 'high-refresh', or 'conservative'.\n";
            return CliParseResult::EXIT_ERROR;
        }
        resolved_profile = *opt;
        if (has_high_refresh) {
            err << "[Config] Note: --pacing-profile took precedence over --high-refresh\n";
        }
    } else if (has_high_refresh) {
        resolved_profile = PacingProfile::HIGH_REFRESH;
    } else if (has_spike_mult || has_min_delta) {
        resolved_profile = PacingProfile::CUSTOM;
    } else {
        resolved_profile = PacingProfile::AUTO_ADAPTIVE;
    }

    // If explicit parameter overrides are supplied with a preset
    if ((has_spike_mult || has_min_delta) && (has_pacing_profile || has_high_refresh)) {
        resolved_profile = PacingProfile::CUSTOM;
        err << "[Config] Note: Explicit parameter override active; operating in CUSTOM profile.\n";
    }

    // Assign multiplier & delta based on resolved profile
    if (resolved_profile == PacingProfile::AUTO_ADAPTIVE) {
        spike_multiplier = 2.0;
        min_spike_delta_ms = 4.0;
    } else if (resolved_profile == PacingProfile::HIGH_REFRESH) {
        spike_multiplier = HIGH_REFRESH_SPIKE_MULTIPLIER;
        min_spike_delta_ms = HIGH_REFRESH_MIN_DELTA_MS;
    } else if (resolved_profile == PacingProfile::CONSERVATIVE) {
        spike_multiplier = CONSERVATIVE_SPIKE_MULTIPLIER;
        min_spike_delta_ms = CONSERVATIVE_MIN_DELTA_MS;
    } else if (resolved_profile == PacingProfile::CUSTOM) {
        if (!has_spike_mult) spike_multiplier = 2.0;
        if (!has_min_delta) min_spike_delta_ms = 4.0;
    }

    // Notice when --trigger-mode static is combined with pacing profile
    if (trigger_mode_str == "static" && (has_pacing_profile || has_high_refresh)) {
        err << "[Config] Note: --pacing-profile has no effect in static-only frame trigger mode.\n";
    }

    if (app.count("--present-threshold-ms") > 0 && present_threshold_ms < 12.0) {
        char note_buf[256];
        std::snprintf(note_buf, sizeof(note_buf), "[Config] Note: --present-threshold-ms (%.1f ms) is below 12.0 ms. Content running below %.0f FPS will trigger static threshold stalls.\n",
                     present_threshold_ms, 1000.0 / present_threshold_ms);
        err << note_buf;
    }

    if (dump_events_path == "-" && (app.count("--dump-max-mb") > 0 || app.count("--dump-max-files") > 0)) {
        err << "[STUTTOMETER] Notice: --dump-max-mb and --dump-max-files are ignored when streaming to stdout ('-').\n";
    }
    if (export_csv_path == "-") {
        err << "[STUTTOMETER] Error: --export-csv does not support stdout ('-'); must specify a file path.\n";
        return CliParseResult::EXIT_ERROR;
    }

    // CLI Range and Option Validation (after profile resolution, per NM7)
    if (window_pre_ms < 50.0 || window_pre_ms > 1000.0) {
        err << "[STUTTOMETER] Error: --window-ms must be between 50.0 and 1000.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (window_post_ms < 0.0 || window_post_ms > 200.0) {
        err << "[STUTTOMETER] Error: --post-trigger-ms must be between 0.0 and 200.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (present_threshold_ms < 2.0 || present_threshold_ms > 200.0) {
        err << "[STUTTOMETER] Error: --present-threshold-ms must be between 2.0 and 200.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (cooldown_ms < 100.0 || cooldown_ms > 10000.0) {
        err << "[STUTTOMETER] Error: --cooldown-ms must be between 100.0 and 10000.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (dpc_threshold_us < 100 || dpc_threshold_us > 50000) {
        err << "[STUTTOMETER] Error: --dpc-threshold-us must be between 100 and 50000 us.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (isr_threshold_us < 50 || isr_threshold_us > 50000) {
        err << "[STUTTOMETER] Error: --isr-threshold-us must be between 50 and 50000 us.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (disk_threshold_ms < 1 || disk_threshold_ms > 1000) {
        err << "[STUTTOMETER] Error: --disk-threshold-ms must be between 1 and 1000 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (cswitch_preempt_ms < 1 || cswitch_preempt_ms > 500) {
        err << "[STUTTOMETER] Error: --cswitch-threshold-ms must be between 1 and 500 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (smi_severity_threshold_ms < 10.0 || smi_severity_threshold_ms > 100.0) {
        err << "[STUTTOMETER] Error: --smi-threshold-ms must be between 10.0 and 100.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (d3d12_pso_threshold_ms < 1 || d3d12_pso_threshold_ms > 500) {
        err << "[STUTTOMETER] Error: --d3d12-pso-threshold-ms must be between 1 and 500 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (vram_demoted_threshold_mb < 1 || vram_demoted_threshold_mb > 1024) {
        err << "[STUTTOMETER] Error: --vram-threshold-mb must be between 1 and 1024 MB.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (mem_alloc_threshold_mb < 1 || mem_alloc_threshold_mb > 1024) {
        err << "[STUTTOMETER] Error: --mem-alloc-threshold-mb must be between 1 and 1024 MB.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (mem_trim_threshold_mb < 1 || mem_trim_threshold_mb > 1024) {
        err << "[STUTTOMETER] Error: --mem-trim-threshold-mb must be between 1 and 1024 MB.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (mem_physical_latency_us < 50 || mem_physical_latency_us > 50000) {
        err << "[STUTTOMETER] Error: --mem-physical-latency-us must be between 50 and 50000 us.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (buffer_slots < 65536 || buffer_slots > 1048576) {
        err << "[STUTTOMETER] Error: --buffer-slots must be between 65536 and 1048576.\n";
        return CliParseResult::EXIT_ERROR;
    }

    if (spike_multiplier < 1.2 || spike_multiplier > 10.0) {
        err << "[STUTTOMETER] Error: --spike-multiplier must be between 1.2 and 10.0.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (min_spike_delta_ms < 1.0 || min_spike_delta_ms > 50.0) {
        err << "[STUTTOMETER] Error: --min-spike-delta-ms must be between 1.0 and 50.0 ms.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (judder_swing_ratio < 0.1 || judder_swing_ratio > 0.9) {
        err << "[STUTTOMETER] Error: --judder-swing-ratio must be between 0.1 and 0.9.\n";
        return CliParseResult::EXIT_ERROR;
    }

    const std::set<std::string> valid_trigger_modes = { "hybrid", "dynamic", "static" };
    if (valid_trigger_modes.find(trigger_mode_str) == valid_trigger_modes.end()) {
        err << "[STUTTOMETER] Error: Invalid --trigger-mode '" << trigger_mode_str << "'. Must be 'hybrid', 'dynamic', or 'static'.\n";
        return CliParseResult::EXIT_ERROR;
    }

    const std::set<std::string> valid_tiers = { "minimal", "standard", "full" };
    if (valid_tiers.find(provider_tier) == valid_tiers.end()) {
        err << "[STUTTOMETER] Error: Invalid --tier '" << provider_tier << "'. Must be 'minimal', 'standard', or 'full'.\n";
        return CliParseResult::EXIT_ERROR;
    }
    if (target_process_name.size() > 260) {
        err << "[STUTTOMETER] Error: --target-process name exceeds maximum length (260 characters).\n";
        return CliParseResult::EXIT_ERROR;
    }

    out_config.window_pre_ms = window_pre_ms;
    out_config.window_post_ms = window_post_ms;
    out_config.present_threshold_ms = present_threshold_ms;
    out_config.enable_audio = enable_audio;
    out_config.cooldown_ms = cooldown_ms;
    out_config.dpc_threshold_us = dpc_threshold_us;
    out_config.isr_threshold_us = isr_threshold_us;
    out_config.disk_threshold_ms = disk_threshold_ms;
    out_config.cswitch_preempt_ms = cswitch_preempt_ms;
    out_config.smi_severity_threshold_ms = smi_severity_threshold_ms;
    out_config.d3d12_pso_threshold_ms = d3d12_pso_threshold_ms;
    out_config.vram_demoted_threshold_mb = vram_demoted_threshold_mb;
    out_config.mem_alloc_threshold_mb = mem_alloc_threshold_mb;
    out_config.mem_trim_threshold_mb = mem_trim_threshold_mb;
    out_config.mem_physical_latency_us = mem_physical_latency_us;
    out_config.buffer_slots = buffer_slots;
    out_config.target_pid = target_pid;
    out_config.target_process_name = target_process_name;
    out_config.output_file = output_file;
    out_config.output_dir = output_dir;
    out_config.max_reports = max_reports;
    out_config.provider_tier = provider_tier;
    out_config.redact = redact;
    out_config.verbose = verbose;
    out_config.print_version = print_version;
    out_config.run_self_check = run_self_check;
    out_config.trigger_mode_str = trigger_mode_str;
    out_config.pacing_profile = resolved_profile;
    out_config.spike_multiplier = spike_multiplier;
    out_config.min_spike_delta_ms = min_spike_delta_ms;
    out_config.enable_judder = enable_judder;
    out_config.judder_swing_ratio = judder_swing_ratio;
    out_config.dump_events_path = dump_events_path;
    out_config.dump_max_mb = dump_max_mb;
    out_config.dump_max_files = dump_max_files;
    out_config.export_csv_path = export_csv_path;
    out_config.target_pid_manual = (app.count("--target-pid") > 0 && target_pid != 0);

    return CliParseResult::SUCCESS;
}

} // namespace stuttometer
