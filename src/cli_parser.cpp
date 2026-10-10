#include "stuttometer/cli_parser.hpp"
#include "stuttometer/gui_config.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/version.hpp"
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cmath>

namespace stuttometer {

CliParseResult parse_cli_args(int argc, const char* const* argv, CliConfig& out_config, std::ostream& out, std::ostream& err) {
    // Future Profile Roadmap: User-defined thresholds can be ingested via JSON configuration profiles (e.g. --profile simracing.json).
    CLI::App app{"Stuttometer - Real-Time Windows ETW Stutter & Glitch Diagnostic Utility"};

    double window_pre_ms = 250.0;
    double window_post_ms = 30.0;
    double present_threshold_ms = DEFAULT_60HZ_VBLANK_MS;
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
    uint32_t buffer_slots = DEFAULT_BUFFER_SLOTS;
    uint32_t target_pid = 0;
    std::string target_process_name;
    std::string output_file;
    std::string output_dir;
    uint32_t max_reports = 0;
    std::string provider_tier = "standard";
    bool redact = false;
    bool verbose = false;
    bool quiet = false;
    bool print_version = false;
    bool run_self_check = false;
    std::string trigger_mode_str = "hybrid";
    std::string pacing_profile_str;
    bool high_refresh_preset = false;
    double spike_multiplier = DEFAULT_SPIKE_MULTIPLIER;
    double min_spike_delta_ms = DEFAULT_MIN_SPIKE_DELTA_MS;
    bool enable_judder = true;
    double judder_swing_ratio = pacing_tuning::DEFAULT_JUDDER_SWING_RATIO;
    uint32_t judder_min_alternations = 5;
    std::string dump_events_path;
    size_t dump_max_mb = 100;
    size_t dump_max_files = 3;
    std::string export_csv_path;
    std::string config_path;
    std::string min_report_severity_str = "warning";
    std::string preset_str = "balanced";
    std::string osd_min_severity_str = "danger";

    char present_thresh_help[160];
    std::snprintf(present_thresh_help, sizeof(present_thresh_help),
                  "DXGI Present stutter threshold in ms (default: auto-detected vblank, %.2f ms at 60Hz)",
                  DEFAULT_60HZ_VBLANK_MS);

    // Group: Capture Window
    app.add_option("--window-ms", window_pre_ms, "Pre-trigger window duration in ms (default: 250)")
       ->check(CLI::Range(50.0, 1000.0))
       ->group("Capture Window");
    app.add_option("--post-trigger-ms", window_post_ms, "Post-trigger capture duration in ms (default: 30)")
       ->check(CLI::Range(0.0, 200.0))
       ->group("Capture Window");
    app.add_option("--cooldown-ms", cooldown_ms, "Minimum cooldown between reports in ms (default: 1000)")
       ->check(CLI::Range(100.0, 10000.0))
       ->group("Capture Window");
    app.add_option("--buffer-slots", buffer_slots, "Ring buffer capacity in slots (default: 262144)")
       ->check(CLI::Range(MIN_BUFFER_SLOTS, MAX_BUFFER_SLOTS))
       ->group("Capture Window");

    // Group: Detection
    app.add_option("-p,--preset", preset_str, "Detection preset: balanced, competitive, conservative, forensic, custom (default: balanced)")
       ->group("Detection");
    app.add_option("--osd-min-severity", osd_min_severity_str, "Minimum severity for in-game OSD toast: all, warning, danger (default: danger)")
       ->group("Detection");
    app.add_option("--judder-min-alternations", judder_min_alternations, "Minimum alternations to trigger judder episode (preset-dependent, balanced: 5)")
       ->check(CLI::Range(1u, 50u))
       ->group("Detection");
    app.add_option("--present-threshold-ms", present_threshold_ms, present_thresh_help)
       ->check(CLI::Range(2.0, 200.0))
       ->group("Detection");
    app.add_option("--trigger-mode", trigger_mode_str, "Frame trigger mode: hybrid, dynamic, static (default: hybrid)")
       ->group("Detection");
    app.add_option("--pacing-profile", pacing_profile_str, "Pacing sensitivity profile: auto, high-refresh, conservative (default: auto)")
       ->group("Detection");
    app.add_flag("--high-refresh", high_refresh_preset, "Alias for --pacing-profile high-refresh")
       ->group("Detection");
    app.add_option("--spike-multiplier", spike_multiplier, "Relative stutter spike multiplier (default: 2.0)")
       ->check(CLI::Range(1.2, 10.0))
       ->group("Detection");
    app.add_option("--min-spike-delta-ms", min_spike_delta_ms, "Minimum absolute spike delta in ms (default: 4.0)")
       ->check(CLI::Range(1.0, 50.0))
       ->group("Detection");
    app.add_option("--min-report-severity", min_report_severity_str, "Minimum severity to trigger a correlated report: all, warning, danger (preset-dependent, balanced: warning)")
       ->group("Detection");
    app.add_flag("--judder-detection,!--no-judder", enable_judder, "Enable/disable cadence judder detection (default: enabled)")
       ->group("Detection");
    app.add_option("--judder-swing-ratio", judder_swing_ratio, "Judder cadence swing threshold ratio (preset-dependent, balanced: 0.50)")
       ->check(CLI::Range(0.1, 1.0))
       ->group("Detection");
    app.add_flag("--audio-trigger,!--no-audio", enable_audio, "Enable/disable AudioGlitch Event ID 11 trigger")
       ->group("Detection");

    // Group: Thresholds
    app.add_option("--dpc-threshold-us", dpc_threshold_us, "DPC anomaly threshold in microseconds (default: 1000)")
       ->check(CLI::Range(100u, 50000u))
       ->group("Thresholds");
    app.add_option("--isr-threshold-us", isr_threshold_us, "ISR anomaly threshold in microseconds (default: 500)")
       ->check(CLI::Range(50u, 50000u))
       ->group("Thresholds");
    app.add_option("--disk-threshold-ms", disk_threshold_ms, "Disk latency anomaly threshold in ms (default: 20)")
       ->check(CLI::Range(1u, 1000u))
       ->group("Thresholds");
    app.add_option("--cswitch-threshold-ms", cswitch_preempt_ms, "Context switch preemption threshold in ms (default: 5)")
       ->check(CLI::Range(1u, 500u))
       ->group("Thresholds");
    app.add_option("--smi-threshold-ms", smi_severity_threshold_ms, "Hardware SMI stall threshold in ms (default: 33.3)")
       ->check(CLI::Range(10.0, 100.0))
       ->group("Thresholds");
    app.add_option("--d3d12-pso-threshold-ms", d3d12_pso_threshold_ms, "D3D12 PSO compilation threshold in ms (default: 5)")
       ->check(CLI::Range(1u, 500u))
       ->group("Thresholds");
    app.add_option("--vram-threshold-mb", vram_demoted_threshold_mb, "GPU VRAM demotion anomaly threshold in MB (default: 8)")
       ->check(CLI::Range(1u, 1024u))
       ->group("Thresholds");
    app.add_option("--mem-alloc-threshold-mb", mem_alloc_threshold_mb, "VirtualAlloc commit stall threshold in MB (default: 16)")
       ->check(CLI::Range(1u, 1024u))
       ->group("Thresholds");
    app.add_option("--mem-trim-threshold-mb", mem_trim_threshold_mb, "Working set out-swap trim threshold in MB (default: 4)")
       ->check(CLI::Range(1u, 1024u))
       ->group("Thresholds");
    app.add_option("--mem-physical-latency-us", mem_physical_latency_us, "Physical memory / MDL allocation latency threshold in us (default: 1000)")
       ->check(CLI::Range(50u, 50000u))
       ->group("Thresholds");

    // Group: Targeting
    auto* opt_pid  = app.add_option("--target-pid", target_pid,
        "Target Process ID to monitor (default: 0 = monitor all)")
       ->group("Targeting");
    auto* opt_proc = app.add_option("-t,--target-process", target_process_name,
        "Target process name substring (e.g. Game.exe; attaches to first matching instance)")
       ->group("Targeting");
    opt_pid->excludes(opt_proc);
    opt_proc->excludes(opt_pid);

    // Group: Output
    app.add_option("-o,--output", output_file, "Output file path for JSON reports (overwritten on each trigger if max-reports != 1; use --output-dir to save all reports)")
       ->group("Output");
    app.add_option("--output-dir", output_dir, "Directory to save individual trigger reports")
       ->group("Output");
    app.add_option("--max-reports", max_reports, "Maximum number of reports before exiting (0 = continuous)")
       ->group("Output");
    app.add_option("--dump-events", dump_events_path, "Stream real-time ETW events to NDJSON file (or - for stdout)")
       ->group("Output");
    app.add_option("--dump-max-mb", dump_max_mb, "Maximum size per NDJSON file before rotation in MB (default: 100; ignored when --dump-events is '-')")
       ->check(CLI::Range(10ull, 1024ull))
       ->needs("--dump-events")
       ->group("Output");
    app.add_option("--dump-max-files", dump_max_files, "Maximum number of rotated NDJSON files to retain (default: 3; ignored when --dump-events is '-')")
       ->check(CLI::Range(1ull, 10ull))
       ->needs("--dump-events")
       ->group("Output");
    app.add_option("--export-csv", export_csv_path, "Export frame pacing timeline to CSV (overwritten on each trigger; use --output-dir for per-trigger files)")
       ->group("Output");

    // Group: General
    app.add_option("--tier", provider_tier, "Provider tier: minimal, standard, full (default: standard)")
       ->group("General");
    app.add_option("-c,--config", config_path, "Load settings from a JSON config file (CLI options override file values)")
       ->check(CLI::ExistingFile)
       ->group("General");
    app.add_flag("-r,--redact", redact, "Redact process names, file paths, and user identifiers")
       ->group("General");
    auto* opt_verbose = app.add_flag("-v,--verbose", verbose, "Print detailed event stream metrics to console")
       ->group("General");
    auto* opt_quiet = app.add_flag("-q,--quiet", quiet, "Suppress stdout startup banners and progress notices (errors still print to stderr)")
       ->group("General");
    opt_verbose->excludes(opt_quiet);
    opt_quiet->excludes(opt_verbose);
    app.add_flag("-V,--version", print_version, "Print version information and exit")
       ->group("General");
    bool dump_effective_config = false;
    app.add_flag("--dump-effective-config", dump_effective_config, "Print the resolved configuration after applying CLI args and --config, then exit")
       ->group("General");
    app.add_flag("--self-check", run_self_check, "Run non-destructive environment diagnostics & ETW provider checks, then exit")
       ->group("General");

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        out << app.help() << "\n";
        return CliParseResult::EXIT_HANDLED;
    } catch (const CLI::CallForAllHelp&) {
        out << app.help("", CLI::AppFormatMode::All) << "\n";
        return CliParseResult::EXIT_HANDLED;
    } catch (const CLI::ParseError& e) {
        err << e.what() << "\n";
        return CliParseResult::ERROR_USAGE;
    }

