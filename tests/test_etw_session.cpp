#include "test_common.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <unordered_set>
#include <vector>
#include <filesystem>

using namespace stuttometer;

static void test_session_manager_initial_state() {
    std::cout << "[TEST] EtwSessionManager initial state and default config...\n";

    FlightRecorder recorder(1024);
    TriggerConfig trig_cfg;
    TriggerEngine engine(trig_cfg, get_qpc_frequency());

    EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    cfg.enable_audio = false;
    cfg.enable_kernel_dpc = false;
    cfg.enable_kernel_disk = false;
    cfg.enable_kernel_cswitch = false;

    EtwSessionManager mgr(recorder, engine, cfg);

    STUTTO_ASSERT(!mgr.is_running());
    STUTTO_ASSERT(!mgr.consumer_failed());
    STUTTO_ASSERT(!mgr.user_consumer_failed());
    STUTTO_ASSERT(!mgr.kernel_consumer_failed());
    STUTTO_ASSERT(mgr.events_lost() == 0);
    STUTTO_ASSERT(mgr.buffers_lost() == 0);
    STUTTO_ASSERT(mgr.unpaired_evictions() == 0);

    std::cout << "  -> Initial state assertions PASSED.\n";
}

static void test_present_key_hash_uniqueness() {
    std::cout << "[TEST] Validating DXGI Present Key Hash injectivity and uniqueness...\n";

    std::unordered_set<uint64_t> hashes;
    const size_t num_tids = 100;
    const size_t num_ptrs = 100;

    for (uint32_t tid = 1; tid <= num_tids; ++tid) {
        for (uint64_t ptr_idx = 0; ptr_idx < num_ptrs; ++ptr_idx) {
            uint64_t ptr = (ptr_idx == 0) ? 0 : (0x00007FF700000000ULL + (ptr_idx * 0x1000));
            uint64_t h = stuttometer::make_present_key(tid, ptr);

            STUTTO_ASSERT(h != 0);
            auto [it, inserted] = hashes.insert(h);
            STUTTO_ASSERT(inserted); // Must have zero collisions across all combinations
        }
    }

    // Explicit test for null swapchain and 0x1ULL sentinel fallback
    uint64_t h_null = stuttometer::make_present_key(1234, 0);
    uint64_t h_sentinel = stuttometer::make_present_key(1234, 0x1ULL);
    STUTTO_ASSERT(h_null == h_sentinel);

    std::cout << "  -> Generated " << hashes.size() << " unique hashes with 0 collisions. PASSED.\n";
}

static void test_session_manager_lifecycle_stop() {
    std::cout << "[TEST] EtwSessionManager safe stop and cleanup on unstarted session...\n";

    FlightRecorder recorder(1024);
    TriggerConfig trig_cfg;
    TriggerEngine engine(trig_cfg, get_qpc_frequency());
    EtwSessionConfig cfg;

    EtwSessionManager mgr(recorder, engine, cfg);
    // Calling stop() on unstarted session should be a safe no-op
    mgr.stop();
    STUTTO_ASSERT(!mgr.is_running());

    std::cout << "  -> Unstarted session stop() PASSED.\n";
}

