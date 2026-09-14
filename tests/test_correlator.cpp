#include "test_common.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>

static void test_dpc_spike_correlation() {
    std::cout << "[TEST] Validating DPC Latency Spike Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord dpc{};
    dpc.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
    dpc.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    dpc.duration_us = 3500; // 3.5ms
    dpc.cpu_index = 3;
    dpc.payload.routine_addr = 0xFFFFF80012340000ULL;
    snapshot.push_back(dpc);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "dpc_isr_spike");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.80);
    STUTTO_ASSERT(report.diagnoses[0].rank == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_disk_stall_correlation() {
    std::cout << "[TEST] Validating Disk I/O Stall Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 55.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord disk{};
    disk.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
    disk.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    disk.pid = 4000;
    disk.duration_us = 48000; // 48ms
    disk.auxiliary_data = 1048576; // 1 MB
    snapshot.push_back(disk);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_disk_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "disk_io_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.70);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_cswitch_preemption_correlation() {
    std::cout << "[TEST] Validating Context Switch Involuntary Preemption Hypothesis (Both-Way)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Switch-in resumption for thread 8000 after 12ms preemption
    stuttometer::EtwEventRecord cs{};
    cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    cs.pid = 4000;
    cs.tid = 8000;
    cs.duration_us = 12000;
    cs.flags = 0; // Involuntary
    cs.payload.cswitch.prev_tid = 9999;
    snapshot.push_back(cs);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "context_switch_interference");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.65);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_smi_hardware_gap_and_provider_awareness() {
    std::cout << "[TEST] Validating Provider-Aware Hardware / SMI Gap Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // 1. When kernel providers ARE active and no events found -> SMI gap fires with capped confidence
    stuttometer::ProviderContext p_active;
    p_active.kernel_dpc_active = true;
    p_active.kernel_cswitch_active = true;

    auto report_active = correlator.correlate(snapshot, trigger, qpc_freq, p_active);
    STUTTO_ASSERT(!report_active.diagnoses.empty());
    STUTTO_ASSERT(report_active.diagnoses[0].hypothesis == "unprofiled_hardware_or_smi_stall");
    STUTTO_ASSERT(report_active.diagnoses[0].confidence <= 0.35);

    // 2. When kernel providers are DISABLED (e.g. minimal tier) -> SMI gap does NOT fire
    stuttometer::ProviderContext p_disabled;
    p_disabled.kernel_dpc_active = false;
    p_disabled.kernel_cswitch_active = false;

    auto report_disabled = correlator.correlate(snapshot, trigger, qpc_freq, p_disabled);
    STUTTO_ASSERT(!report_disabled.diagnoses.empty());
    STUTTO_ASSERT(report_disabled.diagnoses[0].hypothesis == "insufficient_evidence");

    std::cout << "  -> Active kernel providers correctly identified SMI gap (capped <= 35%).\n";
    std::cout << "  -> Disabled kernel providers prevented false SMI conclusion. Provider awareness PASSED.\n";
}

static void test_cswitch_switch_out_duration_non_pollution() {
    std::cout << "[TEST] Validating Switch-Out CSwitch duration does not pollute preemption ranking...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // 1. Genuine switch-in resumption for target thread 8000 with 6.0ms preemption
    stuttometer::EtwEventRecord resume_cs{};
    resume_cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    resume_cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    resume_cs.pid = 4000;
    resume_cs.tid = 8000;
    resume_cs.duration_us = 6000; // 6.0ms
    resume_cs.flags = 0; // Involuntary
    resume_cs.payload.cswitch.prev_tid = 9999;
    snapshot.push_back(resume_cs);

    // 2. Switch-out event for target thread 8000, where the incoming background thread (1111) was sleeping for 80ms
    stuttometer::EtwEventRecord switch_out_cs{};
    switch_out_cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    switch_out_cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(2.0, qpc_freq);
    switch_out_cs.pid = 4000;
    switch_out_cs.tid = 1111; // incoming thread
    switch_out_cs.duration_us = 80000; // 80ms sleep of incoming thread
    switch_out_cs.flags = 0;
    switch_out_cs.payload.cswitch.prev_tid = 8000; // target thread switching OUT
    snapshot.push_back(switch_out_cs);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "context_switch_interference");
    // The summary must reflect the genuine 6.0ms preemption, not the 80ms incoming thread sleep
    STUTTO_ASSERT(report.diagnoses[0].summary.find("6.0ms") != std::string::npos);
    STUTTO_ASSERT(report.diagnoses[0].summary.find("80.0ms") == std::string::npos);
    std::cout << "  -> Switch-out record did not pollute candidate duration ranking PASSED.\n";
}