    // Load configuration overlay from JSON config file if specified
    DetectionPreset file_preset = DetectionPreset::BALANCED;
    bool has_file_preset = false;
    bool file_present_threshold_manual = false;
    bool file_smi_threshold_manual = false;
    bool file_has_spike_mult = false;
    bool file_has_min_delta = false;
    bool file_has_swing_ratio = false;
    bool file_has_judder_alt = false;
    bool file_has_pacing_profile = false;
    bool file_pacing_profile_is_custom = false;
    bool file_has_min_report_sev = false;

    if (!config_path.empty()) {
        std::ifstream config_file(config_path);
        if (!config_file.is_open()) {
            err << "[STUTTOMETER] Error: Failed to open config file: " << config_path << "\n";
            return CliParseResult::ERROR_USAGE;
        }

        nlohmann::json j;
        try {
            config_file >> j;
        } catch (const nlohmann::json::parse_error& ex) {
            err << "[STUTTOMETER] Error: Failed to parse config JSON from '" << config_path << "': " << ex.what() << "\n";
            return CliParseResult::ERROR_USAGE;
        }

        if (!j.is_object()) {
            err << "[STUTTOMETER] Error: Config file root must be a JSON object: " << config_path << "\n";
            return CliParseResult::ERROR_USAGE;
        }

        if (j.contains("settings_version") && j["settings_version"].is_number_integer()) {
            int ver = j["settings_version"].get<int>();
            if (ver < 2) {
                err << "[Config] Warning: Config file uses legacy schema v" << ver
                    << ". Some fields may be migrated or ignored.\n";
            }
        }

        // Helper to apply file value only if CLI flag was not supplied
        auto apply_if_unset = [&app](const char* flag, auto& target, const auto& val) {
            if (app.count(flag) == 0) {
                target = val;
            }
        };

        if (j.contains("window_pre_ms") && j["window_pre_ms"].is_number()) {
            apply_if_unset("--window-ms", window_pre_ms, j["window_pre_ms"].get<double>());
        }
        if (j.contains("window_post_ms") && j["window_post_ms"].is_number()) {
            apply_if_unset("--post-trigger-ms", window_post_ms, j["window_post_ms"].get<double>());
        }
        if (j.contains("cooldown_ms") && j["cooldown_ms"].is_number()) {
            apply_if_unset("--cooldown-ms", cooldown_ms, j["cooldown_ms"].get<double>());
        }
        if (j.contains("buffer_slots") && j["buffer_slots"].is_number_unsigned()) {
            apply_if_unset("--buffer-slots", buffer_slots, j["buffer_slots"].get<uint32_t>());
        }
        if (j.contains("dpc_threshold_us") && j["dpc_threshold_us"].is_number_unsigned()) {
            apply_if_unset("--dpc-threshold-us", dpc_threshold_us, j["dpc_threshold_us"].get<uint32_t>());
        }
        if (j.contains("isr_threshold_us") && j["isr_threshold_us"].is_number_unsigned()) {
            apply_if_unset("--isr-threshold-us", isr_threshold_us, j["isr_threshold_us"].get<uint32_t>());
        }
        if (j.contains("disk_threshold_ms") && j["disk_threshold_ms"].is_number_unsigned()) {
            apply_if_unset("--disk-threshold-ms", disk_threshold_ms, j["disk_threshold_ms"].get<uint32_t>());
        }
        if (j.contains("cswitch_preempt_ms") && j["cswitch_preempt_ms"].is_number_unsigned()) {
            apply_if_unset("--cswitch-threshold-ms", cswitch_preempt_ms, j["cswitch_preempt_ms"].get<uint32_t>());
        }
        if (j.contains("smi_severity_threshold_ms") && j["smi_severity_threshold_ms"].is_number()) {
            apply_if_unset("--smi-threshold-ms", smi_severity_threshold_ms, j["smi_severity_threshold_ms"].get<double>());
        }
        if (j.contains("smi_threshold_manual") && j["smi_threshold_manual"].is_boolean()) {
            file_smi_threshold_manual = j["smi_threshold_manual"].get<bool>();
        }
        if (j.contains("d3d12_pso_threshold_ms") && j["d3d12_pso_threshold_ms"].is_number_unsigned()) {
            apply_if_unset("--d3d12-pso-threshold-ms", d3d12_pso_threshold_ms, j["d3d12_pso_threshold_ms"].get<uint32_t>());
        }
        if (j.contains("vram_demoted_threshold_mb") && j["vram_demoted_threshold_mb"].is_number_unsigned()) {
            apply_if_unset("--vram-threshold-mb", vram_demoted_threshold_mb, j["vram_demoted_threshold_mb"].get<uint32_t>());
        }
        if (j.contains("mem_alloc_threshold_mb") && j["mem_alloc_threshold_mb"].is_number_unsigned()) {
            apply_if_unset("--mem-alloc-threshold-mb", mem_alloc_threshold_mb, j["mem_alloc_threshold_mb"].get<uint32_t>());
        }
        if (j.contains("mem_trim_threshold_mb") && j["mem_trim_threshold_mb"].is_number_unsigned()) {
            apply_if_unset("--mem-trim-threshold-mb", mem_trim_threshold_mb, j["mem_trim_threshold_mb"].get<uint32_t>());
        }
        if (j.contains("mem_physical_latency_us") && j["mem_physical_latency_us"].is_number_unsigned()) {
            apply_if_unset("--mem-physical-latency-us", mem_physical_latency_us, j["mem_physical_latency_us"].get<uint32_t>());
        }
        if (j.contains("spike_multiplier") && j["spike_multiplier"].is_number()) {
            apply_if_unset("--spike-multiplier", spike_multiplier, j["spike_multiplier"].get<double>());
            if (app.count("--spike-multiplier") == 0) file_has_spike_mult = true;
        }
        if (j.contains("min_spike_delta_ms") && j["min_spike_delta_ms"].is_number()) {
            apply_if_unset("--min-spike-delta-ms", min_spike_delta_ms, j["min_spike_delta_ms"].get<double>());
            if (app.count("--min-spike-delta-ms") == 0) file_has_min_delta = true;
        }
        if (j.contains("judder_swing_ratio") && j["judder_swing_ratio"].is_number()) {
            apply_if_unset("--judder-swing-ratio", judder_swing_ratio, j["judder_swing_ratio"].get<double>());
            if (app.count("--judder-swing-ratio") == 0) file_has_swing_ratio = true;
        }
        if (j.contains("judder_min_alternations") && j["judder_min_alternations"].is_number_integer()) {
            apply_if_unset("--judder-min-alternations", judder_min_alternations, static_cast<uint32_t>(j["judder_min_alternations"].get<int64_t>()));
            if (app.count("--judder-min-alternations") == 0) file_has_judder_alt = true;
        }
        if (j.contains("enable_judder_detection") && j["enable_judder_detection"].is_boolean()) {
            apply_if_unset("--judder-detection", enable_judder, j["enable_judder_detection"].get<bool>());
        }
        if (j.contains("enable_audio_glitch") && j["enable_audio_glitch"].is_boolean()) {
            apply_if_unset("--audio-trigger", enable_audio, j["enable_audio_glitch"].get<bool>());
        }
        if (j.contains("enable_pii_redaction") && j["enable_pii_redaction"].is_boolean()) {
            apply_if_unset("--redact", redact, j["enable_pii_redaction"].get<bool>());
        }
        if (j.contains("auto_save_dir") && j["auto_save_dir"].is_string()) {
            apply_if_unset("--output-dir", output_dir, j["auto_save_dir"].get<std::string>());
        }
        if (j.contains("provider_tier") && j["provider_tier"].is_string()) {
            apply_if_unset("--tier", provider_tier, j["provider_tier"].get<std::string>());
        }
        if (j.contains("min_osd_severity") && j["min_osd_severity"].is_string()) {
            apply_if_unset("--osd-min-severity", osd_min_severity_str, j["min_osd_severity"].get<std::string>());
        }
        if (j.contains("min_report_severity") && j["min_report_severity"].is_string()) {
            apply_if_unset("--min-report-severity", min_report_severity_str, j["min_report_severity"].get<std::string>());
            if (app.count("--min-report-severity") == 0) file_has_min_report_sev = true;
        }
        if (j.contains("present_threshold_manual") && j["present_threshold_manual"].is_boolean()) {
            file_present_threshold_manual = j["present_threshold_manual"].get<bool>();
        }
        if (j.contains("min_fps_threshold") && j["min_fps_threshold"].is_number()) {
            double fps = j["min_fps_threshold"].get<double>();
            if (fps > 0.0) {
                apply_if_unset("--present-threshold-ms", present_threshold_ms, fps_to_present_threshold_ms(fps));
                if (app.count("--present-threshold-ms") == 0) {
                    file_present_threshold_manual = true;
                }
            }
        }
        if (j.contains("pacing_profile") && j["pacing_profile"].is_string()) {
            std::string pp = j["pacing_profile"].get<std::string>();
            if (pp == "custom") {
                if (app.count("--pacing-profile") == 0) {
                    file_has_pacing_profile = true;
                    file_pacing_profile_is_custom = true;
                }
            } else {
                apply_if_unset("--pacing-profile", pacing_profile_str, pp);
                if (app.count("--pacing-profile") == 0) file_has_pacing_profile = true;
            }
        }
        if (j.contains("frame_trigger_mode") && j["frame_trigger_mode"].is_string()) {
            apply_if_unset("--trigger-mode", trigger_mode_str, j["frame_trigger_mode"].get<std::string>());
        }
        if (j.contains("detection_preset") && j["detection_preset"].is_string()) {
            std::string dp = j["detection_preset"].get<std::string>();
            file_preset = detection_preset_from_string(dp);
            has_file_preset = true;
            if (app.count("--preset") == 0) {
                preset_str = dp;
            }
        }

        // Process targeting from config file: CLI targeting beats file targeting
        if (app.count("--target-pid") == 0 && app.count("--target-process") == 0) {
            if (j.contains("target_process_name") && j["target_process_name"].is_string()) {
                target_process_name = j["target_process_name"].get<std::string>();
            } else if (j.contains("last_target_process") && j["last_target_process"].is_string()) {
                target_process_name = j["last_target_process"].get<std::string>();
            }
        }

        // Post-merge range validation for the 19 ranged options
        struct RangedParam {
            const char* json_key;
            const char* cli_flag;
            double val;
            double min_v;
            double max_v;
        };

        const RangedParam ranged_params[] = {
            {"window_pre_ms", "--window-ms", window_pre_ms, 50.0, 1000.0},
            {"window_post_ms", "--post-trigger-ms", window_post_ms, 0.0, 200.0},
            {"cooldown_ms", "--cooldown-ms", cooldown_ms, 100.0, 10000.0},
            {"buffer_slots", "--buffer-slots", static_cast<double>(buffer_slots), static_cast<double>(MIN_BUFFER_SLOTS), static_cast<double>(MAX_BUFFER_SLOTS)},
            {"present_threshold_ms", "--present-threshold-ms", present_threshold_ms, 2.0, 200.0},
            {"spike_multiplier", "--spike-multiplier", spike_multiplier, 1.2, 10.0},
            {"min_spike_delta_ms", "--min-spike-delta-ms", min_spike_delta_ms, 1.0, 50.0},
            {"judder_swing_ratio", "--judder-swing-ratio", judder_swing_ratio, 0.1, 1.0},
            {"judder_min_alternations", "--judder-min-alternations", static_cast<double>(judder_min_alternations), 1.0, 50.0},
            {"dpc_threshold_us", "--dpc-threshold-us", static_cast<double>(dpc_threshold_us), 100.0, 50000.0},
            {"isr_threshold_us", "--isr-threshold-us", static_cast<double>(isr_threshold_us), 50.0, 50000.0},
            {"disk_threshold_ms", "--disk-threshold-ms", static_cast<double>(disk_threshold_ms), 1.0, 1000.0},
            {"cswitch_preempt_ms", "--cswitch-threshold-ms", static_cast<double>(cswitch_preempt_ms), 1.0, 500.0},
            {"smi_severity_threshold_ms", "--smi-threshold-ms", smi_severity_threshold_ms, 10.0, 100.0},
            {"d3d12_pso_threshold_ms", "--d3d12-pso-threshold-ms", static_cast<double>(d3d12_pso_threshold_ms), 1.0, 500.0},
            {"vram_demoted_threshold_mb", "--vram-threshold-mb", static_cast<double>(vram_demoted_threshold_mb), 1.0, 1024.0},
            {"mem_alloc_threshold_mb", "--mem-alloc-threshold-mb", static_cast<double>(mem_alloc_threshold_mb), 1.0, 1024.0},
            {"mem_trim_threshold_mb", "--mem-trim-threshold-mb", static_cast<double>(mem_trim_threshold_mb), 1.0, 1024.0},
            {"mem_physical_latency_us", "--mem-physical-latency-us", static_cast<double>(mem_physical_latency_us), 50.0, 50000.0}
        };

        for (const auto& rp : ranged_params) {
            if (rp.val < rp.min_v || rp.val > rp.max_v) {
                err << "[STUTTOMETER] Error: Config value for '" << rp.json_key << "' (" << rp.cli_flag
                    << ") out of range: " << rp.val << " (valid: " << rp.min_v << " - " << rp.max_v << ")\n";
                return CliParseResult::ERROR_USAGE;
            }
        }
    }

