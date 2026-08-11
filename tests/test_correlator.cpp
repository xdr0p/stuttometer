#include "stuttometer/correlator.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <cassert>

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

    // Inject a severe 3.5ms DPC routine 5ms before trigger
    stuttometer::EtwEventRecord dpc{};
    dpc.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
    dpc.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    dpc.duration_us = 3500; // 3.5ms
    dpc.cpu_index = 3;
    dpc.payload.routine_addr = 0xFFFFF80012340000ULL;
    snapshot.push_back(dpc);

    auto report = correlator.correlate(snapshot, trigger, qpc_freq);

    assert(!report.diagnoses.empty());
    assert(report.diagnoses[0].hypothesis == "dpc_isr_spike");
    assert(report.diagnoses[0].confidence >= 0.80);
    assert(report.diagnoses[0].rank == 1);
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

    // Inject 48ms synchronous disk read by the target process
    stuttometer::EtwEventRecord disk{};
    disk.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
    disk.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    disk.pid = 4000;
    disk.duration_us = 48000; // 48ms
    disk.auxiliary_data = 1048576; // 1 MB
    snapshot.push_back(disk);

    auto report = correlator.correlate(snapshot, trigger, qpc_freq);

    assert(!report.diagnoses.empty());
    assert(report.diagnoses[0].hypothesis == "disk_io_stall");
    assert(report.diagnoses[0].confidence >= 0.70);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_cswitch_preemption_correlation() {
    std::cout << "[TEST] Validating Context Switch Involuntary Preemption Hypothesis...\n";

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

    // Inject 12ms involuntary preemption of target thread 8000
    stuttometer::EtwEventRecord cs{};
    cs.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
    cs.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
    cs.pid = 4000;
    cs.tid = 8000; // Target thread resumed
    cs.duration_us = 12000; // Descheduled for 12ms
    cs.flags = 0; // Involuntary (CSWITCH_VOLUNTARY not set)
    cs.payload.cswitch.prev_tid = 9999; // Preempting thread
    snapshot.push_back(cs);

    auto report = correlator.correlate(snapshot, trigger, qpc_freq);

    assert(!report.diagnoses.empty());
    assert(report.diagnoses[0].hypothesis == "context_switch_interference");
    assert(report.diagnoses[0].confidence >= 0.65);
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence) PASSED.\n";
}

static void test_smi_hardware_gap_correlation() {
    std::cout << "[TEST] Validating Constrained Hardware / SMI Gap Hypothesis...\n";

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

    // Empty snapshot (no DPCs, no disk stalls, no CSwitches)
    std::vector<stuttometer::EtwEventRecord> snapshot;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq);

    assert(!report.diagnoses.empty());
    assert(report.diagnoses[0].hypothesis == "unprofiled_hardware_or_smi_stall");
    assert(report.diagnoses[0].confidence <= 0.35); // Strictly capped <= 0.35
    std::cout << "  -> Rank 1: " << report.diagnoses[0].hypothesis 
              << " (" << (report.diagnoses[0].confidence * 100.0) << "% confidence, capped <= 35%) PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Correlation Engine Tests ===\n";
    test_dpc_spike_correlation();
    test_disk_stall_correlation();
    test_cswitch_preemption_correlation();
    test_smi_hardware_gap_correlation();
    std::cout << ">>> All Correlation Engine tests PASSED! <<<\n\n";
    return 0;
}