static void test_audio_glitch_smi_gap_correlation() {
    std::cout << "[TEST] Validating Audio Glitch Trigger SMI / Hardware Gap Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::AUDIO_GLITCH;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 0.0; // Audio triggers carry duration_ms = 0
    trigger.glitch_count = 2;
    trigger.target_pid = 4000;
    trigger.target_tid = 0;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::ProviderContext p_active;
    p_active.kernel_dpc_active = true;
    p_active.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_active);
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "unprofiled_hardware_or_smi_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence <= 0.35);
    STUTTO_ASSERT(report.diagnoses[0].summary.find("Audio buffer underrun") != std::string::npos);
    std::cout << "  -> Audio glitch trigger successfully diagnosed SMI / Hardware Gap PASSED.\n";
}

static void test_auto_detect_disk_stall_correlation() {
    std::cout << "[TEST] Validating Disk I/O Stall under target_pid == 0 (Auto-Detect mode)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 55.0;
    trigger.target_pid = 0; // Auto-detect / monitor all processes
    trigger.target_tid = 0;
    trigger.cpu_index = 2;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord disk{};
    disk.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
    disk.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    disk.pid = 5432;
    disk.cpu_index = 2;
    disk.duration_us = 48000; // 48ms
    disk.auxiliary_data = 1048576; // 1 MB
    snapshot.push_back(disk);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_disk_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "disk_io_stall");
    // Target PID == 0 must receive full confidence without penalty
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.75);
    STUTTO_ASSERT(report.diagnoses[0].factors.duration_severity > 0.70);
    std::cout << "  -> Auto-detect target_pid == 0 disk stall correlation PASSED ("
              << (report.diagnoses[0].confidence * 100.0) << "% confidence).\n";
}

static void test_smi_gap_with_benign_dpcs() {
    std::cout << "[TEST] Validating SMI / Hardware Gap when baseline (benign) DPCs are present...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Add 10 benign DPCs (e.g. 50us each, far below the 1000us threshold)
    for (int i = 0; i < 10; ++i) {
        stuttometer::EtwEventRecord benign_dpc{};
        benign_dpc.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
        benign_dpc.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(20.0 + i, qpc_freq);
        benign_dpc.duration_us = 50;
        benign_dpc.cpu_index = static_cast<uint8_t>(i % 4);
        benign_dpc.payload.routine_addr = 0xFFFFF80012340000ULL;
        snapshot.push_back(benign_dpc);
    }

    stuttometer::ProviderContext p_active;
    p_active.kernel_dpc_active = true;
    p_active.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_active);
    STUTTO_ASSERT(!report.diagnoses.empty());
    // Benign DPCs must not suppress SMI diagnosis
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "unprofiled_hardware_or_smi_stall");
    std::cout << "  -> Benign baseline DPCs correctly ignored by SMI gap diagnosis PASSED.\n";
}

static void test_cswitch_autodetect_mode_correlation() {
    std::cout << "[TEST] Validating CSwitch Preemption Hypothesis in Auto-Detect Mode (target_tid == 0)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 0; // Auto-detect mode
    trigger.target_tid = 0;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Resumption event for thread 5555 after 15ms involuntary deschedule
    stuttometer::EtwEventRecord cs{};
    cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(3.0, qpc_freq);
    cs.pid = 0;
    cs.tid = 5555;
    cs.duration_us = 15000;
    cs.flags = 0; // Involuntary
    cs.payload.cswitch.prev_tid = 6666;
    snapshot.push_back(cs);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "context_switch_interference");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.65);
    STUTTO_ASSERT(report.diagnoses[0].summary.find("15.0ms") != std::string::npos);
    STUTTO_ASSERT(report.diagnoses[0].summary.find("5555") != std::string::npos);
    std::cout << "  -> Auto-detect CSwitch preemption correlation PASSED ("
              << (report.diagnoses[0].confidence * 100.0) << "% confidence).\n";
}

static void test_cswitch_voluntary_wait_ignored() {
    std::cout << "[TEST] Validating Voluntary Context Switch Waits (CSWITCH_VOLUNTARY) are ignored by Preemption Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Resumption event for thread 8000 after 15ms VOLUNTARY sleep
    stuttometer::EtwEventRecord cs{};
    cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(3.0, qpc_freq);
    cs.pid = 4000;
    cs.tid = 8000;
    cs.duration_us = 15000;
    cs.flags = stuttometer::EventFlags::CSWITCH_VOLUNTARY; // Voluntary wait/sleep
    cs.payload.cswitch.prev_tid = 6666;
    snapshot.push_back(cs);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;
    p_ctx.kernel_dpc_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    // Context switch interference should NOT be diagnosed because sleep was voluntary
    for (const auto& diag : report.diagnoses) {
        STUTTO_ASSERT(diag.hypothesis != "context_switch_interference");
    }
    std::cout << "  -> Voluntary CSwitch wait successfully filtered from preemption diagnosis. PASSED.\n";
}