    // Check conflict between --dump-events - and --version
    if (dump_events_path == "-" && print_version) {
        err << "[STUTTOMETER] Error: --dump-events - cannot be combined with --version.\n";
        return CliParseResult::ERROR_USAGE;
    }

    // Precedence: --version beats --self-check
    if (print_version) {
        out << TOOL_NAME << " v" << TOOL_VERSION << "\n";
        return CliParseResult::EXIT_HANDLED;
    }

    // Pacing profile resolution, alias, and overrides
    bool has_preset = (app.count("--preset") > 0);
    bool has_osd_min_sev = (app.count("--osd-min-severity") > 0);
    bool has_judder_alt = (app.count("--judder-min-alternations") > 0);
    bool has_min_report_sev = (app.count("--min-report-severity") > 0);
    bool has_pacing_profile = (app.count("--pacing-profile") > 0);
    bool has_high_refresh = (app.count("--high-refresh") > 0);
    bool has_spike_mult = (app.count("--spike-multiplier") > 0);
    bool has_min_delta = (app.count("--min-spike-delta-ms") > 0);
    bool has_swing_ratio = (app.count("--judder-swing-ratio") > 0);

    const bool has_judder_alt_effective = has_judder_alt || file_has_judder_alt;
    const bool has_swing_ratio_effective = has_swing_ratio || file_has_swing_ratio;
    const bool has_pacing_profile_effective = has_pacing_profile || file_has_pacing_profile;
    const bool has_spike_mult_effective = has_spike_mult || file_has_spike_mult;
    const bool has_min_delta_effective = has_min_delta || file_has_min_delta;
    const bool has_min_report_sev_effective = has_min_report_sev || file_has_min_report_sev;

