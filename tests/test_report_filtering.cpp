#include "test_common.hpp"
#include "stuttometer/filtered_event_ring.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>

using namespace stuttometer;

void test_filtered_event_layout() {
    std::cout << "[TEST] Running test_filtered_event_layout...\n";
    static_assert(sizeof(FilteredEvent) == 56, "FilteredEvent must be exactly 56 bytes");
    static_assert(alignof(FilteredEvent) == 8, "FilteredEvent must be 8-byte aligned");
    static_assert(offsetof(FilteredEvent, qpc_timestamp) == 0);
    static_assert(offsetof(FilteredEvent, duration_ms) == 8);
    static_assert(offsetof(FilteredEvent, baseline_avg_ms) == 16);
    static_assert(offsetof(FilteredEvent, spike_ratio) == 24);
    static_assert(offsetof(FilteredEvent, target_pid) == 32);
    static_assert(offsetof(FilteredEvent, target_tid) == 36);
    static_assert(offsetof(FilteredEvent, reason) == 40);
    static_assert(offsetof(FilteredEvent, source) == 42);
    static_assert(offsetof(FilteredEvent, severity) == 44);
    static_assert(offsetof(FilteredEvent, cpu_index) == 45);
    static_assert(offsetof(FilteredEvent, filter_kind) == 46);
    STUTTO_ASSERT(sizeof(FilteredEvent) == 56);
    STUTTO_ASSERT(alignof(FilteredEvent) == 8);
}

void test_spsc_sanity() {
    std::cout << "[TEST] Running test_spsc_sanity...\n";
    FilteredEventRing ring;

    FilteredEvent dummy;
    STUTTO_ASSERT(!ring.pop(dummy)); // Pop on empty returns false

    FilteredEvent ev1{};
    ev1.qpc_timestamp   = 1000000;
    ev1.duration_ms     = 16.6;
    ev1.baseline_avg_ms = 8.3;
    ev1.spike_ratio     = 2.0;
    ev1.target_pid      = 1234;
    ev1.target_tid      = 5678;
    ev1.reason          = static_cast<uint16_t>(TriggerReason::RELATIVE_SPIKE);
    ev1.source          = static_cast<uint16_t>(TriggerSource::DXGI_PRESENT_STUTTER);
    ev1.severity        = static_cast<uint8_t>(MetricSeverity::WARNING);
    ev1.cpu_index       = 3;
    ev1.filter_kind     = static_cast<uint8_t>(FilterKind::SEVERITY_GATE);

    ring.push(ev1);

    FilteredEvent out{};
    STUTTO_ASSERT(ring.pop(out));
    STUTTO_ASSERT(out.qpc_timestamp == 1000000);
    STUTTO_ASSERT(out.duration_ms == 16.6);
    STUTTO_ASSERT(out.baseline_avg_ms == 8.3);
    STUTTO_ASSERT(out.spike_ratio == 2.0);
    STUTTO_ASSERT(out.target_pid == 1234);
    STUTTO_ASSERT(out.target_tid == 5678);
    STUTTO_ASSERT(out.reason == static_cast<uint16_t>(TriggerReason::RELATIVE_SPIKE));
    STUTTO_ASSERT(out.source == static_cast<uint16_t>(TriggerSource::DXGI_PRESENT_STUTTER));
    STUTTO_ASSERT(out.severity == static_cast<uint8_t>(MetricSeverity::WARNING));
    STUTTO_ASSERT(out.cpu_index == 3);
    STUTTO_ASSERT(out.filter_kind == static_cast<uint8_t>(FilterKind::SEVERITY_GATE));

    STUTTO_ASSERT(!ring.pop(dummy)); // Ring is empty again
}

void test_inflight_ticket_hold() {
    std::cout << "[TEST] Running test_inflight_ticket_hold...\n";
    FilteredEventRing ring;

    // Simulate writer having claimed cell 0 (sequence = 1, writing in progress)
    ring.set_cell_sequence_for_test(0, 1);
    ring.set_write_pos_for_test(1);
    ring.set_read_pos_for_test(0);

    FilteredEvent out{};
    bool popped = ring.pop(out);
    STUTTO_ASSERT(!popped);
    STUTTO_ASSERT(ring.get_read_pos_for_test() == 0); // Reader did not advance!

    // Writer finishes writing (sequence = 2, ready)
    FilteredEvent ev{};
    ev.qpc_timestamp = 42;
    ring.set_cell_event_for_test(0, ev);
    ring.set_cell_sequence_for_test(0, 2);

    popped = ring.pop(out);
    STUTTO_ASSERT(popped);
    STUTTO_ASSERT(out.qpc_timestamp == 42);
    STUTTO_ASSERT(ring.get_read_pos_for_test() == 1); // Reader advanced!
}

