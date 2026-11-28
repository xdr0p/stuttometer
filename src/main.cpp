#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/internal/session_helpers.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/csv_exporter.hpp"
#include "stuttometer/session_bundle.hpp"

#include "stuttometer/cli_parser.hpp"
#include "stuttometer/version.hpp"
#include "stuttometer/constants.hpp"
#include "stuttometer/internal/gui_constants.hpp"
#include "stuttometer/internal/process_watcher.hpp"
#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <set>
#include <mutex>
#include <condition_variable>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static std::atomic<bool> g_stop_requested{false};
static std::atomic<bool> g_shutdown_done{false};
static std::mutex g_shutdown_mutex;
static std::condition_variable g_shutdown_cv;
static stuttometer::ProcessWatcher g_watcher;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;
    } else if (ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_LOGOFF_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
        // Block until main() stops trace sessions or until 4s timeout expires to prevent orphaned kernel logger
        std::unique_lock<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_cv.wait_for(lock, std::chrono::milliseconds(4000), []() {
            return g_shutdown_done.load(std::memory_order_acquire);
        });
        return TRUE;
    }
    return FALSE;
}

namespace {
// Thread-safety note: s_null_stream is used only from main() thread.
class NullStreambuf : public std::streambuf {
    int overflow(int c) override { return c; }
};
NullStreambuf s_null_buf;
std::ostream  s_null_stream(&s_null_buf);
std::ostream* g_info_stream = &std::cout;
}