    // Unconditional source-agnostic validation of merged enum strings
    if (preset_str != "balanced" && preset_str != "competitive" &&
        preset_str != "conservative" && preset_str != "forensic" &&
        preset_str != "custom") {
        err << "[STUTTOMETER] Error: Invalid detection preset '" << preset_str
            << "'. Must be 'balanced', 'competitive', 'conservative', 'forensic', or 'custom'.\n";
        return CliParseResult::ERROR_USAGE;
    }

    if (osd_min_severity_str != "all" && osd_min_severity_str != "warning" && osd_min_severity_str != "danger") {
        err << "[STUTTOMETER] Error: Invalid OSD min severity '" << osd_min_severity_str
            << "'. Must be 'all', 'warning', or 'danger'.\n";
        return CliParseResult::ERROR_USAGE;
    }

    if (min_report_severity_str != "all" && min_report_severity_str != "warning" && min_report_severity_str != "danger") {
        err << "[STUTTOMETER] Error: Invalid min report severity '" << min_report_severity_str
            << "'. Must be 'all', 'warning', or 'danger'.\n";
        return CliParseResult::ERROR_USAGE;
    }

    // 1. Apply --preset (or BALANCED default)
    DetectionPreset resolved_preset = detection_preset_from_string(preset_str);
    TriggerConfig trig_defaults{};
    GuiConfig gui_defaults{};
    apply_detection_preset(resolved_preset, trig_defaults, gui_defaults);