void test_lapped_cell_drain() {
    std::cout << "[TEST] Running test_lapped_cell_drain...\n";
    FilteredEventRing ring;

    // Cell 0 is lapped by a writer from a later cycle (sequence > expected_ready)
    // For r=0, expected_writing=1, expected_ready=2.
    // Set cell 0 sequence to 10 (a future cycle), and cell 1 sequence to 4 (ready for r=1).
    FilteredEvent ev1{};
    ev1.qpc_timestamp = 999;
    ring.set_cell_event_for_test(1, ev1);

    ring.set_cell_sequence_for_test(0, 10); // Lapped dead cell
    ring.set_cell_sequence_for_test(1, 4);  // Valid ready cell for read index 1
    ring.set_write_pos_for_test(2);
    ring.set_read_pos_for_test(0);

    FilteredEvent out{};
    bool popped = ring.pop(out);
    // Reader should skip cell 0 and drain cell 1
    STUTTO_ASSERT(popped);
    STUTTO_ASSERT(out.qpc_timestamp == 999);
    STUTTO_ASSERT(ring.get_read_pos_for_test() == 2);
}

void test_mpsc_concurrency_stress() {
    std::cout << "[TEST] Running test_mpsc_concurrency_stress (4 producers, 4000 total events)...\n";
    FilteredEventRing ring;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int EVENTS_PER_PRODUCER = 1000;
    constexpr int TOTAL_EVENTS = NUM_PRODUCERS * EVENTS_PER_PRODUCER;

    std::atomic<bool> start_signal{false};
    std::atomic<bool> producers_done{false};
    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);

    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&ring, &start_signal, p]() {
            while (!start_signal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < EVENTS_PER_PRODUCER; ++i) {
                FilteredEvent ev{};
                const uint64_t val = (static_cast<uint64_t>(p) << 32) | static_cast<uint64_t>(i);
                ev.qpc_timestamp = val;
                ev.duration_ms = static_cast<double>(val) * 1.5;
                ev.baseline_avg_ms = 10.0;
                ev.spike_ratio = 1.0;
                ev.target_pid = static_cast<uint32_t>(p);
                ev.target_tid = static_cast<uint32_t>(i);
                ev.reason = static_cast<uint16_t>(TriggerReason::STATIC_THRESHOLD);
                ev.source = static_cast<uint16_t>(TriggerSource::DXGI_PRESENT_STUTTER);
                ev.severity = static_cast<uint8_t>(MetricSeverity::WARNING);
                ev.cpu_index = static_cast<uint8_t>(p % 8);
                ev.filter_kind = static_cast<uint8_t>(FilterKind::SEVERITY_GATE);
                ring.push(ev);
            }
        });
    }

    uint64_t popped_count = 0;
    uint64_t torn_reads = 0;

    std::thread consumer([&ring, &producers_done, &popped_count, &torn_reads]() {
        FilteredEvent out{};
        while (!producers_done.load(std::memory_order_acquire)) {
            while (ring.pop(out)) {
                ++popped_count;
                // Verify event consistency (0 torn reads)
                const double expected_dur = static_cast<double>(out.qpc_timestamp) * 1.5;
                if (out.duration_ms != expected_dur ||
                    out.target_pid != static_cast<uint32_t>(out.qpc_timestamp >> 32) ||
                    out.target_tid != static_cast<uint32_t>(out.qpc_timestamp & 0xFFFFFFFFULL)) {
                    ++torn_reads;
                }
            }
            std::this_thread::yield();
        }
        // Drain remaining
        while (ring.pop(out)) {
            ++popped_count;
            const double expected_dur = static_cast<double>(out.qpc_timestamp) * 1.5;
            if (out.duration_ms != expected_dur ||
                out.target_pid != static_cast<uint32_t>(out.qpc_timestamp >> 32) ||
                out.target_tid != static_cast<uint32_t>(out.qpc_timestamp & 0xFFFFFFFFULL)) {
                ++torn_reads;
            }
        }
    });

    start_signal.store(true, std::memory_order_release);

    for (auto& th : producers) {
        th.join();
    }
    producers_done.store(true, std::memory_order_release);
    consumer.join();

    const uint64_t dropped = ring.dropped_filtered();
    std::cout << "  -> Popped: " << popped_count << ", Dropped: " << dropped
              << ", Total accounted: " << (popped_count + dropped)
              << ", write_pos: " << ring.get_write_pos_for_test()
              << ", read_pos: " << ring.get_read_pos_for_test() << "\n";
    STUTTO_ASSERT(torn_reads == 0);
    STUTTO_ASSERT(popped_count + dropped == TOTAL_EVENTS);
}

