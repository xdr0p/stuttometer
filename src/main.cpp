#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_stop_requested{false};

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

static void run_mock_simulation() {
    std::cout << "\n================================================================================\n";
    std::cout << " [STUTTOMETER] Running Synthetic Stutter Simulation Test (Non-Elevated Mode)\n";
    std::cout << "================================================================================\n\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::FlightRecorder recorder(262144);
    
    stuttometer::TriggerConfig trig_config;
    trig_config.window_pre_ms = 250.0;
    trig_config.window_post_ms = 30.0;
    trig_config.present_threshold_ms = 25.0;

    stuttometer::TriggerEngine trigger_engine(trig_config, qpc_freq);
    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);
    stuttometer::JsonReporter reporter;

    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t test_pid = 1234;
    const uint32_t test_tid = 5678;

    std::cout << "[SIM] Generating simulated background context switches...\n";
    for (int i = 0; i < 500; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
        rec.qpc_timestamp = base_qpc + stuttometer::ms_to_qpc_delta(i * 0.5, qpc_freq);
        rec.pid = test_pid;
        rec.tid = test_tid;
        rec.cpu_index = static_cast<uint8_t>(i % 8);
        recorder.push(rec);
    }

    std::cout << "[SIM] Injecting simulated 3.8ms NVIDIA GPU Driver DPC latency spike on Core 2...\n";
    {
        stuttometer::EtwEventRecord dpc{};
        dpc.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
        dpc.qpc_timestamp = base_qpc + stuttometer::ms_to_qpc_delta(240.0, qpc_freq);
        dpc.pid = 0;
        dpc.tid = 0;
        dpc.cpu_index = 2;
        dpc.duration_us = 3800; // 3.8ms DPC
        dpc.payload.routine_addr = 0xFFFFF80100000000ULL; // Synthetic address
        recorder.push(dpc);
    }

    std::cout << "[SIM] Triggering DXGI Present Latency Stutter (42.5ms frame drop)...\n";
    const uint64_t trigger_qpc = base_qpc + stuttometer::ms_to_qpc_delta(245.0, qpc_freq);
    trigger_engine.on_dxgi_present(test_pid, test_tid, 42.5, trigger_qpc);

    stuttometer::TriggerInfo trigger_info;
    uint64_t from_qpc = 0;
    uint64_t to_qpc = 0;

    const uint64_t poll_qpc = trigger_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    if (trigger_engine.poll_state(poll_qpc, trigger_info, from_qpc, to_qpc)) {
        std::cout << "[SIM] Trigger state FROZEN. Extracting snapshot...\n";
        auto snapshot = recorder.snapshot(from_qpc, to_qpc);
        std::cout << "[SIM] Extracted " << snapshot.size() << " events in window. Correlating...\n";

        auto report = correlator.correlate(snapshot, trigger_info, qpc_freq);
        reporter.print_console_summary(report, std::cout, false);

        std::cout << "[SIM] Generating sample JSON report:\n";
        std::cout << reporter.to_json_string(report, false, 2) << "\n";
    }
}