    PacingProfile resolved_profile = trig_defaults.pacing_profile;
    ReportSeverity resolved_min_report_sev = trig_defaults.min_report_severity;
    if (has_min_report_sev_effective) {
        resolved_min_report_sev = report_severity_from_string(min_report_severity_str);
    }
    if (!has_judder_alt_effective) {
        judder_min_alternations = trig_defaults.judder_min_alternations;
    }
    if (!has_swing_ratio_effective) {
        judder_swing_ratio = trig_defaults.judder_swing_ratio;
    }

    // 2. Apply explicit or file-sourced pacing profile
    if (file_pacing_profile_is_custom && app.count("--pacing-profile") == 0) {
        resolved_profile = PacingProfile::CUSTOM;
    } else if (has_pacing_profile_effective) {
        auto opt = pacing_profile_from_cli_string(pacing_profile_str);
        if (!opt.has_value()) {
            err << "[STUTTOMETER] Error: Invalid --pacing-profile '" << pacing_profile_str
                << "'. Must be 'auto', 'high-refresh', or 'conservative'.\n";
            return CliParseResult::ERROR_USAGE;
        }
        resolved_profile = *opt;
        if (has_high_refresh) {
            err << "[Config] Note: --pacing-profile took precedence over --high-refresh\n";
        }
    } else if (has_high_refresh) {
        resolved_profile = PacingProfile::HIGH_REFRESH;
    } else if (has_spike_mult_effective || has_min_delta_effective) {
        resolved_profile = PacingProfile::CUSTOM;
    }