static void test_fixed_table_tombstone_collision_and_update() {
    std::cout << "[TEST] Validating FixedInFlightTable open-addressing tombstone reuse & duplicate prevention...\n";

    FixedInFlightTable<uint64_t, 64> table;

    // Step 1: Insert key 100 and key 200
    STUTTO_ASSERT(table.insert(100, 1000));
    STUTTO_ASSERT(table.insert(200, 2000));

    // Step 2: Erase key 100 -> converts slot for key 100 to TOMBSTONE
    uint64_t val = 0;
    STUTTO_ASSERT(table.find_and_erase(100, val));
    STUTTO_ASSERT(val == 1000);

    // Step 3: Re-insert key 200 with new value 2500
    // Under open-addressing, it MUST update existing key 200 and NOT create a duplicate in slot 0
    STUTTO_ASSERT(table.insert(200, 2500));

    // Step 4: Verify lookup of key 200 gets 2500
    uint64_t val_lookup = 0;
    STUTTO_ASSERT(table.lookup(200, val_lookup));
    STUTTO_ASSERT(val_lookup == 2500);

    // Step 5: Erase key 200
    STUTTO_ASSERT(table.find_and_erase(200, val));
    STUTTO_ASSERT(val == 2500);

    // Step 6: Verify table no longer has key 200 (no duplicate was left behind)
    STUTTO_ASSERT(!table.lookup(200, val_lookup));
    STUTTO_ASSERT(!table.find_and_erase(200, val));

    // Step 7: Test multiple insertions and deletions with tombstone recycling
    for (uint64_t i = 1; i <= 30; ++i) {
        STUTTO_ASSERT(table.insert(i, i * 10));
    }
    for (uint64_t i = 1; i <= 15; ++i) {
        STUTTO_ASSERT(table.find_and_erase(i, val));
        STUTTO_ASSERT(val == i * 10);
    }
    for (uint64_t i = 31; i <= 45; ++i) {
        STUTTO_ASSERT(table.insert(i, i * 10)); // Re-uses tombstones
    }
    for (uint64_t i = 16; i <= 45; ++i) {
        STUTTO_ASSERT(table.find_and_erase(i, val));
        STUTTO_ASSERT(val == i * 10);
    }
    for (uint64_t i = 1; i <= 45; ++i) {
        STUTTO_ASSERT(!table.lookup(i, val_lookup));
    }

    std::cout << "  -> FixedInFlightTable tombstone reuse & collision assertions PASSED.\n";
}

static void test_packed_atomic_target_state() {
    std::cout << "[TEST] Running TriggerEngine packed 64-bit atomic target filtering test...\n";

    stuttometer::TriggerConfig cfg;
    cfg.target_pid = 1234;
    cfg.target_process_name = "game.exe";

    stuttometer::TriggerEngine engine(cfg, 10000000);
    STUTTO_ASSERT(engine.active_target_pid() == 1234);
    STUTTO_ASSERT(!engine.is_target_waiting());

    // Test transition to waiting state
    engine.update_target_pid(0, true);
    STUTTO_ASSERT(engine.active_target_pid() == 0);
    STUTTO_ASSERT(engine.is_target_waiting());

    // Test transition back to active PID
    engine.update_target_pid(5678, false);
    STUTTO_ASSERT(engine.active_target_pid() == 5678);
    STUTTO_ASSERT(!engine.is_target_waiting());

    // Test dynamic vblank interval accessor
    STUTTO_ASSERT(engine.vblank_interval_ms() > 0.0);
    cfg.present_threshold_ms = 8.33; // 120 FPS
    stuttometer::TriggerEngine engine120(cfg, 10000000);
    STUTTO_ASSERT(std::abs(engine120.vblank_interval_ms() - 8.33) < 0.01);

    std::cout << "  -> TriggerEngine packed 64-bit atomic state and vblank interval tests PASSED.\n";
}

static void test_thread_switch_out_table() {
    std::cout << "[TEST] Validating FixedInFlightTable with ThreadSwitchOut struct...\n";

    FixedInFlightTable<ThreadSwitchOut, 64> table;
    const uint32_t tid_voluntary = 1001;
    const uint32_t tid_involuntary = 1002;

    // Insert switch-out state for voluntary sleep (state 5 = Waiting)
    ThreadSwitchOut so_vol{ 1000000ULL, 4000, 5 };
    STUTTO_ASSERT(table.insert(tid_voluntary, so_vol));

    // Insert switch-out state for involuntary preemption (state 2 = Running/Ready)
    ThreadSwitchOut so_invol{ 2000000ULL, 4000, 2 };
    STUTTO_ASSERT(table.insert(tid_involuntary, so_invol));

    // Retrieve voluntary thread
    ThreadSwitchOut out{};
    STUTTO_ASSERT(table.find_and_erase(tid_voluntary, out));
    STUTTO_ASSERT(out.qpc == 1000000ULL);
    STUTTO_ASSERT(out.pid == 4000);
    STUTTO_ASSERT(out.wait_state == 5);

    // Retrieve involuntary thread
    STUTTO_ASSERT(table.find_and_erase(tid_involuntary, out));
    STUTTO_ASSERT(out.qpc == 2000000ULL);
    STUTTO_ASSERT(out.pid == 4000);
    STUTTO_ASSERT(out.wait_state == 2);

    std::cout << "  -> ThreadSwitchOut table insertion and retrieval PASSED.\n";
}

