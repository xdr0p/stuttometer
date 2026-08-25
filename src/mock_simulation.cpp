#include "stuttometer/mock_simulation.hpp"
#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <sstream>

namespace stuttometer {

MockSimulationResult run_mock_simulation_pipeline(bool redact) {
    MockSimulationResult result{};

    const uint64_t qpc_freq = get_qpc_frequency();
    FlightRecorder recorder(1024);

    TriggerConfig trig_config;
    trig_config.window_pre_ms = 250.0;
    trig_config.window_post_ms = 30.0;
    trig_config.present_threshold_ms = 16.67;

    TriggerEngine trigger_engine(trig_config, qpc_freq);
    DriverSymbolResolver driver_resolver;
    CorrelationEngine correlator(driver_resolver);
    JsonReporter reporter;

    const uint64_t base_qpc = get_current_qpc();
    const uint32_t test_pid = 1234;
    const uint32_t test_tid = 5678;

    // 1. Simulated background context switches and an involuntary preemption on target thread
    for (int i = 0; i < 500; ++i) {
        EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(EventCategory::CSWITCH);
        rec.qpc_timestamp = base_qpc + ms_to_qpc_delta(i * 0.5, qpc_freq);
        rec.pid = (i == 480) ? test_pid : (2000 + (i % 10));
        rec.tid = (i == 480) ? test_tid : (3000 + (i % 10));
        rec.cpu_index = static_cast<uint8_t>(i % 8);
        rec.payload.cswitch.prev_pid = (i == 480) ? 9999 : (1000 + (i % 10));
        rec.payload.cswitch.prev_tid = (i == 480) ? 8888 : (4000 + (i % 10));
        if (i == 480) {
            rec.duration_us = 6500; // 6.5ms involuntary preemption (> 5.0ms threshold)
            rec.flags = EventFlags::NONE; // involuntary (no CSWITCH_VOLUNTARY)
        }
        recorder.push(rec);
    }

    // 2. Simulated 3.8ms GPU Driver DPC latency spike on Core 2
    {
        EtwEventRecord dpc{};
        dpc.category = static_cast<uint16_t>(EventCategory::DPC);
        dpc.qpc_timestamp = base_qpc + ms_to_qpc_delta(240.0, qpc_freq);
        dpc.pid = 0;
        dpc.tid = 0;
        dpc.cpu_index = 2;
        dpc.duration_us = 3800; // 3.8ms DPC
        dpc.payload.routine_addr = 0xFFFFF80100000000ULL;
        recorder.push(dpc);
    }

    // 3. Simulated VRAM demoted commitment (45 MB demoted to system RAM)
    {
        EtwEventRecord vram{};
        vram.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VRAM_PAGING);
        vram.qpc_timestamp = base_qpc + ms_to_qpc_delta(235.0, qpc_freq);
        vram.pid = test_pid;
        vram.tid = test_tid;
        vram.auxiliary_data = 45 * 1024 * 1024ULL; // 45 MB demoted
        vram.flags = EventFlags::VRAM_DEMOTED_COMMITMENT;
        recorder.push(vram);
    }

    // 4. Simulated Large VirtualAlloc Commit Stall (64 MB synchronous commit)
    {
        EtwEventRecord valloc{};
        valloc.category = static_cast<uint16_t>(EventCategory::MEM_VIRTUAL_ALLOC);
        valloc.qpc_timestamp = base_qpc + ms_to_qpc_delta(238.0, qpc_freq);
        valloc.pid = test_pid;
        valloc.tid = test_tid;
        valloc.payload.routine_addr = 0x00007FF710000000ULL;
        valloc.auxiliary_data = 64 * 1024 * 1024ULL; // 64 MB
        valloc.flags = EventFlags::MEM_ALLOC_COMMIT;
        recorder.push(valloc);
    }

    // 5. Simulated OS Working Set Trim Out-Swap (16 MB trimmed, 12ms duration)
    {
        EtwEventRecord wstrim{};
        wstrim.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);
        wstrim.qpc_timestamp = base_qpc + ms_to_qpc_delta(242.0, qpc_freq);
        wstrim.pid = test_pid;
        wstrim.duration_us = 12000; // 12ms trim
        wstrim.auxiliary_data = 16 * 1024 * 1024ULL; // 16 MB
        wstrim.flags = EventFlags::MEM_WS_TRIM_OUTSWAP;
        recorder.push(wstrim);
    }

    // 6. Establish rolling baseline with 10 normal 16.67ms frames (60 FPS)
    for (int i = 0; i < 10; ++i) {
        const uint64_t frame_qpc = base_qpc + ms_to_qpc_delta(i * 16.67, qpc_freq);
        trigger_engine.on_dxgi_present(test_pid, test_tid, 16.67, frame_qpc, 0x1234, 0);
    }

    // 7. Simulated DXGI Present latency stutter (42.5ms frame drop = 2.55x relative spike)
    const uint64_t trigger_qpc = base_qpc + ms_to_qpc_delta(245.0, qpc_freq);
    trigger_engine.on_dxgi_present(test_pid, test_tid, 42.5, trigger_qpc, 0x1234, 2);

    TriggerInfo trigger_info;
    uint64_t from_qpc = 0;
    uint64_t to_qpc = 0;

    const uint64_t poll_qpc = trigger_qpc + ms_to_qpc_delta(35.0, qpc_freq);
    if (trigger_engine.poll_state(poll_qpc, trigger_info, from_qpc, to_qpc)) {
        uint64_t drops = 0;
        auto snapshot = recorder.snapshot(from_qpc, to_qpc, &drops);

        ProviderContext p_ctx;
        p_ctx.kernel_dpc_active = true;
        p_ctx.kernel_cswitch_active = true;
        p_ctx.kernel_pagefault_active = true;
        p_ctx.user_dxgkrnl_active = true;
        p_ctx.user_vram_paging_active = true;
        p_ctx.kernel_memory_active = true;

        result.report = correlator.correlate(snapshot, trigger_info, qpc_freq, p_ctx, drops, 0, 0, recorder.total_dropped_events());
        result.report.target_process = "SimulatedGame.exe";
        result.report.window_pre_ms = trig_config.window_pre_ms;
        result.report.window_post_ms = trig_config.window_post_ms;
        result.report.present_threshold_ms = trig_config.present_threshold_ms;
        result.report.provider_tier = "full (simulated)";
        result.report.redacted = redact;

        std::ostringstream oss;
        reporter.print_console_summary(result.report, oss, redact);
        result.summary_text = oss.str();

        result.json_text = reporter.to_json_string(result.report, redact, 2);
    }

    return result;
}

} // namespace stuttometer