    // 3. Apply explicit threshold flags
    // 4. If explicit pacing overrides are supplied on CLI and preset is not CUSTOM, flip to CUSTOM
    const bool has_pacing_override_cli = (has_pacing_profile || has_high_refresh ||
                                          has_spike_mult || has_min_delta ||
                                          has_swing_ratio || has_judder_alt);
    if (has_pacing_override_cli) {
        if (has_preset && resolved_preset != DetectionPreset::CUSTOM) {
            resolved_preset = DetectionPreset::CUSTOM;
            err << "[Config] Note: Explicit parameter override active; operating in CUSTOM preset.\n";
        } else if (!has_preset) {
            resolved_preset = DetectionPreset::CUSTOM;
        }
    }

    // Evaluate preset integrity for file-sourced preset (D3 Option A)
    if (has_file_preset && !has_preset && resolved_preset != DetectionPreset::CUSTOM) {
        TriggerConfig expected_tc{};
        GuiConfig expected_gc{};
        apply_detection_preset(resolved_preset, expected_tc, expected_gc);
        const bool preset_intact =
            (resolved_profile == expected_gc.pacing_profile) &&
            (std::abs(judder_swing_ratio - expected_gc.judder_swing_ratio) < 1e-9) &&
            (judder_min_alternations == expected_gc.judder_min_alternations);
        if (!preset_intact) {
            err << "[Config] Note: Config file values diverge from preset '"
                << detection_preset_to_string(resolved_preset)
                << "'; demoting to CUSTOM.\n";
            resolved_preset = DetectionPreset::CUSTOM;
        }
    }

