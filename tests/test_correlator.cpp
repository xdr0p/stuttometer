#include "test_common.hpp"
#include "stuttometer/correlator.hpp"
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
    trigger.target_process = "Game.exe";

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
    trigger.target_process = "Game.exe";

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
    trigger.target_process = "Game.exe";

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
    trigger.target_process = "Game.exe";

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

int main() {
    std::cout << "=== Stuttometer Correlation Engine Tests ===\n";
    test_dpc_spike_correlation();
    test_disk_stall_correlation();
    test_cswitch_preemption_correlation();
    test_smi_hardware_gap_and_provider_awareness();
    std::cout << ">>> All Correlation Engine tests PASSED! <<<\n\n";
    return 0;
}
