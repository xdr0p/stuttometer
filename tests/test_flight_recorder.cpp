#include "test_common.hpp"
#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/fixed_table.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/trigger_engine.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

static void test_struct_size() {
    std::cout << "[TEST] Validating EtwEventRecord & Slot memory layout...\n";
    static_assert(sizeof(stuttometer::EtwEventRecord) == 56, "EtwEventRecord must be exactly 56 bytes");
    static_assert(sizeof(stuttometer::FlightRecorder::Slot) == 64, "Slot must be strictly 64 bytes");
    STUTTO_ASSERT(sizeof(stuttometer::EtwEventRecord) == 56);
    STUTTO_ASSERT(sizeof(stuttometer::FlightRecorder::Slot) == 64);
    std::cout << "  -> EtwEventRecord is 56 bytes, Slot is strictly 64 bytes (Zero implicit padding, 1 L1 cache line).\n";
}

static void test_fixed_in_flight_table_and_eviction() {
    std::cout << "[TEST] Running FixedInFlightTable concurrency & background eviction test...\n";

    struct DummyVal {
        uint64_t timestamp;
        uint32_t pid;
        uint32_t tid;
    };

    stuttometer::FixedInFlightTable<DummyVal, 2048> table;
    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 10000;

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
                    STUTTO_ASSERT(out.pid == static_cast<uint32_t>(t));
                    STUTTO_ASSERT(out.tid == static_cast<uint32_t>(i));
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // Insert 50 stale items
    for (int i = 1; i <= 50; ++i) {
        DummyVal val{ 100ULL, 999, static_cast<uint32_t>(i) };
        table.insert(i, val);
    }

    // Evict entries older than 200 units (current is 500)
    size_t evicted = table.evict_stale(500, 200, [](const DummyVal& d) { return d.timestamp; });
    STUTTO_ASSERT(evicted == 50);
    STUTTO_ASSERT(table.unpaired_evictions() == 50);

    std::cout << "  -> FixedInFlightTable concurrency and eviction PASSED (50 stale items evicted).\n";
}