    if (resolved_preset == DetectionPreset::CUSTOM) {
        const bool any_override =
            has_pacing_override_cli ||
            file_has_spike_mult || file_has_min_delta || file_has_swing_ratio ||
            file_has_judder_alt || file_has_pacing_profile ||
            (app.count("--present-threshold-ms") > 0) || file_present_threshold_manual ||
            (app.count("--min-report-severity") > 0) || file_has_min_report_sev;
        if (!any_override) {
            err << "[Config] Warning: --preset custom supplies no values on its own. "
                << "Using CUSTOM preset defaults (auto-adaptive pacing, 2.0x spike, 4.0ms delta). "
                << "Supply explicit overrides (e.g. --spike-multiplier, --min-spike-delta-ms) to customize.\n";
        }
    }

    if ((has_spike_mult_effective || has_min_delta_effective) && (has_pacing_profile_effective || has_high_refresh)) {
        resolved_profile = PacingProfile::CUSTOM;
        err << "[Config] Note: Explicit parameter override active; operating in CUSTOM profile.\n";
    }

    // Assign multiplier & delta based on resolved profile
    if (resolved_profile == PacingProfile::AUTO_ADAPTIVE) {
        if (!has_spike_mult_effective) spike_multiplier = DEFAULT_SPIKE_MULTIPLIER;
        if (!has_min_delta_effective) min_spike_delta_ms = DEFAULT_MIN_SPIKE_DELTA_MS;
    } else if (resolved_profile == PacingProfile::HIGH_REFRESH) {
        if (!has_spike_mult_effective) spike_multiplier = HIGH_REFRESH_SPIKE_MULTIPLIER;
        if (!has_min_delta_effective) min_spike_delta_ms = HIGH_REFRESH_MIN_DELTA_MS;
    } else if (resolved_profile == PacingProfile::CONSERVATIVE) {
        if (!has_spike_mult_effective) spike_multiplier = CONSERVATIVE_SPIKE_MULTIPLIER;
        if (!has_min_delta_effective) min_spike_delta_ms = CONSERVATIVE_MIN_DELTA_MS;
    } else if (resolved_profile == PacingProfile::CUSTOM) {
        if (!has_spike_mult_effective) spike_multiplier = DEFAULT_SPIKE_MULTIPLIER;
        if (!has_min_delta_effective) min_spike_delta_ms = DEFAULT_MIN_SPIKE_DELTA_MS;
    }

    // Notice when --trigger-mode static is combined with pacing profile
    if (trigger_mode_str == "static" && (has_pacing_profile_effective || has_high_refresh)) {
        err << "[Config] Note: --pacing-profile has no effect in static-only frame trigger mode.\n";
    }

    if ((app.count("--present-threshold-ms") > 0 || file_present_threshold_manual) && present_threshold_ms < 12.0) {
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
        return CliParseResult::ERROR_USAGE;
    }
    if (output_file == "-") {
        err << "[STUTTOMETER] Error: --output does not support stdout ('-'); must specify a file path "
            << "(use --dump-events - for stdout streaming).\n";
        return CliParseResult::ERROR_USAGE;
    }

    // Data-loss-safety notices routed to err (NOT out) so they survive --quiet.
    if (!output_file.empty() && max_reports != 1 && output_dir.empty()) {
        err << "[STUTTOMETER] Notice: --output file will be overwritten with the latest "
            << "report on each trigger (use --output-dir to save all reports).\n";
    }
    if (!export_csv_path.empty() && max_reports != 1 && output_dir.empty()) {
        err << "[STUTTOMETER] Notice: --export-csv file will be overwritten with the latest "
            << "frame timeline on each trigger (use --output-dir for per-trigger CSV files).\n";
    }