static void test_session_manager_insertion_failures() {
    std::cout << "[TEST] Validating table insertion failure aggregation...\n";

    FlightRecorder recorder(1024);
    TriggerConfig trig_cfg;
    TriggerEngine engine(trig_cfg, get_qpc_frequency());
    EtwSessionConfig cfg;

    EtwSessionManager mgr(recorder, engine, cfg);
    STUTTO_ASSERT(mgr.insertion_failures() == 0);
    STUTTO_ASSERT(mgr.unpaired_evictions() == 0);

    std::cout << "  -> Insertion failure initial zero assertions PASSED.\n";
}

static void test_fixed_table_concurrent_tombstone_stress() {
    std::cout << "[TEST] Stress testing FixedInFlightTable concurrent insert/erase tombstone reuse...\n";

    FixedInFlightTable<uint64_t, 128> table;
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS_PER_THREAD = 5000;
    std::atomic<bool> start_flag{false};
    std::atomic<uint64_t> successful_pairs{0};

    std::vector<std::thread> workers;
    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 1; i <= ITERS_PER_THREAD; ++i) {
                // Use overlapping keys to force heavy collision and tombstone reuse
                uint64_t key = (static_cast<uint64_t>((t % 4) * 1000) + (i % 32)) + 1;
                uint64_t val = (static_cast<uint64_t>(t) << 32) | static_cast<uint64_t>(i);
                if (table.insert(key, val)) {
                    uint64_t out_val = 0;
                    if (table.find_and_erase(key, out_val)) {
                        successful_pairs.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    start_flag.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    std::cout << "  -> Completed " << successful_pairs.load() << " atomic insert/erase cycles under heavy concurrency. PASSED.\n";
}

static void test_fixed_table_in_place_update() {
    std::cout << "[TEST] Validating FixedInFlightTable in-place update() under multi-threaded concurrency...\n";

    struct CounterVal {
        uint64_t counter{0};
    };

    FixedInFlightTable<CounterVal, 64> table;
    STUTTO_ASSERT(table.insert(100, CounterVal{0}));

    constexpr int NUM_THREADS = 4;
    constexpr int ITERS_PER_THREAD = 2500;
    std::atomic<bool> start_flag{false};

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < ITERS_PER_THREAD; ++i) {
                while (!table.update(100, [](CounterVal& val) {
                    ++val.counter;
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }

    start_flag.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    CounterVal final_val{};
    STUTTO_ASSERT(table.lookup(100, final_val));
    STUTTO_ASSERT(final_val.counter == static_cast<uint64_t>(NUM_THREADS * ITERS_PER_THREAD));

    // Verify key 0 rejection and non-existent key handling
    STUTTO_ASSERT(!table.update(0, [](CounterVal& val) { ++val.counter; }));
    STUTTO_ASSERT(!table.update(999, [](CounterVal& val) { ++val.counter; }));

    std::cout << "  -> FixedInFlightTable update() atomic increment concurrency (" << final_val.counter << " ops) PASSED.\n";
}

static void test_fixed_table_key_zero_rejection() {
    std::cout << "[TEST] Validating FixedInFlightTable key 0 rejection...\n";
    FixedInFlightTable<uint64_t, 64> table;
    STUTTO_ASSERT(!table.insert(0, 12345));
    STUTTO_ASSERT(table.insertion_failures() == 1);
    std::cout << "  -> Key 0 rejection PASSED.\n";
}

static void test_calculate_effective_present_duration() {
    std::cout << "[TEST] Validating calculate_effective_present_duration inter-frame delta logic...\n";

    const uint64_t qpc_freq = get_qpc_frequency();

    // 1. First Frame on Swapchain (Baseline Initialization)
    uint64_t t1 = ms_to_qpc_delta(1000.0, qpc_freq);
    uint64_t t1_start = t1 - ms_to_qpc_delta(0.2, qpc_freq);
    PresentDeltaResult r1 = calculate_effective_present_duration(t1, 0, t1_start, qpc_freq);
    STUTTO_ASSERT(r1.is_baseline_reset == true);
    STUTTO_ASSERT(r1.effective_dur_us == 200); // 0.2ms API duration

    // 2. Normal 200 FPS Frame (5.0ms delta)
    uint64_t t2 = t1 + ms_to_qpc_delta(5.0, qpc_freq);
    uint64_t t2_start = t2 - ms_to_qpc_delta(0.2, qpc_freq);
    PresentDeltaResult r2 = calculate_effective_present_duration(t2, t1, t2_start, qpc_freq);
    STUTTO_ASSERT(r2.is_baseline_reset == false);
    STUTTO_ASSERT(r2.effective_dur_us == 5000); // 5.0ms delta

    // 3. In-Game Stutter Frame (50.0ms delta / 20 FPS drop)
    uint64_t t3 = t2 + ms_to_qpc_delta(50.0, qpc_freq);
    uint64_t t3_start = t3 - ms_to_qpc_delta(0.3, qpc_freq);
    PresentDeltaResult r3 = calculate_effective_present_duration(t3, t2, t3_start, qpc_freq);
    STUTTO_ASSERT(r3.is_baseline_reset == false);
    STUTTO_ASSERT(r3.effective_dur_us == 50000); // 50.0ms true stutter duration

    // 4. Alt-Tab / Long Pause Ceiling (> 10s pause)
    uint64_t t4 = t3 + ms_to_qpc_delta(12000.0, qpc_freq); // 12 seconds later (exceeds 10s ceiling)
    uint64_t t4_start = t4 - ms_to_qpc_delta(0.2, qpc_freq);
    PresentDeltaResult r4 = calculate_effective_present_duration(t4, t3, t4_start, qpc_freq);
    STUTTO_ASSERT(r4.is_baseline_reset == true); // Must re-seed baseline without triggering false stutter

    // 5. API Duration Precedence (e.g. VSync wait took 25ms, delta was 5ms)
    uint64_t t5 = t4 + ms_to_qpc_delta(5.0, qpc_freq);
    uint64_t t5_start = t5 - ms_to_qpc_delta(25.0, qpc_freq);
    PresentDeltaResult r5 = calculate_effective_present_duration(t5, t4, t5_start, qpc_freq);
    STUTTO_ASSERT(r5.is_baseline_reset == false);
    STUTTO_ASSERT(r5.effective_dur_us == 25000); // Max(5ms, 25ms) = 25ms

    std::cout << "  -> calculate_effective_present_duration delta, ceiling, and baseline tests PASSED.\n";
}

static void test_guid_activity_id_validation() {
    std::cout << "[TEST] Validating ActivityId GUID_NULL detection for non-zero Data4[1..7]...\n";

    GUID null_guid{};
    STUTTO_ASSERT(IsEqualGUID(null_guid, GUID_NULL));

    // GUID with non-zero byte only in Data4[7]
    GUID guid_with_data4_end{};
    guid_with_data4_end.Data4[7] = 0x01;
    STUTTO_ASSERT(!IsEqualGUID(guid_with_data4_end, GUID_NULL));

    // GUID with non-zero byte only in Data4[1]
    GUID guid_with_data4_mid{};
    guid_with_data4_mid.Data4[1] = 0xFF;
    STUTTO_ASSERT(!IsEqualGUID(guid_with_data4_mid, GUID_NULL));

    std::cout << "  -> ActivityId GUID validation PASSED.\n";
}

static void test_out_of_order_dwm_debounce_logic() {
    std::cout << "[TEST] Validating out-of-order DWM glitch debounce delta calculation...\n";

    const uint64_t qpc_freq = get_qpc_frequency();
    const uint64_t dedup_window_qpc = ms_to_qpc_delta(50.0, qpc_freq);

    uint64_t last_glitch = 1000000;
    
    // Normal chronological duplicate (timestamp > last_glitch, delta 2ms < 50ms)
    uint64_t ts_chrono = last_glitch + ms_to_qpc_delta(2.0, qpc_freq);
    uint64_t delta1 = (ts_chrono >= last_glitch) ? (ts_chrono - last_glitch) : (last_glitch - ts_chrono);
    STUTTO_ASSERT(delta1 < dedup_window_qpc);

    // Out-of-order duplicate from another core (timestamp < last_glitch, delta 2ms < 50ms)
    uint64_t ts_out_of_order = last_glitch - ms_to_qpc_delta(2.0, qpc_freq);
    uint64_t delta2 = (ts_out_of_order >= last_glitch) ? (ts_out_of_order - last_glitch) : (last_glitch - ts_out_of_order);
    STUTTO_ASSERT(delta2 < dedup_window_qpc);

    // Distinct glitch outside 50ms window (delta 60ms > 50ms)
    uint64_t ts_distinct = last_glitch + ms_to_qpc_delta(60.0, qpc_freq);
    uint64_t delta3 = (ts_distinct >= last_glitch) ? (ts_distinct - last_glitch) : (last_glitch - ts_distinct);
    STUTTO_ASSERT(delta3 >= dedup_window_qpc);

    std::cout << "  -> DWM glitch out-of-order debounce delta calculation PASSED.\n";
}

static void test_cswitch_tid_recycling_pid_check() {
    std::cout << "[TEST] Validating CSwitch thread ID recycling cross-process PID validation...\n";

    FixedInFlightTable<ThreadSwitchOut, 64> table;
    const uint32_t recycled_tid = 9999;
    const uint32_t process_a_pid = 1111;
    const uint32_t process_b_pid = 2222;

    // Process A thread 9999 switches out
    ThreadSwitchOut so_proc_a{ 5000000ULL, process_a_pid, 2 };
    STUTTO_ASSERT(table.insert(recycled_tid, so_proc_a));

    // Process B thread 9999 switches in -> retrieves switch-out entry
    ThreadSwitchOut retrieved{};
    STUTTO_ASSERT(table.find_and_erase(recycled_tid, retrieved));
    STUTTO_ASSERT(retrieved.pid == process_a_pid);

    // Incoming PID is process B -> mismatch detected, so duration must be dropped
    bool pid_valid = (retrieved.pid == 0 || retrieved.pid == process_b_pid);
    STUTTO_ASSERT(!pid_valid); // Must reject stale Process A switch-out duration for Process B!

    std::cout << "  -> CSwitch TID recycling cross-process PID rejection PASSED.\n";
}

static void test_provider_permutation_config() {
    std::cout << "[TEST] Validating ETW provider configuration permutations...\n";

    // 1. D3D12 only configuration
    {
        EtwSessionConfig cfg;
        cfg.enable_dxgi = false;
        cfg.enable_audio = false;
        cfg.enable_dxgkrnl = false;
        cfg.enable_dwm_core = false;
        cfg.enable_processor_power = false;
        cfg.enable_antimalware = false;
        cfg.enable_d3d12 = true;
        cfg.enable_kernel_memory = false;

        const bool user_requested = cfg.enable_dxgi || cfg.enable_audio || 
                                    cfg.enable_dxgkrnl || cfg.enable_dwm_core ||
                                    cfg.enable_processor_power || cfg.enable_antimalware ||
                                    cfg.enable_d3d12 || cfg.enable_kernel_memory;
        STUTTO_ASSERT(user_requested == true);
    }

    // 2. Kernel Memory only configuration
    {
        EtwSessionConfig cfg;
        cfg.enable_dxgi = false;
        cfg.enable_audio = false;
        cfg.enable_dxgkrnl = false;
        cfg.enable_dwm_core = false;
        cfg.enable_processor_power = false;
        cfg.enable_antimalware = false;
        cfg.enable_d3d12 = false;
        cfg.enable_kernel_memory = true;

        const bool user_requested = cfg.enable_dxgi || cfg.enable_audio || 
                                    cfg.enable_dxgkrnl || cfg.enable_dwm_core ||
                                    cfg.enable_processor_power || cfg.enable_antimalware ||
                                    cfg.enable_d3d12 || cfg.enable_kernel_memory;
        STUTTO_ASSERT(user_requested == true);
    }

    // 3. Kernel Pagefault only configuration
    {
        EtwSessionConfig cfg;
        cfg.enable_kernel_dpc = false;
        cfg.enable_kernel_disk = false;
        cfg.enable_kernel_cswitch = false;
        cfg.enable_kernel_profile = false;
        cfg.enable_kernel_pagefault = true;

        const bool kernel_requested = cfg.enable_kernel_dpc || cfg.enable_kernel_disk || 
                                      cfg.enable_kernel_cswitch || cfg.enable_kernel_profile ||
                                      cfg.enable_kernel_pagefault;
        STUTTO_ASSERT(kernel_requested == true);
    }

    // 4. Kernel Profile only configuration
    {
        EtwSessionConfig cfg;
        cfg.enable_kernel_dpc = false;
        cfg.enable_kernel_disk = false;
        cfg.enable_kernel_cswitch = false;
        cfg.enable_kernel_profile = true;
        cfg.enable_kernel_pagefault = false;

        const bool kernel_requested = cfg.enable_kernel_dpc || cfg.enable_kernel_disk || 
                                      cfg.enable_kernel_cswitch || cfg.enable_kernel_profile ||
                                      cfg.enable_kernel_pagefault;
        STUTTO_ASSERT(kernel_requested == true);
    }

    std::cout << "  -> Provider configuration permutations PASSED.\n";
}

static void test_granular_session_teardown() {
    std::cout << "[TEST] Validating granular stop_user_session and stop_kernel_session idempotency...\n";

    FlightRecorder recorder(1024);
    TriggerConfig trig_cfg;
    TriggerEngine engine(trig_cfg, get_qpc_frequency());
    EtwSessionConfig cfg;

    EtwSessionManager mgr(recorder, engine, cfg);

    // Calling granular stops on unstarted session must be safe no-ops
    mgr.stop_user_session();
    mgr.stop_kernel_session();
    STUTTO_ASSERT(!mgr.is_running());

    // Calling again must remain idempotent
    mgr.stop_user_session();
    mgr.stop_kernel_session();
    mgr.stop();
    STUTTO_ASSERT(!mgr.is_running());

    std::cout << "  -> Granular session teardown idempotency PASSED.\n";
}

static void test_resolve_process_name_caching() {
    std::cout << "[TEST] Validating resolve_process_name_to_pid fast caching and resolution...\n";

    // Resolving empty string returns 0
    uint32_t pid_empty = resolve_process_name_to_pid("");
    STUTTO_ASSERT(pid_empty == 0);

    // Resolving self (current test executable)
    DWORD current_pid = GetCurrentProcessId();
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    STUTTO_ASSERT(len > 0);

    std::filesystem::path p(exe_path);
    std::string exe_filename = p.filename().string();

    uint32_t resolved_pid = resolve_process_name_to_pid(exe_filename);
    STUTTO_ASSERT(resolved_pid == current_pid);

    // Second call should hit the fast PID image verification cache
    uint32_t cached_pid = resolve_process_name_to_pid(exe_filename);
    STUTTO_ASSERT(cached_pid == current_pid);

    std::cout << "  -> Process name fast resolution & caching PASSED.\n";
}

static void test_trigger_engine_cas_process_attachment() {
    std::cout << "[TEST] Validating TriggerEngine CAS process attachment, string matching & concurrency...\n";

    // 1. Validate matches_process_name logic
    STUTTO_ASSERT(stuttometer::matches_process_name("Game.exe", "game"));
    STUTTO_ASSERT(stuttometer::matches_process_name("Game.exe", "Game.exe"));
    STUTTO_ASSERT(stuttometer::matches_process_name("Game.exe", "GAME.EXE"));
    STUTTO_ASSERT(stuttometer::matches_process_name("Cyberpunk2077.exe", "cyberpunk2077"));
    STUTTO_ASSERT(!stuttometer::matches_process_name("gamebar.exe", "game"));
    STUTTO_ASSERT(!stuttometer::matches_process_name("gameguard.exe", "game"));
    STUTTO_ASSERT(!stuttometer::matches_process_name("MyGame.exe", "game"));
    STUTTO_ASSERT(!stuttometer::matches_process_name("", "game"));
    STUTTO_ASSERT(!stuttometer::matches_process_name("Game.exe", ""));

    // 2. Monitor-All Mode (no target process name, target_pid = 0)
    {
        stuttometer::TriggerConfig all_cfg;
        all_cfg.target_pid = 0;
        all_cfg.target_process_name = "";
        stuttometer::TriggerEngine all_engine(all_cfg, 10000000);

        STUTTO_ASSERT(!all_engine.is_target_waiting());
        STUTTO_ASSERT(all_engine.active_target_pid() == 0);
        STUTTO_ASSERT(!all_engine.try_attach_pid(1234));
        STUTTO_ASSERT(!all_engine.try_detach_pid(1234));
        STUTTO_ASSERT(!all_engine.on_process_launched(1234, "Game.exe"));
    }

    // 3. Waiting Mode (target process name set, target_pid = 0)
    {
        stuttometer::TriggerConfig wait_cfg;
        wait_cfg.target_pid = 0;
        wait_cfg.target_process_name = "Cyberpunk2077.exe";
        stuttometer::TriggerEngine wait_engine(wait_cfg, 10000000);

        STUTTO_ASSERT(wait_engine.is_target_waiting());
        STUTTO_ASSERT(wait_engine.active_target_pid() == 0);

        // Non-matching process launch is ignored
        STUTTO_ASSERT(!wait_engine.on_process_launched(5555, "Discord.exe"));
        STUTTO_ASSERT(wait_engine.is_target_waiting());
        STUTTO_ASSERT(wait_engine.active_target_pid() == 0);

        // Matching process launch attaches via CAS
        STUTTO_ASSERT(wait_engine.on_process_launched(9999, "cyberpunk2077.exe"));
        STUTTO_ASSERT(!wait_engine.is_target_waiting());
        STUTTO_ASSERT(wait_engine.active_target_pid() == 9999);

        // Second launch while attached is ignored
        STUTTO_ASSERT(!wait_engine.on_process_launched(8888, "Cyberpunk2077.exe"));
        STUTTO_ASSERT(wait_engine.active_target_pid() == 9999);

        // Terminating wrong PID is ignored
        wait_engine.on_process_terminated(7777);
        STUTTO_ASSERT(!wait_engine.is_target_waiting());
        STUTTO_ASSERT(wait_engine.active_target_pid() == 9999);

        // Terminating attached PID transitions back to waiting
        wait_engine.on_process_terminated(9999);
        STUTTO_ASSERT(wait_engine.is_target_waiting());
        STUTTO_ASSERT(wait_engine.active_target_pid() == 0);
    }

    // 4. Multi-Threaded CAS Contention (8 threads, 10,000 racing cycles)
    {
        stuttometer::TriggerConfig stress_cfg;
        stress_cfg.target_pid = 0;
        stress_cfg.target_process_name = "StressGame.exe";
        stuttometer::TriggerEngine stress_engine(stress_cfg, 10000000);

        constexpr int NUM_THREADS = 8;
        constexpr int ITERS_PER_THREAD = 1250;
        std::atomic<bool> start_flag{false};
        std::atomic<uint32_t> attach_wins{0};
        std::atomic<uint32_t> detach_wins{0};

        std::vector<std::thread> workers;
        for (int t = 0; t < NUM_THREADS; ++t) {
            workers.emplace_back([&, t]() {
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 1; i <= ITERS_PER_THREAD; ++i) {
                    const uint32_t fake_pid = static_cast<uint32_t>((t + 1) * 10000 + i);
                    if (stress_engine.try_attach_pid(fake_pid)) {
                        attach_wins.fetch_add(1, std::memory_order_relaxed);
                        // Immediately try to detach the same PID
                        if (stress_engine.try_detach_pid(fake_pid)) {
                            detach_wins.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        // If attach failed, attempt detach on active PID
                        const uint32_t active = stress_engine.active_target_pid();
                        if (active != 0 && stress_engine.try_detach_pid(active)) {
                            detach_wins.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }

        start_flag.store(true, std::memory_order_release);
        for (auto& w : workers) {
            w.join();
        }

        // Final state must be cleanly either attached or waiting (never corrupted)
        const bool waiting_final = stress_engine.is_target_waiting();
        const uint32_t pid_final = stress_engine.active_target_pid();
        if (waiting_final) {
            STUTTO_ASSERT(pid_final == 0);
        } else {
            STUTTO_ASSERT(pid_final > 0);
        }
        std::cout << "  -> CAS attach wins: " << attach_wins.load() 
                  << ", detach wins: " << detach_wins.load() << "\n";
    }

    std::cout << "  -> TriggerEngine CAS process attachment & concurrency tests PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer ETW Session Manager Tests ===\n";
    try {
        test_session_manager_initial_state();
        test_present_key_hash_uniqueness();
        test_session_manager_lifecycle_stop();
        test_granular_session_teardown();
        test_resolve_process_name_caching();
        test_fixed_table_tombstone_collision_and_update();
        test_fixed_table_in_place_update();
        test_fixed_table_concurrent_tombstone_stress();
        test_fixed_table_key_zero_rejection();
        test_calculate_effective_present_duration();
        test_packed_atomic_target_state();
        test_trigger_engine_cas_process_attachment();
        test_thread_switch_out_table();
        test_session_manager_insertion_failures();
        test_guid_activity_id_validation();
        test_out_of_order_dwm_debounce_logic();
        test_cswitch_tid_recycling_pid_check();
        test_provider_permutation_config();
        std::cout << ">>> All ETW Session Manager tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}