void test_trigger_engine_preclaim_gate() {
    std::cout << "[TEST] Running test_trigger_engine_preclaim_gate...\n";
    TriggerConfig config{};
    config.present_threshold_ms = 16.67;
    config.min_report_severity = ReportSeverity::WARNING;
    const uint64_t qpc_freq = 10000000;

    TriggerEngine engine(config, qpc_freq);
    STUTTO_ASSERT(engine.filtered_reports() == 0);
    STUTTO_ASSERT(engine.filtered_stall_ms() == 0);

    // Warm up the baseline with 10 normal frames (16.6ms each)
    uint64_t ts = 1000000;
    for (int i = 0; i < 10; ++i) {
        engine.on_dxgi_present(1234, 5678, 16.6, ts);
        ts += 166000;
    }

    // Now introduce a micro-stutter (duration 20.0ms: spike_ratio ~ 1.2x, duration < 1.5*16.67=25ms -> NORMAL)
    // Under min_report_severity == WARNING, this must be filtered!
    bool triggered = engine.on_dxgi_present(1234, 5678, 20.0, ts);
    // Even if detector evaluates it, because severity is NORMAL, the pre-claim gate must filter it
    STUTTO_ASSERT(!triggered);
    STUTTO_ASSERT(engine.current_state() == TriggerState::ARMED);
    STUTTO_ASSERT(engine.suppressed_trigger_count() == 0);
    STUTTO_ASSERT(engine.filtered_reports() == 1);
    STUTTO_ASSERT(engine.filtered_stall_ms() == 20); // 20.0 ms

    FilteredEvent fe{};
    STUTTO_ASSERT(engine.pop_filtered_event(fe));
    STUTTO_ASSERT(fe.filter_kind == static_cast<uint8_t>(FilterKind::SEVERITY_GATE));
    STUTTO_ASSERT(fe.severity == static_cast<uint8_t>(MetricSeverity::NORMAL));
    STUTTO_ASSERT(fe.duration_ms == 20.0);
}

static void test_diagnostic_toggles() {
    std::cout << "[TEST] Running test_diagnostic_toggles...\n";
    const uint64_t qpc_freq = 10000000ULL;
    TriggerConfig config{};
    config.present_threshold_ms = 50.0;
    config.min_report_severity = ReportSeverity::ALL;
    config.enable_relative_spike = false;
    config.enable_kernel_frame_stall = false;
    config.enable_dwm_glitch = false;

    TriggerEngine engine(config, qpc_freq);
    engine.update_target_pid(1234);

    STUTTO_ASSERT(!engine.enable_relative_spike());
    STUTTO_ASSERT(!engine.enable_kernel_frame_stall());
    STUTTO_ASSERT(!engine.enable_dwm_glitch());

    // 1. Ingest DXGI frames: 10 warmup at 16.6ms, then 35ms spike (ratio > 2.0x, but < 50ms static threshold)
    uint64_t ts = 1000000ULL;
    for (int i = 0; i < 10; ++i) {
        engine.on_dxgi_present(1234, 5678, 16.6, ts);
        ts += 166000ULL;
    }
    bool dxgi_trig = engine.on_dxgi_present(1234, 5678, 35.0, ts);
    STUTTO_ASSERT(!dxgi_trig); // Suppressed by enable_relative_spike = false

    // 2. Ingest Kernel frame stall: must be suppressed immediately by enable_kernel_frame_stall = false
    bool kern_trig = engine.on_kernel_frame_stall(1234, 5678, 60.0, ts + 100000ULL, 0, 0);
    STUTTO_ASSERT(!kern_trig);

    // 3. Ingest DWM glitch: must be suppressed immediately by enable_dwm_glitch = false
    bool dwm_trig = engine.on_dwm_glitch(888, 999, 3, 50.0, ts + 200000ULL, 0);
    STUTTO_ASSERT(!dwm_trig);

    std::cout << "  -> Diagnostic toggles verification PASSED.\n";
}

static void test_trigger_engine_judder_filtered_stall_exclusion() {
    std::cout << "[TEST] Running test_trigger_engine_judder_filtered_stall_exclusion...\n";
    const uint64_t qpc_freq = 10000000ULL;
    TriggerConfig config{};
    config.present_threshold_ms = 16.67;
    config.min_report_severity = ReportSeverity::WARNING;

    TriggerEngine engine(config, qpc_freq);
    STUTTO_ASSERT(engine.filtered_reports() == 0);
    STUTTO_ASSERT(engine.filtered_stall_ms() == 0);

    // 1. Ingest relative spike filtered event (20.0ms) -> both count and stall time increment
    engine.record_filtered_event_for_test(
        TriggerSource::DXGI_PRESENT_STUTTER,
        TriggerReason::RELATIVE_SPIKE,
        20.0, 1234, 5678);
    STUTTO_ASSERT(engine.filtered_reports() == 1);
    STUTTO_ASSERT(engine.filtered_stall_ms() == 20);

    // 2. Ingest judder filtered event (2500.0ms) -> count increments, stall time DOES NOT increment
    engine.record_filtered_event_for_test(
        TriggerSource::FRAME_PACING_JUDDER,
        TriggerReason::CADENCE_JUDDER,
        2500.0, 1234, 5678);
    STUTTO_ASSERT(engine.filtered_reports() == 2);
    STUTTO_ASSERT(engine.filtered_stall_ms() == 20); // Remains 20ms, not 2520ms!

    std::cout << "  -> Judder filtered stall exclusion PASSED.\n";
}

int main() {
    try {
        test_filtered_event_layout();
        test_spsc_sanity();
        test_inflight_ticket_hold();
        test_lapped_cell_drain();
        test_mpsc_concurrency_stress();
        test_trigger_engine_preclaim_gate();
        test_diagnostic_toggles();
        test_trigger_engine_judder_filtered_stall_exclusion();
        std::cout << "[PASS] All test_report_filtering tests passed successfully!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] test_report_filtering: " << ex.what() << "\n";
        return 1;
    }
}