int main(int argc, char** argv) {
    CLI::App app{"Stuttometer - Real-Time Windows ETW Stutter & Glitch Diagnostic Utility"};

    double window_pre_ms = 250.0;
    double window_post_ms = 30.0;
    double present_threshold_ms = 25.0;
    bool enable_audio = true;
    double cooldown_ms = 1000.0;
    uint32_t dpc_threshold_us = 1000;
    uint32_t disk_threshold_ms = 20;
    uint32_t target_pid = 0;
    std::string target_process_name;
    std::string output_file;
    std::string output_dir;
    uint32_t max_reports = 0;
    std::string provider_tier = "full";
    bool redact = false;
    bool verbose = false;
    bool mock_test = false;
    bool print_version = false;

    app.add_option("--window-ms", window_pre_ms, "Pre-trigger window duration in ms (default: 250)");
    app.add_option("--post-trigger-ms", window_post_ms, "Post-trigger capture duration in ms (default: 30)");
    app.add_option("--present-threshold-ms", present_threshold_ms, "DXGI Present stutter threshold in ms (default: 25.0)");
    app.add_flag("--audio-trigger,!--no-audio", enable_audio, "Enable/disable AudioGlitch Event ID 11 trigger");
    app.add_option("--cooldown-ms", cooldown_ms, "Minimum cooldown between reports in ms (default: 1000)");
    app.add_option("--dpc-threshold-us", dpc_threshold_us, "DPC anomaly threshold in microseconds (default: 1000)");
    app.add_option("--disk-threshold-ms", disk_threshold_ms, "Disk latency anomaly threshold in ms (default: 20)");
    app.add_option("--target-pid", target_pid, "Target Process ID to monitor (default: auto-detect)");
    app.add_option("--target-process", target_process_name, "Target process name substring (e.g. Game.exe)");
    app.add_option("--output", output_file, "Output file path for JSON reports");
    app.add_option("--output-dir", output_dir, "Directory to save individual trigger reports");
    app.add_option("--max-reports", max_reports, "Maximum number of reports before exiting (0 = continuous)");
    app.add_option("--tier", provider_tier, "Provider tier: minimal, standard, full (default: full)");
    app.add_flag("--redact", redact, "Redact process names, file paths, and user identifiers");
    app.add_flag("--verbose", verbose, "Print detailed event stream metrics to console");
    app.add_flag("--mock-test", mock_test, "Run internal synthetic trace simulation suite");
    app.add_flag("--version", print_version, "Print version information and exit");

    CLI11_PARSE(app, argc, argv);

    if (print_version) {
        std::cout << "Stuttometer v0.1.0\n";
        return 0;
    }

    if (mock_test) {
        run_mock_simulation();
        return 0;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    const bool is_admin = stuttometer::is_running_as_admin();
    if (!is_admin) {
        std::cout << "\n[STUTTOMETER] Notice: Running in Standard (Non-Elevated) Mode.\n";
        std::cout << "Kernel ETW providers (DPC, ISR, Disk I/O, Context Switches) require Administrator privileges.\n";
        std::cout << "To test the diagnostic engine right now, run:\n";
        std::cout << "  .\\stuttometer.exe --mock-test\n\n";
        std::cout << "To run full real-time capture against games, please launch PowerShell as Administrator and run:\n";
        std::cout << "  .\\stuttometer.exe [OPTIONS]\n\n";
        return 0;
    }

    stuttometer::enable_system_profile_privilege();

    // Resolve target process name to PID at startup if provided
    if (target_pid == 0 && !target_process_name.empty()) {
        target_pid = stuttometer::resolve_process_name_to_pid(target_process_name);
        if (target_pid != 0) {
            std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' matched PID " << target_pid << "\n";
        } else {
            std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' not currently running. Will monitor all processes.\n";
        }
    }

    std::cout << "[STUTTOMETER] Initializing Stuttometer v0.1.0 (Elevated Mode)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    stuttometer::FlightRecorder flight_recorder(262144);

    stuttometer::TriggerConfig trig_config;
    trig_config.window_pre_ms = window_pre_ms;
    trig_config.window_post_ms = window_post_ms;
    trig_config.present_threshold_ms = present_threshold_ms;
    trig_config.audio_trigger_enabled = enable_audio;
    trig_config.cooldown_ms = cooldown_ms;
    trig_config.target_pid = target_pid;
    trig_config.target_process_name = target_process_name;

    stuttometer::TriggerEngine trigger_engine(trig_config, qpc_freq);

    stuttometer::EtwSessionConfig etw_config;
    etw_config.enable_dxgi = true;
    etw_config.enable_audio = enable_audio;
    etw_config.enable_kernel_dpc = (provider_tier != "minimal");
    etw_config.enable_kernel_disk = (provider_tier == "full");
    etw_config.enable_kernel_cswitch = (provider_tier == "full");

    stuttometer::EtwSessionManager session_mgr(flight_recorder, trigger_engine, etw_config);

    if (!session_mgr.start()) {
        std::cerr << "[STUTTOMETER] Error: Failed to start ETW sessions.\n";
        return 1;
    }

    std::cout << "[STUTTOMETER] Active. Monitoring DXGI Present latency (> " << present_threshold_ms << "ms)...\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelatorThresholds thresholds;
    thresholds.dpc_threshold_us = dpc_threshold_us;
    thresholds.disk_threshold_ms = disk_threshold_ms;

    stuttometer::CorrelationEngine correlator(driver_resolver, thresholds);
    stuttometer::JsonReporter reporter;

    uint32_t report_count = 0;
    uint32_t loop_counter = 0;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        const uint64_t current_qpc = stuttometer::get_current_qpc();
        stuttometer::TriggerInfo trigger_info;
        uint64_t from_qpc = 0;
        uint64_t to_qpc = 0;

        // Periodic background process PID refresh (every ~100 iterations = ~1s)
        if (++loop_counter % 100 == 0 && !target_process_name.empty() && target_pid == 0) {
            uint32_t found_pid = stuttometer::resolve_process_name_to_pid(target_process_name);
            if (found_pid != 0) {
                target_pid = found_pid;
                trigger_engine.update_target_pid(target_pid);
                std::cout << "[STUTTOMETER] Target process '" << target_process_name << "' detected (PID " << target_pid << ")\n";
            }
        }

        if (trigger_engine.poll_state(current_qpc, trigger_info, from_qpc, to_qpc)) {
            uint64_t drops = 0;
            auto snapshot = flight_recorder.snapshot(from_qpc, to_qpc, &drops);
            auto report = correlator.correlate(snapshot, trigger_info, qpc_freq, drops);
            report.window_pre_ms = window_pre_ms;
            report.window_post_ms = window_post_ms;
            report.present_threshold_ms = present_threshold_ms;
            report.provider_tier = provider_tier;
            report.redacted = redact;

            reporter.print_console_summary(report, std::cout, redact);

            if (!output_file.empty()) {
                reporter.save_to_file(report, output_file, redact);
            }
            if (!output_dir.empty()) {
                const std::string path = output_dir + "/stutto_" + std::to_string(current_qpc) + ".json";
                reporter.save_to_file(report, path, redact);
            }

            trigger_engine.on_report_completed(stuttometer::get_current_qpc());
            ++report_count;

            if (max_reports > 0 && report_count >= max_reports) {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\n[STUTTOMETER] Stopping trace sessions and cleaning up...\n";
    session_mgr.stop();
    std::cout << "[STUTTOMETER] Done. Total reports generated: " << report_count << "\n";

    return 0;
}