    // Unconditional source-agnostic validation of trigger mode and tier
    const std::set<std::string> valid_trigger_modes = { "hybrid", "dynamic", "static" };
    if (valid_trigger_modes.find(trigger_mode_str) == valid_trigger_modes.end()) {
        err << "[STUTTOMETER] Error: Invalid trigger mode '" << trigger_mode_str << "'. Must be 'hybrid', 'dynamic', or 'static'.\n";
        return CliParseResult::ERROR_USAGE;
    }

    const std::set<std::string> valid_tiers = { "minimal", "standard", "full" };
    if (valid_tiers.find(provider_tier) == valid_tiers.end()) {
        err << "[STUTTOMETER] Error: Invalid provider tier '" << provider_tier << "'. Must be 'minimal', 'standard', or 'full'.\n";
        return CliParseResult::ERROR_USAGE;
    }

    if (target_process_name.size() > 260) {
        err << "[STUTTOMETER] Error: --target-process name exceeds maximum length (260 characters).\n";
        return CliParseResult::ERROR_USAGE;
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
    out_config.quiet = quiet;
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
    out_config.preset = resolved_preset;
    out_config.preset_manual = has_preset;
    out_config.min_report_severity = resolved_min_report_sev;
    out_config.min_report_severity_manual = has_min_report_sev;
    out_config.osd_min_severity = report_severity_from_string(osd_min_severity_str);
    out_config.osd_min_severity_manual = has_osd_min_sev;
    out_config.judder_min_alternations = static_cast<uint8_t>(judder_min_alternations);
    out_config.judder_min_alternations_manual = has_judder_alt;
    out_config.dwm_min_missed_vblanks = trig_defaults.dwm_min_missed_vblanks;
    out_config.kernel_frame_stall_min_missed_vblanks = trig_defaults.kernel_frame_stall_min_missed_vblanks;
    out_config.target_pid_manual = (app.count("--target-pid") > 0);
    out_config.present_threshold_manual = (app.count("--present-threshold-ms") > 0) || file_present_threshold_manual;
    out_config.smi_threshold_manual = (app.count("--smi-threshold-ms") > 0) || file_smi_threshold_manual;

    if (dump_effective_config) {
        nlohmann::json eff;
        eff["preset"] = detection_preset_to_string(out_config.preset);
        eff["pacing_profile"] = pacing_profile_to_string(out_config.pacing_profile);
        eff["trigger_mode"] = out_config.trigger_mode_str;
        eff["spike_multiplier"] = out_config.spike_multiplier;
        eff["min_spike_delta_ms"] = out_config.min_spike_delta_ms;
        eff["present_threshold_ms"] = out_config.present_threshold_ms;
        eff["present_threshold_manual"] = out_config.present_threshold_manual;
        eff["judder_swing_ratio"] = out_config.judder_swing_ratio;
        eff["judder_min_alternations"] = out_config.judder_min_alternations;
        eff["enable_judder"] = out_config.enable_judder;
        eff["enable_audio"] = out_config.enable_audio;
        eff["window_pre_ms"] = out_config.window_pre_ms;
        eff["window_post_ms"] = out_config.window_post_ms;
        eff["cooldown_ms"] = out_config.cooldown_ms;
        eff["buffer_slots"] = out_config.buffer_slots;
        eff["target_pid"] = out_config.target_pid;
        eff["target_pid_manual"] = out_config.target_pid_manual;
        eff["target_process_name"] = out_config.target_process_name;
        eff["output_file"] = out_config.output_file;
        eff["output_dir"] = out_config.output_dir;
        eff["max_reports"] = out_config.max_reports;
        eff["provider_tier"] = out_config.provider_tier;
        eff["min_report_severity"] = report_severity_to_string(out_config.min_report_severity);
        eff["osd_min_severity"] = report_severity_to_string(out_config.osd_min_severity);
        eff["dpc_threshold_us"] = out_config.dpc_threshold_us;
        eff["isr_threshold_us"] = out_config.isr_threshold_us;
        eff["disk_threshold_ms"] = out_config.disk_threshold_ms;
        eff["cswitch_preempt_ms"] = out_config.cswitch_preempt_ms;
        eff["smi_severity_threshold_ms"] = out_config.smi_severity_threshold_ms;
        eff["smi_threshold_manual"] = out_config.smi_threshold_manual;
        eff["d3d12_pso_threshold_ms"] = out_config.d3d12_pso_threshold_ms;
        eff["vram_demoted_threshold_mb"] = out_config.vram_demoted_threshold_mb;
        eff["mem_alloc_threshold_mb"] = out_config.mem_alloc_threshold_mb;
        eff["mem_trim_threshold_mb"] = out_config.mem_trim_threshold_mb;
        eff["mem_physical_latency_us"] = out_config.mem_physical_latency_us;
        eff["redact"] = out_config.redact;
        eff["verbose"] = out_config.verbose;
        eff["quiet"] = out_config.quiet;

        out << eff.dump(2) << "\n";
        return CliParseResult::EXIT_HANDLED;
    }

    return CliParseResult::OK;
}

} // namespace stuttometer
