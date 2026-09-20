#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/csv_exporter.hpp"

#include "stuttometer/cli_parser.hpp"
#include <iostream>
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

int main(int argc, char** argv) {
    stuttometer::CliConfig config;
    auto parse_res = stuttometer::parse_cli_args(argc, argv, config, std::cout, std::cerr);
    if (parse_res == stuttometer::CliParseResult::EXIT_OK) {
        return 0;
    }
    if (parse_res == stuttometer::CliParseResult::EXIT_ERROR) {
        return 1;
    }

    if (config.run_self_check) {
        if (config.dump_events_path == "-") {
            std::cerr << "[STUTTOMETER] Self-check output redirected to stderr because --dump-events - is active\n";
            bool ok = stuttometer::run_environment_self_check(std::cerr);
            return ok ? 0 : 1;
        } else {
            bool ok = stuttometer::run_environment_self_check(std::cout);
            return ok ? 0 : 1;
        }
    }

    double window_pre_ms = config.window_pre_ms;
    double window_post_ms = config.window_post_ms;
    double present_threshold_ms = config.present_threshold_ms;
    bool enable_audio = config.enable_audio;
    double cooldown_ms = config.cooldown_ms;
    uint32_t dpc_threshold_us = config.dpc_threshold_us;
    uint32_t isr_threshold_us = config.isr_threshold_us;
    uint32_t disk_threshold_ms = config.disk_threshold_ms;
    uint32_t cswitch_preempt_ms = config.cswitch_preempt_ms;
    double smi_severity_threshold_ms = config.smi_severity_threshold_ms;
    uint32_t d3d12_pso_threshold_ms = config.d3d12_pso_threshold_ms;
    uint32_t vram_demoted_threshold_mb = config.vram_demoted_threshold_mb;
    uint32_t mem_alloc_threshold_mb = config.mem_alloc_threshold_mb;
    uint32_t mem_trim_threshold_mb = config.mem_trim_threshold_mb;
    uint32_t mem_physical_latency_us = config.mem_physical_latency_us;
    uint32_t buffer_slots = config.buffer_slots;
    uint32_t target_pid = config.target_pid;
    std::string target_process_name = config.target_process_name;
    std::string output_file = config.output_file;
    std::string output_dir = config.output_dir;
    uint32_t max_reports = config.max_reports;
    std::string provider_tier = config.provider_tier;
    bool redact = config.redact;
    bool verbose = config.verbose;
    std::string trigger_mode_str = config.trigger_mode_str;
    stuttometer::PacingProfile pacing_profile = config.pacing_profile;
    double spike_multiplier = config.spike_multiplier;
    double min_spike_delta_ms = config.min_spike_delta_ms;
    bool enable_judder = config.enable_judder;
    double judder_swing_ratio = config.judder_swing_ratio;
    std::string dump_events_path = config.dump_events_path;
    size_t dump_max_mb = config.dump_max_mb;
    size_t dump_max_files = config.dump_max_files;
    std::string export_csv_path = config.export_csv_path;
    const bool target_pid_manual = config.target_pid_manual;

    if (!output_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            std::cerr << "[STUTTOMETER] Error: Failed to create output directory '" << output_dir << "': " << ec.message() << "\n";
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
                std::cerr << "[STUTTOMETER] Error: Failed to create NDJSON output file: " << dump_events_path << "\n";
                return 1;
            }
        }
    }

    const bool is_admin = stuttometer::is_running_as_admin();
    if (!is_admin) {
        std::cerr << "\n[STUTTOMETER] Error: Running in Standard (Non-Elevated) Mode.\n";
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
            std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' matched PID " << target_pid << "\n";
        } else {
            std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' not currently running. Waiting for process to launch...\n";
        }
    }

    if (!stuttometer::enable_system_profile_privilege()) {
        std::cerr << "[STUTTOMETER] Warning: Failed to enable SeSystemprofilePrivilege. Kernel trace session may fail or be degraded.\n";
    }

    std::cout << "[STUTTOMETER] Initializing Stuttometer v0.4.2 (Elevated Mode)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    stuttometer::EtwSessionConfig etw_config;
    etw_config.enable_dxgi = true;
    etw_config.enable_audio = enable_audio;
    etw_config.enable_kernel_dpc = (provider_tier != "minimal");
    etw_config.enable_kernel_disk = (provider_tier != "minimal");
    etw_config.enable_kernel_cswitch = (provider_tier == "full");
    etw_config.enable_dxgkrnl = (provider_tier != "minimal");
    etw_config.enable_dwm_core = (provider_tier != "minimal");
    etw_config.enable_kernel_pagefault = (provider_tier != "minimal");
    etw_config.enable_processor_power = (provider_tier != "minimal");
    etw_config.enable_antimalware = (provider_tier != "minimal");
    etw_config.enable_d3d12 = (provider_tier != "minimal");
    etw_config.enable_kernel_memory = (provider_tier != "minimal");

    const uint32_t requested_slots = buffer_slots;
    buffer_slots = stuttometer::compute_recommended_buffer_slots(etw_config, requested_slots);
    if (buffer_slots > requested_slots) {
        std::cout << "[STUTTOMETER] Buffer capacity automatically bumped to " << buffer_slots << " slots for active providers\n";
    }

    stuttometer::FlightRecorder flight_recorder(buffer_slots);
    if (flight_recorder.capacity() != buffer_slots) {
        std::cout << "[STUTTOMETER] Buffer capacity rounded up to " << flight_recorder.capacity() << " slots\n";
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

    stuttometer::TriggerEngine trigger_engine(trig_config, qpc_freq);
    stuttometer::EtwSessionManager session_mgr(flight_recorder, trigger_engine, etw_config);
    if (ndjson_writer) {
        session_mgr.set_ndjson_writer(ndjson_writer.get());
    }

    const auto start_result = session_mgr.start();
    if (start_result == stuttometer::SessionStartResult::FAILED) {
        std::cerr << "[STUTTOMETER] Error: Failed to start ETW sessions.\n";
        return 1;
    } else if (start_result == stuttometer::SessionStartResult::DEGRADED_USER_ONLY) {
        std::cout << "[STUTTOMETER] Notice: Running in DEGRADED USER-ONLY mode (Kernel trace session unavailable).\n";
    } else if (start_result == stuttometer::SessionStartResult::DEGRADED_KERNEL_ONLY) {
        std::cout << "[STUTTOMETER] Notice: Running in DEGRADED KERNEL-ONLY mode (User DXGI/Audio session unavailable).\n";
    }

    std::cout << "[STUTTOMETER] Active. Monitoring frame delivery (Mode: " << stuttometer::frame_trigger_mode_to_string(frame_trig_mode)
              << ", Spike: " << spike_multiplier << "x, Static Threshold: " << present_threshold_ms << "ms)...\n";
    if (!output_file.empty() && max_reports != 1) {
        std::cout << "[STUTTOMETER] Notice: --output specified for multiple reports. The file will be overwritten with the latest report on each trigger (use --output-dir to save all reports).\n";
    }
    std::cout << "Press Ctrl+C to stop.\n\n";

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelatorThresholds thresholds;
    thresholds.dpc_threshold_us = dpc_threshold_us;
    thresholds.isr_threshold_us = isr_threshold_us;
    thresholds.disk_threshold_ms = disk_threshold_ms;
    thresholds.cswitch_preempt_ms = cswitch_preempt_ms;
    thresholds.smi_severity_threshold_ms = smi_severity_threshold_ms;
    thresholds.d3d12_pso_threshold_ms = d3d12_pso_threshold_ms;
    thresholds.vram_demoted_threshold_mb = vram_demoted_threshold_mb;
    thresholds.mem_alloc_threshold_mb = mem_alloc_threshold_mb;
    thresholds.mem_trim_threshold_mb = mem_trim_threshold_mb;
    thresholds.mem_physical_latency_us = mem_physical_latency_us;

    stuttometer::CorrelationEngine correlator(driver_resolver, thresholds);
    stuttometer::JsonReporter reporter;

    uint32_t report_count = 0;
    uint32_t loop_counter = 0;

    // Dedicated background thread for CLI process name watcher (eliminates main loop jitter)
    std::thread watcher_thread;
    if (!target_pid_manual && !target_process_name.empty()) {
        watcher_thread = std::thread([&]() {
            while (!g_stop_requested.load(std::memory_order_relaxed)) {
                const uint32_t found_pid = stuttometer::resolve_process_name_to_pid(target_process_name);
                if (found_pid != 0) {
                    if (trigger_engine.try_attach_pid(found_pid)) {
                        std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' active (PID " << found_pid << ")\n";
                    }
                } else {
                    const uint32_t active = trigger_engine.active_target_pid();
                    if (active != 0 && trigger_engine.try_detach_pid(active)) {
                        std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' closed. Waiting for restart...\n";
                    }
                }

                // Poll responsive 50ms while waiting for target to launch; poll ~2s once attached
                const int sleep_steps = trigger_engine.is_target_waiting() ? 1 : 40;
                for (int i = 0; i < sleep_steps && !g_stop_requested.load(std::memory_order_relaxed); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    }

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const uint64_t current_qpc = stuttometer::get_current_qpc();
        stuttometer::TriggerInfo trigger_info;
        uint64_t from_qpc = 0;
        uint64_t to_qpc = 0;

        ++loop_counter;

        if (verbose && loop_counter % 500 == 0) {
            std::cout << "[VERBOSE] Head: " << flight_recorder.current_head() 
                      << " | Upstream Lost Events: " << session_mgr.events_lost()
                      << " | Lost Buffers: " << session_mgr.buffers_lost()
                      << " | Unpaired Evictions: " << session_mgr.unpaired_evictions()
                      << " | Insertion Failures: " << session_mgr.insertion_failures() << "\n";
        }

        if (trigger_engine.poll_state(current_qpc, trigger_info, from_qpc, to_qpc)) {
            struct ReportScopeGuard {
                stuttometer::TriggerEngine& engine;
                ~ReportScopeGuard() {
                    engine.on_report_completed(stuttometer::get_current_qpc());
                }
            } guard{trigger_engine};

            try {
                // Synchronously flush active buffers and deterministically drain post-trigger window
                session_mgr.flush_buffers();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
                while (session_mgr.last_processed_qpc() < to_qpc && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

                uint64_t drops = 0;
                auto snapshot = flight_recorder.snapshot(from_qpc, to_qpc, &drops);

                stuttometer::ProviderContext p_ctx;
                p_ctx.kernel_dpc_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_dpc;
                p_ctx.kernel_disk_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_disk;
                p_ctx.kernel_cswitch_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_cswitch;
                p_ctx.user_dxgi_active = session_mgr.is_user_session_active() && etw_config.enable_dxgi;
                p_ctx.user_audio_active = session_mgr.is_user_session_active() && etw_config.enable_audio;
                p_ctx.user_dxgkrnl_active = session_mgr.is_user_session_active() && etw_config.enable_dxgkrnl;
                p_ctx.user_dwm_active = session_mgr.is_user_session_active() && etw_config.enable_dwm_core;
                p_ctx.kernel_pagefault_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_pagefault;
                p_ctx.user_processor_power_active = session_mgr.is_user_session_active() && etw_config.enable_processor_power;
                p_ctx.user_antimalware_active = session_mgr.is_user_session_active() && etw_config.enable_antimalware;
                p_ctx.user_d3d12_active = session_mgr.is_user_session_active() && etw_config.enable_d3d12;
                p_ctx.user_vram_paging_active = session_mgr.is_user_session_active() && etw_config.enable_dxgkrnl;
                // Note: Kernel-Memory provider runs on the user trace session, not the NT Kernel Logger
                p_ctx.kernel_memory_active = session_mgr.is_user_session_active() && etw_config.enable_kernel_memory;
                p_ctx.etw_events_lost = session_mgr.events_lost();
                p_ctx.etw_buffers_lost = session_mgr.buffers_lost();

                uint64_t unpaired_evicts = session_mgr.unpaired_evictions();
                uint64_t ins_failures = session_mgr.insertion_failures();
                stuttometer::CorrelateOptions correlate_opts{
                    .window_pre_ms = window_pre_ms,
                    .window_post_ms = window_post_ms,
                    .present_threshold_ms = present_threshold_ms,
                    .provider_tier = provider_tier,
                    .redact = redact
                };

                auto report = correlator.correlate(snapshot, trigger_info, qpc_freq, correlate_opts, p_ctx, drops, unpaired_evicts, ins_failures, flight_recorder.total_dropped_events());

                reporter.print_console_summary(report, std::cout, redact);

                if (!output_file.empty()) {
                    if (!reporter.save_to_file(report, std::filesystem::path(output_file), redact)) {
                        std::cerr << "[STUTTOMETER] Error: Failed to write report to '" << output_file << "'\n";
                    }
                }
                if (!export_csv_path.empty()) {
                    if (!stuttometer::csv::export_to_file(report, std::filesystem::path(export_csv_path))) {
                        std::cerr << "[STUTTOMETER] Error: Failed to export CSV to '" << export_csv_path << "'\n";
                    }
                }
                if (!output_dir.empty()) {
                    const std::filesystem::path dir(output_dir);
                    const std::string json_name = "stutto_report_" + std::to_string(report_count + 1) + "_" + std::to_string(current_qpc) + ".json";
                    if (reporter.save_to_file(report, dir / json_name, redact)) {
                        stuttometer::rotate_directory_by_prefix(dir, "stutto_report_", ".json", 100);
                    } else {
                        std::cerr << "[STUTTOMETER] Error: Failed to write report to '" << (dir / json_name).string() << "'\n";
                    }
                    const std::string csv_name = "stutto_pacing_" + std::to_string(report_count + 1) + "_" + std::to_string(current_qpc) + ".csv";
                    if (stuttometer::csv::export_to_file(report, dir / csv_name)) {
                        stuttometer::rotate_directory_by_prefix(dir, "stutto_pacing_", ".csv", 100);
                    }
                }

                ++report_count;

                if (max_reports > 0 && report_count >= max_reports) {
                    break;
                }
            } catch (const std::exception& ex) {
                std::cerr << "[STUTTOMETER] Exception during report processing: " << ex.what() << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (watcher_thread.joinable()) {
        watcher_thread.join();
    }

    std::cout << "\n[STUTTOMETER] Stopping trace sessions and cleaning up...\n";
    session_mgr.stop();
    if (ndjson_writer) {
        ndjson_writer->stop();
    }
    std::cout << "[STUTTOMETER] Done. Total reports generated: " << report_count << "\n";

    {
        std::lock_guard<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_done.store(true, std::memory_order_release);
    }
    g_shutdown_cv.notify_all();

    return 0;
}