static void test_dwm_compositor_stall_correlation() {
    std::cout << "[TEST] Validating DWM Compositor Stall Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    // 1. DWM Glitch Trigger (33.3ms stall)
    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DWM_GLITCH;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 33.3;
    trigger.glitch_count = 2; // 2 missed vblanks
    trigger.target_pid = 4000;
    trigger.target_tid = 0;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord dwm_rec{};
    dwm_rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DWM_GLITCH);
    dwm_rec.qpc_timestamp = trigger.trigger_timestamp_qpc;
    dwm_rec.duration_us = 33300;
    dwm_rec.auxiliary_data = 2; // 2 missed vblanks
    snapshot.push_back(dwm_rec);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_dwm_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "dwm_compositor_stall");
    // Expected confidence: 0.50 + 0.25 * min(1.0, 33.3 / 50.0) ≈ 0.6665
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.60 && report.diagnoses[0].confidence <= 0.70);
    STUTTO_ASSERT(report.diagnoses[0].summary.find("missed 2 vertical sync interval") != std::string::npos);

    // 2. 50.0ms stall test
    trigger.duration_ms = 50.0;
    trigger.glitch_count = 3;
    auto report50 = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(!report50.diagnoses.empty());
    STUTTO_ASSERT(report50.diagnoses[0].hypothesis == "dwm_compositor_stall");
    // Expected confidence: 0.50 + 0.25 * (50.0 / 50.0) = 0.75
    STUTTO_ASSERT(report50.diagnoses[0].confidence >= 0.74 && report50.diagnoses[0].confidence <= 0.76);

    std::cout << "  -> DWM compositor stall hypothesis & confidence formula PASSED.\n";
}

static void test_dwm_secondary_diagnosis_and_smi_suppression() {
    std::cout << "[TEST] Validating DWM glitch secondary diagnosis on DXGI stutter and SMI suppression...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    // DXGI Present Stutter trigger (45.0ms stall)
    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 5000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Add DWM glitch in the snapshot
    stuttometer::EtwEventRecord dwm_rec{};
    dwm_rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DWM_GLITCH);
    dwm_rec.qpc_timestamp = trigger.trigger_timestamp_qpc;
    dwm_rec.duration_us = 33300;
    dwm_rec.auxiliary_data = 2; // Glitch type
    snapshot.push_back(dwm_rec);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = true;
    p_ctx.user_dwm_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(!report.diagnoses.empty());
    // DWM glitch must produce dwm_compositor_stall diagnosis
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "dwm_compositor_stall");
    // DWM glitch presence MUST suppress unprofiled_hardware_or_smi_stall
    for (const auto& diag : report.diagnoses) {
        STUTTO_ASSERT(diag.hypothesis != "unprofiled_hardware_or_smi_stall");
    }

    std::cout << "  -> DWM secondary diagnosis and SMI suppression PASSED.\n";
}

static void test_smi_gap_suppression_on_target_preemption() {
    std::cout << "[TEST] Validating SMI / Hardware Gap suppression when target thread preemption is present...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Target thread 8000 experiences 12ms involuntary preemption (> 5ms threshold)
    stuttometer::EtwEventRecord target_cs{};
    target_cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    target_cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    target_cs.pid = 4000;
    target_cs.tid = 8000;
    target_cs.cpu_index = 1;
    target_cs.duration_us = 12000;
    target_cs.flags = 0; // Involuntary
    target_cs.payload.cswitch.prev_tid = 1111;
    snapshot.push_back(target_cs);

    stuttometer::ProviderContext p_active;
    p_active.kernel_dpc_active = true;
    p_active.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_active);
    // SMI gap must NOT be diagnosed because target thread was preempted by scheduler
    for (const auto& diag : report.diagnoses) {
        STUTTO_ASSERT(diag.hypothesis != "unprofiled_hardware_or_smi_stall");
    }
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "context_switch_interference");
    std::cout << "  -> Target preemption correctly prioritized over SMI PASSED.\n";
}

static void test_smi_gap_standard_tier_without_cswitch() {
    std::cout << "[TEST] Validating SMI / Hardware Gap fires on Standard Tier (without cswitch provider)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Standard tier: kernel_dpc is active, but kernel_cswitch is inactive
    stuttometer::ProviderContext p_standard;
    p_standard.kernel_dpc_active = true;
    p_standard.kernel_cswitch_active = false;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_standard);
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "unprofiled_hardware_or_smi_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence == 0.30);
    std::cout << "  -> Standard tier SMI gap detection PASSED.\n";
}