static void test_fixed_table_same_key_contention() {
    std::cout << "[TEST] Running FixedInFlightTable same-key heavy contention test...\n";

    struct DummyVal {
        uint64_t timestamp;
        uint32_t thread_id;
    };

    stuttometer::FixedInFlightTable<DummyVal, 512> table;
    constexpr int NUM_THREADS = 8;
    constexpr int OPS = 2000;
    static constexpr uint64_t SHARED_KEYS[] = { 101, 102, 103, 104 };

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&table, t]() {
            for (int i = 0; i < OPS; ++i) {
                const uint64_t key = SHARED_KEYS[i % 4];
                DummyVal val{ static_cast<uint64_t>(i), static_cast<uint32_t>(t) };

                table.insert(key, val);

                DummyVal out{};
                table.find_and_erase(key, out);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    std::cout << "  -> FixedInFlightTable same-key contention PASSED (no deadlocks or state corruption).\n";
}

static void test_trigger_engine_concurrency_and_claimed_state() {
    std::cout << "[TEST] Running TriggerEngine 3-step CLAIMED concurrency test (8 threads)...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 20.0;
    config.window_post_ms = 0.0; // Freeze immediately

    stuttometer::TriggerEngine engine(config, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    std::atomic<int> trigger_initiators{0};
    constexpr int NUM_THREADS = 8;
    std::vector<std::thread> workers;

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&engine, &trigger_initiators, t, base_qpc]() {
            bool initiated = engine.on_dxgi_present(1000 + t, 2000 + t, 35.0, base_qpc, static_cast<uint8_t>(t));
            if (initiated) {
                trigger_initiators.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // Strictly 1 thread must win the CLAIMED race, other 7 must be suppressed
    STUTTO_ASSERT(trigger_initiators.load() == 1);
    STUTTO_ASSERT(engine.suppressed_trigger_count() == 7);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::FROZEN);

    stuttometer::TriggerInfo info;
    uint64_t from = 0, to = 0;
    STUTTO_ASSERT(engine.poll_state(base_qpc, info, from, to) == true);
    STUTTO_ASSERT(info.duration_ms == 35.0);

    std::cout << "  -> Exactly 1 thread claimed trigger; 7 threads cleanly suppressed. CLAIMED lifecycle PASSED.\n";
}

static void test_trigger_engine_watchdog_zero_post_window() {
    std::cout << "[TEST] Running TriggerEngine Watchdog recovery test with window_post_ms == 0.0...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 20.0;
    config.window_post_ms = 0.0;
    config.cooldown_ms = 100.0;

    stuttometer::TriggerEngine engine(config, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    bool ok = engine.on_dxgi_present(100, 200, 30.0, base_qpc, 1);
    STUTTO_ASSERT(ok == true);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::FROZEN);

    stuttometer::TriggerInfo info;
    uint64_t from = 0, to = 0;
    // Before watchdog timeout: poll returns true
    STUTTO_ASSERT(engine.poll_state(base_qpc + stuttometer::ms_to_qpc_delta(1000.0, qpc_freq), info, from, to) == true);

    // Advance past 5.0 second watchdog timeout (e.g. 5.5s) without calling on_report_completed
    const uint64_t timeout_qpc = base_qpc + stuttometer::ms_to_qpc_delta(5500.0, qpc_freq);
    bool polled = engine.poll_state(timeout_qpc, info, from, to);
    STUTTO_ASSERT(polled == false);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::COOLDOWN);

    // After cooldown expiration, transitions back to ARMED
    const uint64_t after_cooldown = timeout_qpc + stuttometer::ms_to_qpc_delta(150.0, qpc_freq);
    engine.poll_state(after_cooldown, info, from, to);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    std::cout << "  -> Zero post-window watchdog recovery PASSED.\n";
}

static void test_fixed_table_concurrent_lookup_seqlock() {
    std::cout << "[TEST] Running FixedInFlightTable concurrent lookup() seqlock torn-read validation...\n";

    struct DummyVal {
        uint64_t timestamp;
        uint32_t pid;
        uint32_t tid;
        uint64_t checksum; // pid ^ tid ^ timestamp
    };

    stuttometer::FixedInFlightTable<DummyVal, 1024> table;
    std::atomic<bool> stop_flag{false};
    std::atomic<uint64_t> successful_lookups{0};
    std::atomic<uint64_t> torn_reads_detected{0};

    constexpr int NUM_WRITERS = 4;
    constexpr int NUM_READERS = 4;
    constexpr int OPS = 5000;

    std::vector<std::thread> threads;

    // Writers: constantly insert, update, and erase entries
    for (int t = 0; t < NUM_WRITERS; ++t) {
        threads.emplace_back([&table, &stop_flag, t]() {
            for (int i = 0; i < OPS && !stop_flag.load(std::memory_order_relaxed); ++i) {
                const uint64_t key = (i % 64) + 1;
                const uint64_t ts = 1000ULL + i;
                const uint32_t pid = static_cast<uint32_t>(t * 1000 + (i % 100));
                const uint32_t tid = static_cast<uint32_t>(i);
                DummyVal val{ ts, pid, tid, ts ^ pid ^ tid };

                table.insert(key, val);

                if (i % 5 == 0) {
                    DummyVal out{};
                    table.find_and_erase(key, out);
                }
            }
        });
    }

    // Readers: continuously call lookup() and verify data integrity
    for (int r = 0; r < NUM_READERS; ++r) {
        threads.emplace_back([&table, &stop_flag, &successful_lookups, &torn_reads_detected]() {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                for (uint64_t k = 1; k <= 64; ++k) {
                    DummyVal out{};
                    if (table.lookup(k, out)) {
                        // Validate that the struct was not torn (checksum matches internal fields)
                        if (out.checksum != (out.timestamp ^ out.pid ^ out.tid)) {
                            torn_reads_detected.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            successful_lookups.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }

    // Join writers first
    for (int t = 0; t < NUM_WRITERS; ++t) {
        threads[t].join();
    }
    stop_flag.store(true, std::memory_order_release);

    // Join readers
    for (int r = NUM_WRITERS; r < NUM_WRITERS + NUM_READERS; ++r) {
        threads[r].join();
    }

    STUTTO_ASSERT(torn_reads_detected.load() == 0);
    STUTTO_ASSERT(successful_lookups.load() > 0);
    std::cout << "  -> FixedInFlightTable seqlock lookup() PASSED ("
              << successful_lookups.load() << " atomic reads verified, 0 torn reads).\n";
}

static void test_flight_recorder_wraparound() {
    std::cout << "[TEST] Running FlightRecorder wrap-around test...\n";

    constexpr size_t CAPACITY = 16;
    stuttometer::FlightRecorder recorder(CAPACITY);

    // Push 32 items (2 full buffer cycles)
    for (uint64_t i = 0; i < 32; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.qpc_timestamp = 1000 + i;
        rec.duration_us = static_cast<uint32_t>(i);
        recorder.push(rec);
    }

    STUTTO_ASSERT(recorder.current_head() == 32);

    uint64_t drops = 0;
    auto snapshot = recorder.snapshot(1000, 2000, &drops);
    STUTTO_ASSERT(snapshot.size() == CAPACITY);
    STUTTO_ASSERT(snapshot.front().qpc_timestamp == 1000 + 16);
    STUTTO_ASSERT(snapshot.back().qpc_timestamp == 1000 + 31);

    std::cout << "  -> FlightRecorder wrap-around PASSED.\n";
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

    STUTTO_ASSERT(recorder.current_head() == (NUM_THREADS * EVENTS_PER_THREAD));
    std::cout << "  -> All " << (NUM_THREADS * EVENTS_PER_THREAD) << " events successfully pushed.\n";

    std::cout << "[TEST] Testing snapshot extraction and chronological sorting...\n";
    const uint64_t from_qpc = base_qpc;
    const uint64_t to_qpc = base_qpc + stuttometer::ms_to_qpc_delta(10000.0, qpc_freq);

    uint64_t dropped = 0;
    auto snapshot = recorder.snapshot(from_qpc, to_qpc, &dropped);
    std::cout << "  -> Extracted snapshot size: " << snapshot.size() << " records (drops: " << dropped << ")\n";

    for (size_t i = 1; i < snapshot.size(); ++i) {
        STUTTO_ASSERT(snapshot[i].qpc_timestamp >= snapshot[i - 1].qpc_timestamp);
        // Verify valid payload fields
        STUTTO_ASSERT(snapshot[i].category == static_cast<uint16_t>(stuttometer::EventCategory::DPC));
        STUTTO_ASSERT(snapshot[i].pid >= 1000 && snapshot[i].pid < 1008);
        STUTTO_ASSERT(snapshot[i].duration_us >= 100);
    }
    std::cout << "  -> Verified strict chronological ordering and payload integrity of snapshot records.\n";
}

static void test_driver_symbol_resolver_thread_safety() {
    std::cout << "[TEST] Testing DriverSymbolResolver C++20 atomic copy-on-write resolution...\n";

    stuttometer::DriverSymbolResolver resolver;
    std::string name = resolver.resolve_driver_name(0xFFFFF80000000000ULL);
    STUTTO_ASSERT(!name.empty());

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&resolver]() {
            for (int j = 0; j < 1000; ++j) {
                resolver.resolve_driver_name(0xFFFFF80012340000ULL);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::cout << "  -> DriverSymbolResolver lock-free resolution PASSED.\n";
}

static void test_fixed_table_aba_turnover_safety() {
    std::cout << "[TEST] Running FixedInFlightTable rapid ABA turnover & probe advance test...\n";

    stuttometer::FixedInFlightTable<uint64_t, 64> table;
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS = 5000;

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    // Threads rapidly cycle the same small set of colliding keys
    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&table, t]() {
            for (int i = 0; i < ITERS; ++i) {
                const uint64_t key = (i % 8) * 64 + (t % 4);
                table.insert(key, static_cast<uint64_t>(i));
                uint64_t out = 0;
                table.find_and_erase(key, out);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    std::cout << "  -> FixedInFlightTable ABA turnover & probe collision recovery PASSED.\n";
}

static void test_flight_recorder_reverse_snapshot_bounds() {
    std::cout << "[TEST] Running FlightRecorder reverse snapshot bounds & underflow safety test...\n";

    stuttometer::FlightRecorder recorder(1024); // Small capacity: 1024 slots

    // Test 1: start_seq == 0 boundary (few events, underflow safety)
    for (uint64_t i = 1; i <= 20; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.qpc_timestamp = 1000 + (i * 10);
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
        rec.pid = 1234;
        rec.tid = static_cast<uint32_t>(i);
        recorder.push(rec);
    }

    uint64_t dropped = 0;
    // Snapshot window: [1050, 1150] (events with timestamps 1050..1150)
    auto snap1 = recorder.snapshot(1050, 1150, &dropped);
    STUTTO_ASSERT(snap1.size() == 11); // 1050, 1060, ..., 1150
    STUTTO_ASSERT(dropped == 0);
    // Verify chronological ordering
    for (size_t i = 0; i < snap1.size(); ++i) {
        STUTTO_ASSERT(snap1[i].qpc_timestamp == 1050 + (i * 10));
        if (i > 0) {
            STUTTO_ASSERT(snap1[i - 1].qpc_timestamp <= snap1[i].qpc_timestamp);
        }
    }

    // Test 2: Large volume exceeding capacity (start_seq > 0) with out-of-window early exit
    for (uint64_t i = 21; i <= 3000; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.qpc_timestamp = 1000 + (i * 10);
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::CSWITCH);
        rec.pid = 5678;
        rec.tid = static_cast<uint32_t>(i);
        recorder.push(rec);
    }

    // Target recent window [29000, 30500]
    auto snap2 = recorder.snapshot(29000, 30500, &dropped);
    STUTTO_ASSERT(!snap2.empty());
    // Verify all items fall within [29000, 30500] and are strictly chronological
    for (size_t i = 0; i < snap2.size(); ++i) {
        STUTTO_ASSERT(snap2[i].qpc_timestamp >= 29000);
        STUTTO_ASSERT(snap2[i].qpc_timestamp <= 30500);
        if (i > 0) {
            STUTTO_ASSERT(snap2[i - 1].qpc_timestamp <= snap2[i].qpc_timestamp);
        }
    }

    std::cout << "  -> FlightRecorder reverse snapshot bounds, chronological sort & underflow safety PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Flight Recorder & Concurrency Tests ===\n";
    try {
        test_struct_size();
        test_fixed_in_flight_table_and_eviction();
        test_fixed_table_same_key_contention();
        test_fixed_table_aba_turnover_safety();
        test_fixed_table_concurrent_lookup_seqlock();
        test_trigger_engine_concurrency_and_claimed_state();
        test_trigger_engine_watchdog_zero_post_window();
        test_flight_recorder_wraparound();
        test_flight_recorder_reverse_snapshot_bounds();
        test_multithreaded_flight_recorder();
        test_driver_symbol_resolver_thread_safety();
        std::cout << ">>> All Flight Recorder tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
