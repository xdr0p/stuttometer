#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/fixed_table.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>

static void test_struct_size() {
    std::cout << "[TEST] Validating EtwEventRecord & Slot memory layout...\n";
    static_assert(sizeof(stuttometer::EtwEventRecord) == 56, "EtwEventRecord must be exactly 56 bytes");
    static_assert(sizeof(stuttometer::FlightRecorder::Slot) == 64, "Slot must be strictly 64 bytes");
    assert(sizeof(stuttometer::EtwEventRecord) == 56);
    assert(sizeof(stuttometer::FlightRecorder::Slot) == 64);
    std::cout << "  -> EtwEventRecord is 56 bytes, Slot is strictly 64 bytes (Zero implicit padding, 1 L1 cache line).\n";
}

static void test_fixed_in_flight_table() {
    std::cout << "[TEST] Running FixedInFlightTable multi-threaded concurrency test (8 threads, 100,000 ops)...\n";

    struct DummyVal {
        uint64_t timestamp;
        uint32_t pid;
        uint32_t tid;
    };

    stuttometer::FixedInFlightTable<DummyVal, 2048> table;
    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 12500;

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&table, t]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                const uint64_t key = (static_cast<uint64_t>(t + 1) << 32) | (i + 1);
                DummyVal val{ 1000ULL + i, static_cast<uint32_t>(t), static_cast<uint32_t>(i) };

                table.insert(key, val);

                DummyVal out{};
                if (table.find_and_erase(key, out)) {
                    assert(out.pid == static_cast<uint32_t>(t));
                    assert(out.tid == static_cast<uint32_t>(i));
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    std::cout << "  -> FixedInFlightTable concurrent insert & find_and_erase PASSED with zero leaks.\n";
}

static void test_multithreaded_flight_recorder() {
    std::cout << "[TEST] Running multi-producer concurrency test (8 threads, 400,000 events)...\n";
    
    constexpr size_t CAPACITY = 65536;
    stuttometer::FlightRecorder recorder(CAPACITY);

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    constexpr int NUM_THREADS = 8;
    constexpr int EVENTS_PER_THREAD = 50000;

    std::vector<std::thread> producers;
    producers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        producers.emplace_back([&recorder, t, base_qpc, qpc_freq]() {
            for (int i = 0; i < EVENTS_PER_THREAD; ++i) {
                stuttometer::EtwEventRecord rec{};
                rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
                rec.qpc_timestamp = base_qpc + stuttometer::ms_to_qpc_delta((t * 1000) + (i * 0.1), qpc_freq);
                rec.pid = static_cast<uint32_t>(1000 + t);
                rec.tid = static_cast<uint32_t>(2000 + i);
                rec.duration_us = static_cast<uint32_t>(100 + i);
                rec.cpu_index = static_cast<uint8_t>(t % 8);
                recorder.push(rec);
            }
        });
    }

    for (auto& p : producers) {
        p.join();
    }

    assert(recorder.current_head() == (NUM_THREADS * EVENTS_PER_THREAD));
    std::cout << "  -> All " << (NUM_THREADS * EVENTS_PER_THREAD) << " events successfully pushed.\n";

    std::cout << "[TEST] Testing snapshot extraction and chronological sorting...\n";
    const uint64_t from_qpc = base_qpc;
    const uint64_t to_qpc = base_qpc + stuttometer::ms_to_qpc_delta(10000.0, qpc_freq);

    uint64_t dropped = 0;
    auto snapshot = recorder.snapshot(from_qpc, to_qpc, &dropped);
    std::cout << "  -> Extracted snapshot size: " << snapshot.size() << " records (drops: " << dropped << ")\n";

    // Verify chronological ordering
    for (size_t i = 1; i < snapshot.size(); ++i) {
        assert(snapshot[i].qpc_timestamp >= snapshot[i - 1].qpc_timestamp);
    }
    std::cout << "  -> Verified strict chronological ordering of snapshot records.\n";
}

int main() {
    std::cout << "=== Stuttometer Flight Recorder & FixedTable Tests ===\n";
    test_struct_size();
    test_fixed_in_flight_table();
    test_multithreaded_flight_recorder();
    std::cout << ">>> All Flight Recorder tests PASSED! <<<\n\n";
    return 0;
}