static void test_page_fault_stall_correlation() {
    std::cout << "[TEST] Validating Hard Page Fault Stall Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord pf{};
    pf.category = static_cast<uint16_t>(stuttometer::EventCategory::PAGE_FAULT);
    pf.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    pf.pid = 4000;
    pf.tid = 8000;
    pf.duration_us = 8000; // 8.0ms hard page fault
    pf.auxiliary_data = 65536; // 64 KB read
    pf.payload.file_key = 0xFFFFFA8001234567ULL; // FILE_OBJECT pointer
    snapshot.push_back(pf);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_pagefault_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "page_fault_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.55);
    STUTTO_ASSERT(report.event_counts.page_fault == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_thermal_throttle_correlation() {
    std::cout << "[TEST] Validating CPU Thermal Throttle Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord tt{};
    tt.category = static_cast<uint16_t>(stuttometer::EventCategory::THERMAL_THROTTLE);
    tt.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    tt.cpu_index = 2;
    tt.auxiliary_data = 15; // 15 seconds throttled
    tt.duration_us = 0; // Ambient state
    snapshot.push_back(tt);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_processor_power_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "thermal_throttle");
    STUTTO_ASSERT(std::abs(report.diagnoses[0].confidence - 0.6625) < 0.001);
    STUTTO_ASSERT(report.event_counts.thermal_throttle == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_antimalware_interference_correlation() {
    std::cout << "[TEST] Validating Antimalware Interference Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord am{};
    am.category = static_cast<uint16_t>(stuttometer::EventCategory::ANTIMALWARE_SCAN);
    am.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(20.0, qpc_freq);
    am.duration_us = 25000; // 25ms real-time scan
    snapshot.push_back(am);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_antimalware_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "antimalware_interference");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.45);
    STUTTO_ASSERT(report.event_counts.antimalware_scan == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_smi_gap_suppression_on_autodetect_core_preemption() {
    std::cout << "[TEST] Validating SMI Gap suppression on cumulative sub-threshold preemption in auto-detect mode...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 0; // Auto-detect mode
    trigger.target_tid = 0;
    trigger.cpu_index = 2;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Two 3ms involuntary preemptions on Core 2 (each below 5ms threshold individually, but total = 6ms on Core 2)
    stuttometer::EtwEventRecord cs1{};
    cs1.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs1.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    cs1.cpu_index = 2;
    cs1.duration_us = 3000;
    cs1.tid = 2001;
    cs1.payload.cswitch.prev_tid = 1001;
    snapshot.push_back(cs1);

    stuttometer::EtwEventRecord cs2{};
    cs2.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs2.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    cs2.cpu_index = 2;
    cs2.duration_us = 3000;
    cs2.tid = 2002;
    cs2.payload.cswitch.prev_tid = 1002;
    snapshot.push_back(cs2);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = true;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    // SMI gap must NOT be diagnosed because Core 2 had 6ms of preemption
    for (const auto& diag : report.diagnoses) {
        STUTTO_ASSERT(diag.hypothesis != "unprofiled_hardware_or_smi_stall");
    }
    std::cout << "  -> Auto-detect cumulative preemption SMI suppression PASSED.\n";
}

static void test_smi_gap_autodetect_unrelated_core_preemption_does_not_suppress_smi() {
    std::cout << "[TEST] Validating auto-detect SMI is NOT suppressed by preemption on unrelated core...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 0; // Auto-detect mode
    trigger.target_tid = 0;
    trigger.cpu_index = 1; // Trigger on Core 1

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Two 3ms involuntary preemptions on Core 7 (unrelated to Core 1, cumulative 6ms on Core 7)
    stuttometer::EtwEventRecord cs1{};
    cs1.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs1.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    cs1.cpu_index = 7; // Unrelated Core 7
    cs1.duration_us = 3000;
    cs1.tid = 7001;
    cs1.payload.cswitch.prev_tid = 7002;
    snapshot.push_back(cs1);

    stuttometer::EtwEventRecord cs2{};
    cs2.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs2.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    cs2.cpu_index = 7; // Unrelated Core 7
    cs2.duration_us = 3000;
    cs2.tid = 7003;
    cs2.payload.cswitch.prev_tid = 7004;
    snapshot.push_back(cs2);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = true;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    // SMI stall SHOULD be diagnosed for Core 1 despite Core 7 preemption
    bool has_smi = false;
    for (const auto& diag : report.diagnoses) {
        if (diag.hypothesis == "unprofiled_hardware_or_smi_stall") has_smi = true;
    }
    STUTTO_ASSERT(has_smi);
    std::cout << "  -> Auto-detect unrelated core preemption non-suppression PASSED.\n";
}

static void test_d3d12_pso_compilation_correlation() {
    std::cout << "[TEST] Validating Direct3D 12 PSO Compilation Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 5000;
    trigger.target_tid = 10000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // 1. A 35ms D3D12 PSO compilation event on the render thread occurring 5ms before trigger
    stuttometer::EtwEventRecord pso{};
    pso.category = static_cast<uint16_t>(stuttometer::EventCategory::D3D12_PSO_CREATE);
    pso.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    pso.duration_us = 35000; // 35ms
    pso.pid = 5000;
    pso.tid = 10000;
    pso.auxiliary_data = 0x7FFE12345678ULL;
    pso.flags = stuttometer::EventFlags::D3D12_GRAPHICS_PSO;
    snapshot.push_back(pso);

    // 2. A sub-threshold 2ms PSO creation (should not trigger diagnosis on its own)
    stuttometer::EtwEventRecord pso_sub{};
    pso_sub.category = static_cast<uint16_t>(stuttometer::EventCategory::D3D12_PSO_CREATE);
    pso_sub.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    pso_sub.duration_us = 2000;
    pso_sub.pid = 5000;
    pso_sub.tid = 10000;
    snapshot.push_back(pso_sub);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_d3d12_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "d3d12_shader_pso_compilation_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.80);
    STUTTO_ASSERT(report.event_counts.d3d12_pso_create == 2);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_vram_exhaustion_paging_correlation() {
    std::cout << "[TEST] Validating GPU VRAM Exhaustion & PCIe Paging Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 4455;
    trigger.target_tid = 8899;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // 1. A 45 MB demoted commitment event on target PID occurring 10ms before trigger
    stuttometer::EtwEventRecord vram{};
    vram.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
    vram.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    vram.pid = 4455;
    vram.tid = 8899;
    vram.auxiliary_data = 45 * 1024 * 1024ULL; // 45 MB demoted
    vram.flags = stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT;
    snapshot.push_back(vram);

    // 2. A sub-threshold 2 MB VRAM demotion (should not trigger candidate threshold)
    stuttometer::EtwEventRecord vram_sub{};
    vram_sub.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
    vram_sub.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    vram_sub.pid = 4455;
    vram_sub.auxiliary_data = 2 * 1024 * 1024ULL;
    vram_sub.flags = stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT;
    snapshot.push_back(vram_sub);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_vram_paging_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "vram_exhaustion_paging_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.80);
    STUTTO_ASSERT(report.event_counts.dxgkrnl_vram_paging == 2);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_gpu_pipeline_stall_suppression_by_vram_candidates() {
    std::cout << "[TEST] Validating gpu_pipeline_stall suppression when VRAM candidates are present...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::KERNEL_FRAME_STALL;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4455;
    trigger.target_tid = 8899;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord vram{};
    vram.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
    vram.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    vram.pid = 4455;
    vram.tid = 8899;
    vram.auxiliary_data = 30 * 1024 * 1024ULL; // 30 MB demoted
    vram.flags = stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT;
    snapshot.push_back(vram);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_vram_paging_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    // Verify vram_exhaustion_paging_stall is diagnosed and gpu_pipeline_stall is NOT present
    bool has_vram = false;
    bool has_gpu_pipe = false;
    for (const auto& diag : report.diagnoses) {
        if (diag.hypothesis == "vram_exhaustion_paging_stall") has_vram = true;
        if (diag.hypothesis == "gpu_pipeline_stall") has_gpu_pipe = true;
    }
    STUTTO_ASSERT(has_vram);
    STUTTO_ASSERT(!has_gpu_pipe);
    std::cout << "  -> gpu_pipeline_stall suppressed by VRAM paging candidate PASSED.\n";
}

static void test_gpu_pipeline_stall_suppression_by_memory_and_pagefault_candidates() {
    std::cout << "[TEST] Validating gpu_pipeline_stall suppression when memory or page fault candidates are present...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::KERNEL_FRAME_STALL;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4455;
    trigger.target_tid = 8899;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Add a 64MB VirtualAlloc candidate
    stuttometer::EtwEventRecord mem{};
    mem.category = static_cast<uint16_t>(stuttometer::EventCategory::MEM_VIRTUAL_ALLOC);
    mem.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    mem.pid = 4455;
    mem.auxiliary_data = 64 * 1024 * 1024ULL; // 64 MB
    mem.flags = stuttometer::EventFlags::MEM_ALLOC_COMMIT;
    snapshot.push_back(mem);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_pagefault_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    bool has_mem = false;
    bool has_gpu_pipe = false;
    for (const auto& diag : report.diagnoses) {
        if (diag.hypothesis == "virtual_memory_allocation_stall") has_mem = true;
        if (diag.hypothesis == "gpu_pipeline_stall") has_gpu_pipe = true;
    }
    STUTTO_ASSERT(has_mem);
    STUTTO_ASSERT(!has_gpu_pipe);
    std::cout << "  -> gpu_pipeline_stall suppressed by memory allocation candidate PASSED.\n";
}

static void test_gpu_pipeline_stall_suppression_by_cswitch_candidates() {
    std::cout << "[TEST] Validating gpu_pipeline_stall suppression when CSwitch preemption candidates are present...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::KERNEL_FRAME_STALL;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 4455;
    trigger.target_tid = 8899;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Add an involuntary context-switch preemption event on target thread (15ms duration >= 5ms threshold)
    stuttometer::EtwEventRecord cs{};
    cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    cs.pid = 4455;
    cs.tid = 8899;
    cs.duration_us = 15000; // 15ms preemption
    cs.flags = 0; // Involuntary
    cs.payload.cswitch.prev_tid = 9999;
    cs.payload.cswitch.prev_pid = 1111;
    snapshot.push_back(cs);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    bool has_cswitch = false;
    bool has_gpu_pipe = false;
    for (const auto& diag : report.diagnoses) {
        if (diag.hypothesis == "context_switch_interference") has_cswitch = true;
        if (diag.hypothesis == "gpu_pipeline_stall") has_gpu_pipe = true;
    }
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "context_switch_interference");
    STUTTO_ASSERT(report.diagnoses[0].rank == 1);
    STUTTO_ASSERT(has_cswitch);
    STUTTO_ASSERT(!has_gpu_pipe);
    std::cout << "  -> gpu_pipeline_stall suppressed by CSwitch candidate PASSED.\n";
}

static void test_virtual_alloc_stall_correlation() {
    std::cout << "[TEST] Validating VirtualAlloc Commit Stall Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 5000;
    trigger.target_tid = 9000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord valloc{};
    valloc.category = static_cast<uint16_t>(stuttometer::EventCategory::MEM_VIRTUAL_ALLOC);
    valloc.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(8.0, qpc_freq);
    valloc.pid = 5000;
    valloc.tid = 9000;
    valloc.auxiliary_data = 64 * 1024 * 1024ULL; // 64 MB
    valloc.flags = stuttometer::EventFlags::MEM_ALLOC_COMMIT;
    snapshot.push_back(valloc);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_pagefault_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "virtual_memory_allocation_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.70);
    STUTTO_ASSERT(report.diagnoses[0].rank == 1);
    STUTTO_ASSERT(report.event_counts.mem_virtual_alloc == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_working_set_trim_correlation() {
    std::cout << "[TEST] Validating Working Set Out-Swap Trim Stall Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 50.0;
    trigger.target_pid = 5000;
    trigger.target_tid = 9000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord wstrim{};
    wstrim.category = static_cast<uint16_t>(stuttometer::EventCategory::MEM_WORKING_SET_TRIM);
    wstrim.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(12.0, qpc_freq);
    wstrim.pid = 5000;
    wstrim.duration_us = 15000; // 15ms duration
    wstrim.auxiliary_data = 16 * 1024 * 1024ULL; // 16 MB trimmed
    wstrim.flags = stuttometer::EventFlags::MEM_WS_TRIM_OUTSWAP;
    snapshot.push_back(wstrim);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_memory_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "low_memory_working_set_trim_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.75);
    STUTTO_ASSERT(report.diagnoses[0].rank == 1);
    STUTTO_ASSERT(report.event_counts.mem_working_set_trim == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_physical_memory_latency_correlation() {
    std::cout << "[TEST] Validating Physical Memory Allocation Latency Hypothesis...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 5000;
    trigger.target_tid = 9000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    stuttometer::EtwEventRecord phys{};
    phys.category = static_cast<uint16_t>(stuttometer::EventCategory::MEM_PHYSICAL_ALLOC);
    phys.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    phys.pid = 0;
    phys.duration_us = 4500; // 4.5ms (> 1000us)
    phys.auxiliary_data = 8 * 1024 * 1024ULL; // 8 MB
    phys.flags = stuttometer::EventFlags::MEM_PHYSICAL_CONTIGUOUS;
    snapshot.push_back(phys);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_memory_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "physical_memory_allocation_latency");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.60);
    STUTTO_ASSERT(report.diagnoses[0].rank == 1);
    STUTTO_ASSERT(report.event_counts.mem_physical_alloc == 1);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_correlator_attribution_pipeline_wiring() {
    std::cout << "[TEST] Validating Attribution & Timeline Pipeline Wiring with Modern CorrelateOptions...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = 4000;
    trigger.target_tid = 8000;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Normal frame before stutter
    stuttometer::EtwEventRecord frame_normal{};
    frame_normal.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    frame_normal.event_id = 43;
    frame_normal.pid = 4000;
    frame_normal.tid = 8000;
    frame_normal.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
    frame_normal.duration_us = 16000; // 16.0ms
    snapshot.push_back(frame_normal);

    // Trigger frame
    stuttometer::EtwEventRecord frame_trigger{};
    frame_trigger.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    frame_trigger.event_id = 43;
    frame_trigger.pid = 4000;
    frame_trigger.tid = 8000;
    frame_trigger.qpc_timestamp = trigger.trigger_timestamp_qpc;
    frame_trigger.duration_us = 45000; // 45.0ms
    snapshot.push_back(frame_trigger);

    // Severe DPC spike (3500us > 1000us threshold)
    stuttometer::EtwEventRecord dpc{};
    dpc.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
    dpc.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    dpc.duration_us = 3500;
    dpc.cpu_index = 2;
    dpc.payload.routine_addr = 0xFFFFF80012340000ULL;
    snapshot.push_back(dpc);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = true;

    stuttometer::CorrelateOptions opts{
        .window_pre_ms = 250.0,
        .window_post_ms = 30.0,
        .present_threshold_ms = 16.67,
        .provider_tier = "standard",
        .redact = false
    };

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts, p_ctx);

    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "dpc_isr_spike");
    STUTTO_ASSERT(report.attribution == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(report.attribution_pid == 4);
    STUTTO_ASSERT(!report.attribution_process.empty());
    STUTTO_ASSERT(!report.attribution_redacted);

    STUTTO_ASSERT(report.frame_timeline.size() == 2);
    STUTTO_ASSERT(!report.frame_timeline[0].is_trigger_frame);
    STUTTO_ASSERT(!report.frame_timeline[0].is_pacing_stall);
    STUTTO_ASSERT(report.frame_timeline[1].is_trigger_frame);
    STUTTO_ASSERT(report.frame_timeline[1].is_pacing_stall);

    std::cout << "  -> Attribution: " << stuttometer::attribution_to_string(report.attribution)
              << " (" << report.attribution_process << " PID " << report.attribution_pid << ") PASSED.\n";
    std::cout << "  -> Frame timeline points retained: " << report.frame_timeline.size() << " PASSED.\n";
}

static void test_cswitch_targeted_autodetect_filtering() {
    std::cout << "[TEST] Validating Targeted CSwitch Auto-Detect Filtering (target_pid != 0, target_tid == 0)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 1234;
    trigger.target_tid = 0; // Auto-detect threads of PID 1234

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Involuntary preemption of unrelated background PID 9999
    stuttometer::EtwEventRecord cs_unrelated{};
    cs_unrelated.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs_unrelated.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
    cs_unrelated.pid = 9999;
    cs_unrelated.tid = 5555;
    cs_unrelated.duration_us = 15000;
    cs_unrelated.flags = 0; // Involuntary
    cs_unrelated.payload.cswitch.prev_pid = 8888;
    cs_unrelated.payload.cswitch.prev_tid = 7777;
    snapshot.push_back(cs_unrelated);

    stuttometer::ProviderContext p_ctx;
    p_ctx.kernel_cswitch_active = true;

    auto report_unrelated = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
    // Should NOT diagnose context_switch_interference because PID 9999 is not target_pid 1234
    for (const auto& diag : report_unrelated.diagnoses) {
        STUTTO_ASSERT(diag.hypothesis != "context_switch_interference");
    }

    // Now add involuntary preemption of target PID 1234
    stuttometer::EtwEventRecord cs_target{};
    cs_target.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs_target.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    cs_target.pid = 1234;
    cs_target.tid = 2222;
    cs_target.duration_us = 12000;
    cs_target.flags = 0; // Involuntary
    cs_target.payload.cswitch.prev_pid = 9999;
    cs_target.payload.cswitch.prev_tid = 5555;
    snapshot.push_back(cs_target);

    auto report_target = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(!report_target.diagnoses.empty());
    STUTTO_ASSERT(report_target.diagnoses[0].hypothesis == "context_switch_interference");
    STUTTO_ASSERT(report_target.diagnoses[0].confidence >= 0.65);

    std::cout << "  -> Targeted CSwitch Auto-Detect filtering PASSED.\n";
}

static void test_dwm_glitch_suppresses_gpu_pipeline_stall() {
    std::cout << "[TEST] Validating DWM Glitch Suppresses GPU Pipeline Stall...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::KERNEL_FRAME_STALL;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
    trigger.duration_ms = 40.0;
    trigger.target_pid = 1234;
    trigger.target_tid = 5678;

    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Add a DWM glitch event in the snapshot
    stuttometer::EtwEventRecord dwm{};
    dwm.category = static_cast<uint16_t>(stuttometer::EventCategory::DWM_GLITCH);
    dwm.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    dwm.duration_us = 33000; // 33ms DWM compositor stall
    dwm.auxiliary_data = 1; // glitch type
    snapshot.push_back(dwm);

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_dwm_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);

    // dwm_compositor_stall must be diagnosed
    bool has_dwm = false;
    bool has_gpu = false;
    for (const auto& diag : report.diagnoses) {
        if (diag.hypothesis == "dwm_compositor_stall") has_dwm = true;
        if (diag.hypothesis == "gpu_pipeline_stall") has_gpu = true;
    }
    STUTTO_ASSERT(has_dwm);
    STUTTO_ASSERT(!has_gpu); // gpu_pipeline_stall suppressed by DWM candidates!

    std::cout << "  -> DWM compositor stall diagnosed and GPU pipeline stall suppressed PASSED.\n";
}

static void test_monitor_all_glitch_attribution() {
    std::cout << "[TEST] Validating Monitor-All Glitch Attribution (target_pid == 0)...\n";

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    // 1. DWM glitch trigger in monitor-all mode (target_pid == 0)
    stuttometer::TriggerInfo dwm_trigger;
    dwm_trigger.source = stuttometer::TriggerSource::DWM_GLITCH;
    dwm_trigger.trigger_timestamp_qpc = 1000000;
    dwm_trigger.duration_ms = 33.3;
    dwm_trigger.target_pid = 0;
    dwm_trigger.target_tid = 0;

    std::vector<stuttometer::EtwEventRecord> snapshot;
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::ProviderContext p_ctx;

    auto dwm_report = correlator.correlate(snapshot, dwm_trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(dwm_report.target_process.empty());
    STUTTO_ASSERT(dwm_report.trigger.target_pid == 0);

    // 2. Audio glitch trigger in monitor-all mode (target_pid == 0)
    stuttometer::TriggerInfo audio_trigger;
    audio_trigger.source = stuttometer::TriggerSource::AUDIO_GLITCH;
    audio_trigger.trigger_timestamp_qpc = 1000000;
    audio_trigger.glitch_count = 3;
    audio_trigger.target_pid = 0;
    audio_trigger.target_tid = 0;

    auto audio_report = correlator.correlate(snapshot, audio_trigger, qpc_freq, p_ctx);
    STUTTO_ASSERT(audio_report.target_process.empty());
    STUTTO_ASSERT(audio_report.trigger.target_pid == 0);

    // Also verify TriggerEngine itself produces effective_pid = 0 in monitor-all mode
    // DWM glitch with DWM PID 999
    {
        stuttometer::TriggerConfig trg_cfg;
        stuttometer::TriggerEngine engine(trg_cfg, qpc_freq);
        engine.update_target_pid(0); // Monitor all mode
        bool dwm_fired = engine.on_dwm_glitch(999, 1000, 33.3, 1000000, 0);
        STUTTO_ASSERT(dwm_fired);
        stuttometer::TriggerInfo trg_out{};
        uint64_t from_q = 0, to_q = 0;
        bool polled = engine.poll_state(1000000 + qpc_freq, trg_out, from_q, to_q);
        STUTTO_ASSERT(polled);
        STUTTO_ASSERT(trg_out.target_pid == 0);
        STUTTO_ASSERT(trg_out.target_tid == 0);
    }

    // Audio glitch with audiodg PID 888
    {
        stuttometer::TriggerConfig trg_cfg;
        trg_cfg.audio_trigger_enabled = true;
        stuttometer::TriggerEngine engine(trg_cfg, qpc_freq);
        engine.update_target_pid(0); // Monitor all mode
        bool audio_fired = engine.on_audio_glitch(888, 1001, 2, 2000000, 0);
        STUTTO_ASSERT(audio_fired);
        stuttometer::TriggerInfo trg_out{};
        uint64_t from_q = 0, to_q = 0;
        bool polled = engine.poll_state(2000000 + qpc_freq, trg_out, from_q, to_q);
        STUTTO_ASSERT(polled);
        STUTTO_ASSERT(trg_out.target_pid == 0);
        STUTTO_ASSERT(trg_out.target_tid == 0);
    }

    std::cout << "  -> Monitor-all glitch attribution verification PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Correlation Engine Tests ===\n";
    try {
        test_dpc_spike_correlation();
        test_disk_stall_correlation();
        test_auto_detect_disk_stall_correlation();
        test_cswitch_preemption_correlation();
        test_cswitch_voluntary_wait_ignored();
        test_cswitch_switch_out_duration_non_pollution();
        test_cswitch_autodetect_mode_correlation();
        test_cswitch_targeted_autodetect_filtering();
        test_dwm_compositor_stall_correlation();
        test_dwm_glitch_suppresses_gpu_pipeline_stall();
        test_dwm_secondary_diagnosis_and_smi_suppression();
        test_monitor_all_glitch_attribution();
        test_page_fault_stall_correlation();
        test_thermal_throttle_correlation();
        test_antimalware_interference_correlation();
        test_d3d12_pso_compilation_correlation();
        test_vram_exhaustion_paging_correlation();
        test_gpu_pipeline_stall_suppression_by_vram_candidates();
        test_gpu_pipeline_stall_suppression_by_memory_and_pagefault_candidates();
        test_gpu_pipeline_stall_suppression_by_cswitch_candidates();
        test_virtual_alloc_stall_correlation();
        test_working_set_trim_correlation();
        test_physical_memory_latency_correlation();
        test_smi_hardware_gap_and_provider_awareness();
        test_smi_gap_suppression_on_target_preemption();
        test_smi_gap_suppression_on_autodetect_core_preemption();
        test_smi_gap_autodetect_unrelated_core_preemption_does_not_suppress_smi();
        test_smi_gap_standard_tier_without_cswitch();
        test_audio_glitch_smi_gap_correlation();
        test_smi_gap_with_benign_dpcs();
        test_correlator_attribution_pipeline_wiring();
        std::cout << ">>> All Correlation Engine tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