int main(int argc, char** argv) {
    stuttometer::CliConfig config;
    auto parse_res = stuttometer::parse_cli_args(argc, argv, config, std::cout, std::cerr);
    if (parse_res == stuttometer::CliParseResult::EXIT_HANDLED) {
        return 0;
    }
    if (parse_res == stuttometer::CliParseResult::ERROR_USAGE) {
        return 2;
    }

    if (config.run_self_check) {
        if (config.dump_events_path == "-") {
            std::cerr << "info: [STUTTOMETER] Self-check output redirected to stderr because --dump-events - is active\n";
            bool ok = stuttometer::run_environment_self_check(std::cerr);
            return ok ? 0 : 3;
        } else {
            bool ok = stuttometer::run_environment_self_check(std::cout);
            return ok ? 0 : 3;
        }
    }

    double window_pre_ms = config.window_pre_ms;
    double window_post_ms = config.window_post_ms;
    double present_threshold_ms = config.present_threshold_ms;
    bool enable_audio = config.enable_audio;
    double cooldown_ms = config.cooldown_ms;
    uint32_t buffer_slots = config.buffer_slots;
    uint32_t target_pid = config.target_pid;
    std::string target_process_name = config.target_process_name;
    std::string output_file = config.output_file;
    std::string output_dir = config.output_dir;
    uint32_t max_reports = config.max_reports;
    std::string provider_tier = config.provider_tier;
    bool redact = config.redact;
    bool verbose = config.verbose;
    bool quiet = config.quiet;
    std::ostream& info = quiet ? s_null_stream : std::cout;
    g_info_stream = &info;
    std::string trigger_mode_str = config.trigger_mode_str;
    stuttometer::PacingProfile pacing_profile = config.pacing_profile;
    double spike_multiplier = config.spike_multiplier;
    double min_spike_delta_ms = config.min_spike_delta_ms;
    bool enable_judder = config.enable_judder;
    double judder_swing_ratio = config.judder_swing_ratio;
    uint8_t judder_min_alternations = config.judder_min_alternations;
    std::string dump_events_path = config.dump_events_path;
    size_t dump_max_mb = config.dump_max_mb;
    size_t dump_max_files = config.dump_max_files;
    std::string export_csv_path = config.export_csv_path;
    const bool target_pid_manual = config.target_pid_manual;

    if (!output_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            std::cerr << "error: [STUTTOMETER] Failed to create output directory '" << output_dir << "': " << ec.message() << "\n";
            return 1;
        }
    }
    if (!output_file.empty()) {
        std::filesystem::path p(output_file);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    struct CoutRedirectGuard {
        std::streambuf* old_rdbuf{nullptr};
        ~CoutRedirectGuard() {
            if (old_rdbuf) {
                std::cout.rdbuf(old_rdbuf);
            }
        }
    } cout_guard;

    if (dump_events_path == "-") {
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        cout_guard.old_rdbuf = std::cout.rdbuf(std::cerr.rdbuf());
    }

    std::unique_ptr<stuttometer::NdjsonWriter> ndjson_writer;
    if (!dump_events_path.empty()) {
        if (dump_events_path == "-") {
            ndjson_writer = stuttometer::NdjsonWriter::create_for_stream(stdout);
        } else {
            ndjson_writer = stuttometer::NdjsonWriter::create_for_file(
                dump_events_path,
                dump_max_mb * 1024 * 1024,
                dump_max_files
            );
            if (!ndjson_writer) {
                std::cerr << "error: [STUTTOMETER] Failed to create NDJSON output file: " << dump_events_path << "\n";
                return 1;
            }
        }
    }

    const bool is_admin = stuttometer::is_running_as_admin();
    if (!is_admin) {
        std::cerr << "\nerror: [STUTTOMETER] Running in Standard (Non-Elevated) Mode.\n";
        std::cerr << "Kernel ETW providers (DPC, ISR, Disk I/O, Context Switches) require Administrator privileges.\n\n";
        std::cerr << "To verify your system ETW providers and environment before elevating, run:\n";
        std::cerr << "  .\\stuttometer.exe --self-check\n\n";
        std::cerr << "To run live capture, please launch PowerShell as Administrator and run:\n";
        std::cerr << "  .\\stuttometer.exe [OPTIONS]\n\n";
        return 1;
    }

    if (!target_pid_manual && !target_process_name.empty()) {
        target_pid = stuttometer::resolve_process_name_to_pid(target_process_name);
        if (target_pid != 0) {
            info << "info: [STUTTOMETER] Target process '" << target_process_name << "' matched PID " << target_pid << "\n";
        } else {
            info << "info: [STUTTOMETER] Target process '" << target_process_name << "' not currently running. Waiting for process to launch...\n";
        }
    }

    if (!stuttometer::enable_system_profile_privilege()) {
        std::cerr << "warn: [STUTTOMETER] Failed to enable SeSystemprofilePrivilege. Kernel trace session may fail or be degraded.\n";
    }

    info << "info: [STUTTOMETER] Initializing Stuttometer v" << stuttometer::TOOL_VERSION << " (Elevated Mode)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    stuttometer::EtwSessionConfig etw_config = stuttometer::session_helpers::make_etw_config(provider_tier, enable_audio);

    const uint32_t requested_slots = buffer_slots;
    buffer_slots = stuttometer::compute_recommended_buffer_slots(etw_config, requested_slots);
    if (buffer_slots > requested_slots) {
        info << "info: [STUTTOMETER] Buffer capacity automatically bumped to " << buffer_slots << " slots for active providers\n";
    }

    stuttometer::FlightRecorder flight_recorder(buffer_slots);
    if (flight_recorder.capacity() != buffer_slots) {
        info << "info: [STUTTOMETER] Buffer capacity rounded up to " << flight_recorder.capacity() << " slots\n";
    }

    stuttometer::FrameTriggerMode frame_trig_mode = stuttometer::FrameTriggerMode::HYBRID;
    if (trigger_mode_str == "dynamic") frame_trig_mode = stuttometer::FrameTriggerMode::DYNAMIC_ONLY;
    else if (trigger_mode_str == "static") frame_trig_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;

    stuttometer::TriggerConfig trig_config;
    trig_config.window_pre_ms = window_pre_ms;
    trig_config.window_post_ms = window_post_ms;
    trig_config.present_threshold_ms = present_threshold_ms;
    trig_config.audio_trigger_enabled = enable_audio;
    trig_config.cooldown_ms = cooldown_ms;
    trig_config.target_pid = target_pid;
    trig_config.target_process_name = target_process_name;
    trig_config.frame_trigger_mode = frame_trig_mode;
    trig_config.pacing_profile = pacing_profile;
    trig_config.spike_multiplier = spike_multiplier;
    trig_config.min_spike_delta_ms = min_spike_delta_ms;
    trig_config.enable_judder_detection = enable_judder;
    trig_config.judder_swing_ratio = judder_swing_ratio;
    trig_config.judder_min_alternations = judder_min_alternations;
    trig_config.min_report_severity = config.min_report_severity;
    trig_config.dwm_min_missed_vblanks = config.dwm_min_missed_vblanks;
    trig_config.kernel_frame_stall_min_missed_vblanks = config.kernel_frame_stall_min_missed_vblanks;

    stuttometer::DisplayRefreshInfo disp_info = stuttometer::query_display_refresh_info(target_pid);
    trig_config.vblank_interval_ms = disp_info.vblank_interval_ms;
    if (!config.present_threshold_manual) {
        present_threshold_ms = disp_info.vblank_interval_ms;
        trig_config.present_threshold_ms = present_threshold_ms;
    }
    if (disp_info.query_succeeded) {
        info << "info: [STUTTOMETER] Auto-detected display refresh: " 
             << disp_info.refresh_rate_hz << " Hz (vblank: " 
             << std::fixed << std::setprecision(2) << disp_info.vblank_interval_ms << " ms)\n";
    } else {
        info << "info: [STUTTOMETER] Notice: Display refresh detection unavailable; defaulting to 60.0 Hz ("
             << stuttometer::DEFAULT_60HZ_VBLANK_MS << " ms).\n";
    }
    if (config.present_threshold_manual && (present_threshold_ms > stuttometer::VBLANK_WARNING_FACTOR * disp_info.vblank_interval_ms)) {
        info << "info: [STUTTOMETER] Notice: Configured stutter threshold (" 
             << std::fixed << std::setprecision(1) << present_threshold_ms << " ms) is >"
             << std::setprecision(1) << stuttometer::VBLANK_WARNING_FACTOR << "x "
             << "the detected display refresh interval ("
             << disp_info.vblank_interval_ms << " ms, "
             << std::setprecision(0) << disp_info.refresh_rate_hz << " Hz). "
             << "Stutters under " << std::setprecision(1) << present_threshold_ms << " ms will not be reported.\n";
    }

    stuttometer::TriggerEngine trigger_engine(trig_config, qpc_freq);
    stuttometer::EtwSessionManager session_mgr(flight_recorder, trigger_engine, etw_config);
    if (ndjson_writer) {
        session_mgr.set_ndjson_writer(ndjson_writer.get());
    }

    const auto start_result = session_mgr.start();
    if (start_result == stuttometer::SessionStartResult::FAILED) {
        std::cerr << "error: [STUTTOMETER] Failed to start ETW sessions.\n";
        return 1;
    } else if (start_result == stuttometer::SessionStartResult::DEGRADED_USER_ONLY) {
        info << "info: [STUTTOMETER] Notice: Running in DEGRADED USER-ONLY mode (Kernel trace session unavailable).\n";
    } else if (start_result == stuttometer::SessionStartResult::DEGRADED_KERNEL_ONLY) {
        info << "info: [STUTTOMETER] Notice: Running in DEGRADED KERNEL-ONLY mode (User DXGI/Audio session unavailable).\n";
    }

    const auto ps = session_mgr.provider_status();
    auto warn_provider = [&info](bool active, bool requested, const char* name) {
        if (requested && !active) {
            info << "warn: [STUTTOMETER] Notice: " << name << " provider failed to enable.\n";
        }
    };
    warn_provider(ps.dxgi_enabled, etw_config.enable_dxgi, "DXGI");
    warn_provider(ps.audio_enabled, etw_config.enable_audio, "Audio");
    warn_provider(ps.dxgkrnl_enabled, etw_config.enable_dxgkrnl, "DxgKrnl");
    warn_provider(ps.dwm_core_enabled, etw_config.enable_dwm_core, "DWM-Core");
    warn_provider(ps.kernel_dpc_enabled, etw_config.enable_kernel_dpc, "Kernel DPC/ISR");
    warn_provider(ps.kernel_disk_enabled, etw_config.enable_kernel_disk, "Kernel Disk I/O");
    warn_provider(ps.kernel_cswitch_enabled, etw_config.enable_kernel_cswitch, "Kernel Context Switch");
    warn_provider(ps.kernel_pagefault_enabled, etw_config.enable_kernel_pagefault, "Kernel Page Fault");
    warn_provider(ps.processor_power_enabled, etw_config.enable_processor_power, "Processor Power");
    warn_provider(ps.antimalware_enabled, etw_config.enable_antimalware, "Antimalware Engine");
    warn_provider(ps.d3d12_enabled, etw_config.enable_d3d12, "Direct3D 12");
    warn_provider(ps.kernel_memory_enabled, etw_config.enable_kernel_memory, "Kernel Memory");
    warn_provider(ps.kernel_process_enabled, etw_config.enable_kernel_process_events, "Kernel Process");

    info << "info: [STUTTOMETER] Active. Monitoring frame delivery (Mode: " << stuttometer::frame_trigger_mode_to_string(frame_trig_mode)
         << ", Spike: " << spike_multiplier << "x, Static Threshold: " << present_threshold_ms << "ms)...\n";
    info << "Press Ctrl+C to stop.\n\n";

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelatorThresholds thresholds = stuttometer::session_helpers::make_correlator_thresholds(config);

    stuttometer::CorrelationEngine correlator(driver_resolver, thresholds);
    stuttometer::JsonReporter reporter;

    stuttometer::SessionBundleWriter bundle_writer;
    if (!config.session_file.empty() || (!output_dir.empty() && config.auto_save_mode == "session_bundle")) {
        const nlohmann::json cfg_snap = {
            {"provider_tier", provider_tier},
            {"window_pre_ms", window_pre_ms},
            {"window_post_ms", window_post_ms},
            {"cooldown_ms", cooldown_ms},
            {"present_threshold_ms", present_threshold_ms},
            {"hardware_vblank_ms", trig_config.vblank_interval_ms},
            {"target_process", target_process_name},
            {"output_dir", output_dir},
            {"session_file", config.session_file}
        };
        if (!config.session_file.empty()) {
            if (!bundle_writer.open(std::filesystem::path(config.session_file), cfg_snap, redact)) {
                std::cerr << "error: [STUTTOMETER] Failed to open session file '" << config.session_file << "'\n";
            }
        } else {
            if (!bundle_writer.open_in_dir(std::filesystem::path(output_dir), cfg_snap, redact)) {
                std::cerr << "error: [STUTTOMETER] Failed to open session bundle in '" << output_dir << "'\n";
            }
        }
    }

    const uint64_t session_start_qpc = stuttometer::get_current_qpc();
    uint32_t report_count = 0;
    uint32_t loop_counter = 0;

    // Dedicated background thread for CLI process name watcher (eliminates main loop jitter)
    if (!target_pid_manual && !target_process_name.empty()) {
        stuttometer::WatcherCallbacks callbacks;
        callbacks.user_data = &trigger_engine;
        callbacks.try_attach = [](uint32_t pid, void* ud) -> bool {
            return static_cast<stuttometer::TriggerEngine*>(ud)->try_attach_pid(pid);
        };
        callbacks.try_detach = [](uint32_t pid, void* ud) -> bool {
            return static_cast<stuttometer::TriggerEngine*>(ud)->try_detach_pid(pid);
        };
        callbacks.is_waiting = [](void* ud) -> bool {
            return static_cast<stuttometer::TriggerEngine*>(ud)->is_target_waiting();
        };
        callbacks.get_active_pid = [](void* ud) -> uint32_t {
            return static_cast<stuttometer::TriggerEngine*>(ud)->active_target_pid();
        };
        callbacks.on_attach_success = [](uint32_t pid, std::string_view name, void*) {
            (*g_info_stream) << "info: [STUTTOMETER] Target process '" << name << "' active (PID " << pid << ")\n";
        };
        callbacks.on_detach_success = [](uint32_t, std::string_view name, void*) {
            (*g_info_stream) << "info: [STUTTOMETER] Target process '" << name << "' closed. Waiting for restart...\n";
        };
        g_watcher.start(target_process_name, callbacks);
    }

    std::vector<stuttometer::EtwEventRecord> snapshot_buffer;
    snapshot_buffer.reserve(4096);

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const uint64_t current_qpc = stuttometer::get_current_qpc();
        stuttometer::TriggerInfo trigger_info;
        uint64_t from_qpc = 0;
        uint64_t to_qpc = 0;

        ++loop_counter;

        if (verbose && loop_counter % stuttometer::gui_constants::VERBOSE_LOG_INTERVAL_LOOPS == 0) {
            info << "[VERBOSE] Head: " << flight_recorder.current_head() 
                 << " | Upstream Lost Events: " << session_mgr.events_lost()
                 << " | Lost Buffers: " << session_mgr.buffers_lost()
                 << " | Unpaired Evictions: " << session_mgr.unpaired_evictions()
                 << " | Insertion Failures: " << session_mgr.insertion_failures()
                 << " | Suppressed Triggers: " << trigger_engine.suppressed_trigger_count() << "\n";
        }

        if (ndjson_writer) {
            stuttometer::session_helpers::drain_filtered_events(trigger_engine, *ndjson_writer, 64);
        }

        if (trigger_engine.poll_state(current_qpc, trigger_info, from_qpc, to_qpc)) {
            struct ReportScopeGuard {
                stuttometer::TriggerEngine& engine;
                ~ReportScopeGuard() {
                    engine.on_report_completed(stuttometer::get_current_qpc());
                }
            } guard{trigger_engine};

            try {
                uint64_t drops = 0;
                stuttometer::session_helpers::capture_trigger_snapshot(session_mgr, flight_recorder, from_qpc, to_qpc, snapshot_buffer, drops);

                stuttometer::ProviderContext p_ctx = stuttometer::session_helpers::build_provider_context(session_mgr, etw_config);

                uint64_t unpaired_evicts = session_mgr.unpaired_evictions();
                uint64_t ins_failures = session_mgr.insertion_failures();

                const double effective_post_ms = (to_qpc > trigger_info.trigger_timestamp_qpc)
                    ? stuttometer::qpc_delta_to_ms(to_qpc - trigger_info.trigger_timestamp_qpc, qpc_freq)
                    : window_post_ms;

                stuttometer::CorrelateOptions correlate_opts{
                    .window_pre_ms = window_pre_ms,
                    .window_post_ms = effective_post_ms,
                    .present_threshold_ms = present_threshold_ms,
                    .hardware_vblank_ms = trig_config.vblank_interval_ms,
                    .provider_tier = provider_tier,
                    .redact = redact,
                    .judder_swing_ratio = judder_swing_ratio,
                    .enable_transition_salience = config.enable_transition_salience
                };

                auto report = correlator.correlate(snapshot_buffer, trigger_info, qpc_freq, correlate_opts, p_ctx, drops, unpaired_evicts, ins_failures, flight_recorder.total_dropped_events());

                reporter.print_console_summary(report, info, redact);

                if (!output_file.empty()) {
                    if (!reporter.save_to_file(report, std::filesystem::path(output_file), redact)) {
                        std::cerr << "error: [STUTTOMETER] Failed to write report to '" << output_file << "'\n";
                    }
                }
                if (!export_csv_path.empty()) {
                    if (!stuttometer::csv::export_to_file(report, std::filesystem::path(export_csv_path))) {
                        std::cerr << "error: [STUTTOMETER] Failed to export CSV to '" << export_csv_path << "'\n";
                    }
                }
                if (bundle_writer.is_open()) {
                    if (!bundle_writer.write_report(report, redact)) {
                        std::cerr << "error: [STUTTOMETER] Failed to append report to session bundle\n";
                    }
                }
                if (!output_dir.empty() && config.auto_save_mode == "per_trigger") {
                    stuttometer::session_helpers::save_per_trigger_reports(
                        report, std::filesystem::path(output_dir), report_count, current_qpc, redact, reporter, "error: [STUTTOMETER] ");
                }

                ++report_count;

                if (max_reports > 0 && report_count >= max_reports) {
                    break;
                }
            } catch (const std::exception& ex) {
                std::cerr << "error: [STUTTOMETER] Exception during report processing: " << ex.what() << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(stuttometer::gui_constants::SESSION_LOOP_SLEEP_MS));
    }

    g_watcher.stop();

    info << "\ninfo: [STUTTOMETER] Stopping trace sessions and cleaning up...\n";
    session_mgr.stop();
    if (bundle_writer.is_open()) {
        const double session_dur_ms = stuttometer::qpc_delta_to_ms(stuttometer::get_current_qpc() - session_start_qpc, qpc_freq);
        bundle_writer.write_cli_summary(report_count, session_dur_ms);
        bundle_writer.close();
    }
    if (ndjson_writer) {
        stuttometer::session_helpers::drain_filtered_events(trigger_engine, *ndjson_writer);
        ndjson_writer->stop();
        info << "info: [STUTTOMETER] NDJSON writer: " << ndjson_writer->written_records()
             << " written, " << ndjson_writer->dropped_records() << " dropped.\n";
    }
    info << "info: [STUTTOMETER] Done. Total reports generated: " << report_count << "\n";

    {
        std::lock_guard<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_done.store(true, std::memory_order_release);
    }
    g_shutdown_cv.notify_all();

    return 0;
}
