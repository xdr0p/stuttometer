#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <set>
#include <mutex>
#include <condition_variable>

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
    std::string trigger_mode_str = "hybrid";
    double spike_multiplier = 2.0;
    double min_spike_delta_ms = 4.0;
    bool enable_judder = true;
    double judder_swing_ratio = 0.35;

    app.add_option("--window-ms", window_pre_ms, "Pre-trigger window duration in ms (50-1000, default: 250)");
    app.add_option("--post-trigger-ms", window_post_ms, "Post-trigger capture duration in ms (0-200, default: 30)");
    app.add_option("--present-threshold-ms", present_threshold_ms, "DXGI Present stutter threshold in ms (2.0-200.0, default: 16.67)");
    app.add_option("--trigger-mode", trigger_mode_str, "Frame trigger mode: hybrid, dynamic, static (default: hybrid)");
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
    app.add_flag("--redact", redact, "Redact process names, file paths, and user identifiers");
    app.add_flag("--verbose", verbose, "Print detailed event stream metrics to console");
    app.add_flag("--version", print_version, "Print version information and exit");

    CLI11_PARSE(app, argc, argv);

    if (print_version) {
        std::cout << "Stuttometer v0.1.0\n";
        return 0;
    }

    // CLI Range and Option Validation
    if (window_pre_ms < 50.0 || window_pre_ms > 1000.0) {
        std::cerr << "[STUTTOMETER] Error: --window-ms must be between 50.0 and 1000.0 ms.\n";
        return 1;
    }
    if (window_post_ms < 0.0 || window_post_ms > 200.0) {
        std::cerr << "[STUTTOMETER] Error: --post-trigger-ms must be between 0.0 and 200.0 ms.\n";
        return 1;
    }
    if (present_threshold_ms < 2.0 || present_threshold_ms > 200.0) {
        std::cerr << "[STUTTOMETER] Error: --present-threshold-ms must be between 2.0 and 200.0 ms.\n";
        return 1;
    }
    if (cooldown_ms < 100.0 || cooldown_ms > 10000.0) {
        std::cerr << "[STUTTOMETER] Error: --cooldown-ms must be between 100.0 and 10000.0 ms.\n";
        return 1;
    }
    if (dpc_threshold_us < 100 || dpc_threshold_us > 50000) {
        std::cerr << "[STUTTOMETER] Error: --dpc-threshold-us must be between 100 and 50000 us.\n";
        return 1;
    }
    if (isr_threshold_us < 50 || isr_threshold_us > 50000) {
        std::cerr << "[STUTTOMETER] Error: --isr-threshold-us must be between 50 and 50000 us.\n";
        return 1;
    }
    if (disk_threshold_ms < 1 || disk_threshold_ms > 1000) {
        std::cerr << "[STUTTOMETER] Error: --disk-threshold-ms must be between 1 and 1000 ms.\n";
        return 1;
    }
    if (cswitch_preempt_ms < 1 || cswitch_preempt_ms > 500) {
        std::cerr << "[STUTTOMETER] Error: --cswitch-threshold-ms must be between 1 and 500 ms.\n";
        return 1;
    }
    if (smi_severity_threshold_ms < 10.0 || smi_severity_threshold_ms > 100.0) {
        std::cerr << "[STUTTOMETER] Error: --smi-threshold-ms must be between 10.0 and 100.0 ms.\n";
        return 1;
    }
    if (d3d12_pso_threshold_ms < 1 || d3d12_pso_threshold_ms > 500) {
        std::cerr << "[STUTTOMETER] Error: --d3d12-pso-threshold-ms must be between 1 and 500 ms.\n";
        return 1;
    }
    if (vram_demoted_threshold_mb < 1 || vram_demoted_threshold_mb > 1024) {
        std::cerr << "[STUTTOMETER] Error: --vram-threshold-mb must be between 1 and 1024 MB.\n";
        return 1;
    }
    if (mem_alloc_threshold_mb < 1 || mem_alloc_threshold_mb > 1024) {
        std::cerr << "[STUTTOMETER] Error: --mem-alloc-threshold-mb must be between 1 and 1024 MB.\n";
        return 1;
    }
    if (mem_trim_threshold_mb < 1 || mem_trim_threshold_mb > 1024) {
        std::cerr << "[STUTTOMETER] Error: --mem-trim-threshold-mb must be between 1 and 1024 MB.\n";
        return 1;
    }
    if (mem_physical_latency_us < 50 || mem_physical_latency_us > 50000) {
        std::cerr << "[STUTTOMETER] Error: --mem-physical-latency-us must be between 50 and 50000 us.\n";
        return 1;
    }
    if (buffer_slots < 65536 || buffer_slots > 1048576) {
        std::cerr << "[STUTTOMETER] Error: --buffer-slots must be between 65536 and 1048576.\n";
        return 1;
    }

    if (spike_multiplier < 1.2 || spike_multiplier > 10.0) {
        std::cerr << "[STUTTOMETER] Error: --spike-multiplier must be between 1.2 and 10.0.\n";
        return 1;
    }
    if (min_spike_delta_ms < 1.0 || min_spike_delta_ms > 50.0) {
        std::cerr << "[STUTTOMETER] Error: --min-spike-delta-ms must be between 1.0 and 50.0 ms.\n";
        return 1;
    }
    if (judder_swing_ratio < 0.1 || judder_swing_ratio > 0.9) {
        std::cerr << "[STUTTOMETER] Error: --judder-swing-ratio must be between 0.1 and 0.9.\n";
        return 1;
    }

    const std::set<std::string> valid_trigger_modes = { "hybrid", "dynamic", "static" };
    if (valid_trigger_modes.find(trigger_mode_str) == valid_trigger_modes.end()) {
        std::cerr << "[STUTTOMETER] Error: Invalid --trigger-mode '" << trigger_mode_str << "'. Must be 'hybrid', 'dynamic', or 'static'.\n";
        return 1;
    }

    const std::set<std::string> valid_tiers = { "minimal", "standard", "full" };
    if (valid_tiers.find(provider_tier) == valid_tiers.end()) {
        std::cerr << "[STUTTOMETER] Error: Invalid --tier '" << provider_tier << "'. Must be 'minimal', 'standard', or 'full'.\n";
        return 1;
    }
    if (target_process_name.size() > 260) {
        std::cerr << "[STUTTOMETER] Error: --target-process name exceeds maximum length (260 characters).\n";
        return 1;
    }

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

    const bool is_admin = stuttometer::is_running_as_admin();
    if (!is_admin) {
        std::cerr << "\n[STUTTOMETER] Error: Running in Standard (Non-Elevated) Mode.\n";
        std::cerr << "Kernel ETW providers (DPC, ISR, Disk I/O, Context Switches) require Administrator privileges.\n";
        std::cerr << "To test the diagnostic engine without elevation, run:\n";
        std::cerr << "  .\\stuttometer.exe --mock-test\n\n";
        std::cerr << "To run live capture, please launch PowerShell as Administrator and run:\n";
        std::cerr << "  .\\stuttometer.exe [OPTIONS]\n\n";
        return 1;
    }

    const bool target_pid_manual = (app.count("--target-pid") > 0 && target_pid != 0);
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

    std::cout << "[STUTTOMETER] Initializing Stuttometer v0.1.0 (Elevated Mode)...\n";
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

    if (etw_config.enable_dxgkrnl || etw_config.enable_dwm_core) {
        buffer_slots = std::max(buffer_slots, 262144U);
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
    trig_config.spike_multiplier = spike_multiplier;
    trig_config.min_spike_delta_ms = min_spike_delta_ms;
    trig_config.enable_judder_detection = enable_judder;
    trig_config.judder_swing_ratio = judder_swing_ratio;

    stuttometer::TriggerEngine trigger_engine(trig_config, qpc_freq);
    stuttometer::EtwSessionManager session_mgr(flight_recorder, trigger_engine, etw_config);

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
                p_ctx.kernel_memory_active = session_mgr.is_user_session_active() && etw_config.enable_kernel_memory;
                p_ctx.etw_events_lost = session_mgr.events_lost();
                p_ctx.etw_buffers_lost = session_mgr.buffers_lost();

                uint64_t unpaired_evicts = session_mgr.unpaired_evictions();
                uint64_t ins_failures = session_mgr.insertion_failures();
                auto report = correlator.correlate(snapshot, trigger_info, qpc_freq, p_ctx, drops, unpaired_evicts, ins_failures, flight_recorder.total_dropped_events());
                report.target_process = stuttometer::get_process_name_by_pid(trigger_info.target_pid);
                report.window_pre_ms = window_pre_ms;
                report.window_post_ms = window_post_ms;
                report.present_threshold_ms = present_threshold_ms;
                report.provider_tier = provider_tier;
                report.redacted = redact;

                reporter.print_console_summary(report, std::cout, redact);

                if (!output_file.empty()) {
                    if (!reporter.save_to_file(report, std::filesystem::path(output_file), redact)) {
                        std::cerr << "[STUTTOMETER] Error: Failed to write report to '" << output_file << "'\n";
                    }
                }
                if (!output_dir.empty()) {
                    const std::filesystem::path dir(output_dir);
                    const std::filesystem::path path = dir / ("stutto_report_" + std::to_string(report_count + 1) + "_" + std::to_string(current_qpc) + ".json");
                    if (!reporter.save_to_file(report, path, redact)) {
                        std::cerr << "[STUTTOMETER] Error: Failed to write report to '" << path.string() << "'\n";
                    } else {
                        constexpr size_t MAX_SAVED_REPORTS = 100;
                        std::error_code dir_ec;
                        std::vector<std::filesystem::directory_entry> entries;
                        for (const auto& entry : std::filesystem::directory_iterator(dir, dir_ec)) {
                            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                                entries.push_back(entry);
                            }
                        }
                        if (entries.size() > MAX_SAVED_REPORTS) {
                            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                                return a.last_write_time() < b.last_write_time();
                            });
                            for (size_t i = 0; i < entries.size() - MAX_SAVED_REPORTS; ++i) {
                                std::filesystem::remove(entries[i].path(), dir_ec);
                            }
                        }
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
    std::cout << "[STUTTOMETER] Done. Total reports generated: " << report_count << "\n";

    {
        std::lock_guard<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_done.store(true, std::memory_order_release);
    }
    g_shutdown_cv.notify_all();

    return 0;
}
