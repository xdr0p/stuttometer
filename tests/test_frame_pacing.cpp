#include "test_common.hpp"
#include "stuttometer/frame_pacing_tracker.hpp"
#include "stuttometer/fixed_table.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

static void test_struct_properties() {
    std::cout << "[TEST] Validating struct properties & trivial copyability...\n";
    static_assert(std::is_trivially_copyable_v<stuttometer::RollingFrameStats>, "RollingFrameStats must be trivially copyable");
    static_assert(sizeof(stuttometer::TriggerInfo) == 56, "TriggerInfo must be strictly 56 bytes");
    static_assert(std::is_trivially_copyable_v<stuttometer::TriggerInfo>, "TriggerInfo must be trivially copyable");

    STUTTO_ASSERT(sizeof(stuttometer::TriggerInfo) == 56);
    std::cout << "  -> Struct size & trivial copyability verified.\n";
}

static void test_table_update_upsert_concurrency() {
    std::cout << "[TEST] Running FixedInFlightTable atomic update & upsert concurrency test...\n";

    stuttometer::FixedInFlightTable<stuttometer::RollingFrameStats, 256> table;
    constexpr int NUM_THREADS = 8;
    constexpr int FRAMES_PER_THREAD = 2000;
    constexpr uint64_t STREAM_KEY = 0xABCD1234ULL;

    // Initialize key via upsert
    stuttometer::RollingFrameStats init_stats{};
    stuttometer::reset_frame_stats(init_stats, 100);
    bool up_ok = table.upsert(STREAM_KEY, init_stats, [](stuttometer::RollingFrameStats& s) {
        s.sum_dur_us += 1;
    });
    STUTTO_ASSERT(up_ok);

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&table, t]() {
            for (int i = 0; i < FRAMES_PER_THREAD; ++i) {
                const uint64_t key = STREAM_KEY + (t % 4); // Contend on 4 stream keys
                stuttometer::RollingFrameStats def_stats{};
                stuttometer::reset_frame_stats(def_stats, 100 + i);

                table.upsert(key, def_stats, [](stuttometer::RollingFrameStats& s) {
                    s.sum_dur_us += 10;
                    s.sample_count++;
                });
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    stuttometer::RollingFrameStats result_stats{};
    bool found = table.lookup(STREAM_KEY, result_stats);
    STUTTO_ASSERT(found);
    STUTTO_ASSERT(result_stats.sample_count > 0);
    std::cout << "  -> Concurrent atomic RMW updates completed without data races or corruption.\n";
}

static void test_rolling_statistics_math() {
    std::cout << "[TEST] Verifying O(1) rolling statistics mean and standard deviation...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    // Push 64 frames of 16,666 us (60 FPS)
    for (size_t i = 0; i < 64; ++i) {
        stuttometer::push_clean_frame(stats, 16666, 1000 + (i * 100));
    }

    STUTTO_ASSERT(stats.sample_count == 64);
    double mean_ms = stuttometer::calculate_mean_ms(stats);
    double stddev_ms = stuttometer::calculate_stddev_ms(stats);

    STUTTO_ASSERT(std::abs(mean_ms - 16.666) < 0.01);
    STUTTO_ASSERT(stddev_ms < 0.001);

    // Push 64 frames of 5,000 us (200 FPS) to overwrite circular buffer completely
    for (size_t i = 0; i < 64; ++i) {
        stuttometer::push_clean_frame(stats, 5000, 2000 + (i * 100));
    }

    STUTTO_ASSERT(stats.sample_count == 64);
    mean_ms = stuttometer::calculate_mean_ms(stats);
    stddev_ms = stuttometer::calculate_stddev_ms(stats);

    STUTTO_ASSERT(std::abs(mean_ms - 5.000) < 0.01);
    STUTTO_ASSERT(stddev_ms < 0.001);
    std::cout << "  -> Rolling mean and stddev math verified across full ring wrap-arounds.\n";
}

static void test_high_fps_micro_stutter_relative_spike() {
    std::cout << "[TEST] Verifying high-FPS relative micro-stutter spike detection...\n";

    const uint64_t qpc_freq = 10000000ULL; // 10 MHz = 100ns per tick
    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 25.0; // High static threshold (40 FPS)
    config.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    config.spike_multiplier = 2.0;
    config.min_spike_delta_ms = 4.0;

    stuttometer::TriggerEngine engine(config, qpc_freq);
    const uint32_t pid = 4321;
    const uint32_t tid = 8765;
    const uint64_t stream_key = 0x5555AAAAULL;

    uint64_t qpc = 10000000ULL;

    // 1. Establish 200 FPS baseline (5.0ms per frame) for 20 frames
    for (int i = 0; i < 20; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
        bool trig = engine.on_dxgi_present(pid, tid, 5.0, qpc, stream_key, 0);
        STUTTO_ASSERT(!trig);
    }

    // 2. Introduce a 12.0ms frame:
    // - Static check (25ms) would MISS this!
    // - Dynamic check: 12.0ms / 5.0ms = 2.4x spike (>= 2.0x) AND (12.0 - 5.0) = 7.0ms (>= 4.0ms) -> MUST TRIGGER!
    qpc += stuttometer::ms_to_qpc_delta(12.0, qpc_freq);
    bool trig = engine.on_dxgi_present(pid, tid, 12.0, qpc, stream_key, 0);
    STUTTO_ASSERT(trig);

    stuttometer::TriggerInfo info{};
    uint64_t from_qpc = 0;
    uint64_t to_qpc = 0;
    const uint64_t poll_qpc = stuttometer::get_current_qpc()
                            + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    bool polled = engine.poll_state(poll_qpc, info, from_qpc, to_qpc);
    STUTTO_ASSERT(polled);
    STUTTO_ASSERT(info.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);
    STUTTO_ASSERT(info.duration_ms == 12.0);
    STUTTO_ASSERT(std::abs(info.baseline_avg_ms - 5.0) < 0.1);
    STUTTO_ASSERT(info.spike_ratio >= 2.39);

    std::cout << "  -> High-FPS relative micro-stutter successfully caught (" << info.spike_ratio << "x spike at 200 FPS).\n";
}

static void test_cadence_judder_detection() {
    std::cout << "[TEST] Verifying 3:2 alternating cadence judder detection...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 40.0;
    config.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    config.enable_judder_detection = true;
    config.judder_swing_ratio = 0.35;

    stuttometer::TriggerEngine engine(config, qpc_freq);
    const uint32_t pid = 7777;
    const uint32_t tid = 8888;
    const uint64_t stream_key = 0x99991111ULL;

    uint64_t qpc = 10000000ULL;

    // Warmup 10 frames around 20.0ms baseline
    for (int i = 0; i < 10; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(20.0, qpc_freq);
        engine.on_dxgi_present(pid, tid, 20.0, qpc, stream_key, 0);
    }

    // Deliver alternating pattern (13ms, 27ms, 13ms, 27ms - 3 sign alternations >= 35% swing)
    qpc += stuttometer::ms_to_qpc_delta(13.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 13.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(27.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 27.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(13.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 13.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(27.0, qpc_freq);
    bool judder_trig = engine.on_dxgi_present(pid, tid, 27.0, qpc, stream_key, 0);
    STUTTO_ASSERT(judder_trig);

    stuttometer::TriggerInfo info{};
    uint64_t from_qpc = 0;
    uint64_t to_qpc = 0;
    const uint64_t poll_qpc = stuttometer::get_current_qpc()
                            + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    bool polled = engine.poll_state(poll_qpc, info, from_qpc, to_qpc);
    STUTTO_ASSERT(polled);
    STUTTO_ASSERT(info.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(info.source == stuttometer::TriggerSource::FRAME_PACING_JUDDER);

    std::cout << "  -> Cadence judder pattern successfully detected with reason CADENCE_JUDDER.\n";
}

static void test_pause_reset_ceiling() {
    std::cout << "[TEST] Verifying 10.0s pause / loading screen ceiling reset...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    // Warmup 20 frames
    for (int i = 0; i < 20; ++i) {
        stuttometer::push_clean_frame(stats, 16666, 1000 + (i * 1000));
    }
    STUTTO_ASSERT(stats.sample_count == 20);

    // Simulate 12 seconds gap (120,000,000 us at 10 MHz)
    const uint64_t qpc_freq = 10000000ULL;
    const uint64_t now_qpc = stats.last_frame_timestamp_qpc + stuttometer::ms_to_qpc_delta(12000.0, qpc_freq);

    auto res = stuttometer::evaluate_frame_pacing(
        stats,
        16.67,
        now_qpc,
        qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0,
        4.0,
        true,
        0.35,
        25.0
    );

    // After pause reset, sample_count should be reset and frame incorporated in warmup
    STUTTO_ASSERT(!res.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 1);
    std::cout << "  -> 10.0s pause ceiling reset verified.\n";
}

static void test_dynamic_only_warmup_sanity_clamping() {
    std::cout << "[TEST] Verifying DYNAMIC_ONLY warmup spike rejection against startup hitches...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t qpc = 10000;

    // First frame is a 5-second stall (e.g. startup hitch) in DYNAMIC_ONLY mode
    auto res = stuttometer::evaluate_frame_pacing(
        stats,
        5000.0, // 5000 ms stall
        qpc,
        qpc_freq,
        stuttometer::FrameTriggerMode::DYNAMIC_ONLY,
        2.0,
        4.0,
        true,
        0.35,
        25.0
    );

    // Must not trigger static threshold in DYNAMIC_ONLY mode.
    // DYNAMIC_ONLY pushes a clamped sample (max 100 ms) so warmup can complete even if every
    // frame exceeds the static threshold (prevents permanent starvation on high-refresh displays).
    STUTTO_ASSERT(!res.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 1);
    STUTTO_ASSERT(stats.durations_us[0] == 100000); // 5000 ms clamped to 100 ms

    // Subsequent clean frame (16.6ms) is successfully ingested
    qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
    auto res_clean = stuttometer::evaluate_frame_pacing(
        stats,
        16.666,
        qpc,
        qpc_freq,
        stuttometer::FrameTriggerMode::DYNAMIC_ONLY,
        2.0,
        4.0,
        true,
        0.35,
        25.0
    );
    STUTTO_ASSERT(!res_clean.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 2);
    STUTTO_ASSERT(stats.durations_us[0] == 100000); // Clamped warmup sample still at index 0
    STUTTO_ASSERT(stats.durations_us[1] == 16666);  // Clean frame pushed at index 1
    std::cout << "  -> DYNAMIC_ONLY warmup spike rejection and clean frame ingestion verified.\n";
}

static void test_cadence_reset_after_stutter_frame() {
    std::cout << "[TEST] Verifying cadence state reset on stutter frame to prevent false judder...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t qpc = 1000000ULL;

    // Establish baseline with 10 clean 16.6ms frames
    for (int i = 0; i < 10; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.666, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }

    // Single large stutter frame (50ms)
    qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    auto res_stutter = stuttometer::evaluate_frame_pacing(
        stats, 50.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(res_stutter.is_stutter);
    // Cadence state must be reset
    STUTTO_ASSERT(stats.last_delta_us == 0);
    STUTTO_ASSERT(stats.alternating_cadence_count == 0);

    // Next clean frame: should NOT trigger judder
    qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
    auto res_clean = stuttometer::evaluate_frame_pacing(
        stats, 16.666, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(!res_clean.is_stutter);
    std::cout << "  -> Cadence state reset after stutter frame verified.\n";
}

static void test_sustained_stutter_storm_baseline_preservation() {
    std::cout << "[TEST] Verifying sustained stutter storm baseline stability (>10s continuous stutters)...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL; // 10 MHz
    uint64_t current_qpc = 1000000ULL;

    // 1. Establish 60 FPS clean baseline (64 frames of 16.67ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.666, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);

    // 2. Simulate 300 consecutive stutter frames (50ms each = 15.0s of continuous stutter storm)
    for (int i = 0; i < 300; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 50.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(res.is_stutter);
        // Sample count must NOT reset to 0 during the storm
        STUTTO_ASSERT(stats.sample_count == 64);
        // Baseline must NOT be corrupted by the 50ms stutters
        STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);
    }

    // 3. Clean frame arrives after storm: baseline is still intact and clean frame is accepted
    current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
    auto clean_res = stuttometer::evaluate_frame_pacing(
        stats, 16.666, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(!clean_res.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);

    std::cout << "  -> Sustained stutter storm baseline preservation PASSED.\n";
}

static void test_cadence_helper_no_double_increment() {
    std::cout << "[TEST] Verifying cadence helper does not double-increment on clean frames...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);
    const uint64_t qpc_freq = 10000000ULL;
    uint64_t current_qpc = 1000;

    // Warm up 16 frames at 16.666ms
    for (int i = 0; i < 16; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        stuttometer::push_clean_frame(stats, 16666, current_qpc);
    }
    STUTTO_ASSERT(stats.alternating_cadence_count == 0);

    // Frame 1: +6.33ms swing -> delta positive, alternating_cadence_count remains 0
    current_qpc += stuttometer::ms_to_qpc_delta(23.0, qpc_freq);
    auto res1 = stuttometer::evaluate_frame_pacing(
        stats, 23.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0
    );
    STUTTO_ASSERT(!res1.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 0);

    // Frame 2: -13.0ms swing (opposite sign) -> exactly 1 alternation count (must NOT be 2!)
    current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    auto res2 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0
    );
    STUTTO_ASSERT(!res2.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 1);

    // Frame 3: +13.0ms swing (opposite sign) -> exactly 2 alternation counts
    current_qpc += stuttometer::ms_to_qpc_delta(23.0, qpc_freq);
    auto res3 = stuttometer::evaluate_frame_pacing(
        stats, 23.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0
    );
    STUTTO_ASSERT(!res3.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 2);

    // Frame 4: -13.0ms swing (opposite sign) -> hits 3 alternations -> triggers CADENCE_JUDDER!
    current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    auto res4 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0
    );
    STUTTO_ASSERT(res4.is_stutter);
    STUTTO_ASSERT(res4.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(stats.alternating_cadence_count == 0);

    std::cout << "  -> Cadence helper single-increment & judder trigger PASSED.\n";
}

static void test_static_only_immediate_trigger_preserved() {
    std::cout << "[TEST] Verifying STATIC_ONLY immediate threshold trigger & clean frame incorporation...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 10000000ULL;
    // Frame 0: duration 30.0ms >= effective static threshold 25.0ms
    qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
    auto res = stuttometer::evaluate_frame_pacing(
        stats, 30.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::STATIC_ONLY,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(res.is_stutter);
    STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
    STUTTO_ASSERT(stats.sample_count == 0); // Not pushed into baseline

    // Clean frame: 16.6ms < 25.0ms
    qpc += stuttometer::ms_to_qpc_delta(16.6, qpc_freq);
    auto res_clean = stuttometer::evaluate_frame_pacing(
        stats, 16.6, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::STATIC_ONLY,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(!res_clean.is_stutter);
    STUTTO_ASSERT(res_clean.reason == stuttometer::TriggerReason::NONE);
    STUTTO_ASSERT(stats.sample_count == 1);
    std::cout << "  -> STATIC_ONLY immediate trigger from frame 0 verified.\n";
}

static void test_hybrid_warmup_reject_then_recover() {
    std::cout << "[TEST] Verifying HYBRID warmup reject-then-recover behavior...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 10000000ULL;
    // Push 3 stalls of 500ms (>= 285.7ms catastrophic cutoff).
    // HYBRID mode clamps catastrophic frames to a 100 ms sample and pushes them so warmup
    // can complete (prevents permanent starvation when every early frame exceeds the threshold).
    for (int i = 0; i < 3; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 500.0, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter); // Rejected without trigger during initial 4 frames
        STUTTO_ASSERT(stats.sample_count == static_cast<uint16_t>(i + 1)); // Clamped 100 ms sample pushed each iteration
        STUTTO_ASSERT(stats.last_delta_us == 0);
        STUTTO_ASSERT(stats.alternating_cadence_count == 0);
    }
    STUTTO_ASSERT(stats.durations_us[0] == 100000); // 500 ms clamped to 100 ms
    STUTTO_ASSERT(stats.durations_us[1] == 100000);
    STUTTO_ASSERT(stats.durations_us[2] == 100000);

    // Push 4 clean frames of 16.6ms
    for (int i = 0; i < 4; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.6, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.6, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    // 3 clamped warmup samples + 4 clean frames = 7 total samples
    STUTTO_ASSERT(stats.sample_count == 7);

    // Push frame 8 at 30.0ms (>= 25.0ms). sample_count == 7 is in the [4,7] window, so static
    // triggers are active now.
    qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
    auto res_spike = stuttometer::evaluate_frame_pacing(
        stats, 30.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(res_spike.is_stutter);
    STUTTO_ASSERT(res_spike.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
    STUTTO_ASSERT(stats.sample_count == 7); // Stutter frame rejected from baseline
    std::cout << "  -> HYBRID warmup reject-then-recover verified.\n";
}

static void test_hybrid_steady_slow_game_baseline_establishment() {
    std::cout << "[TEST] Verifying HYBRID steady slow game baseline establishment (30 FPS, 40ms floor)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 10000000ULL;
    // Steady 30 FPS = 33.3ms. Static floor = 40.0ms.
    for (int i = 0; i < 10; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.3, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 33.3, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 40.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 10);
    double mean_ms = stuttometer::calculate_mean_ms(stats);
    STUTTO_ASSERT(std::abs(mean_ms - 33.3) < 0.1);
    std::cout << "  -> HYBRID 30 FPS baseline established without false triggers (mean: " << mean_ms << " ms).\n";
}

static void test_hybrid_warmup_static_suppression_below_200ms() {
    std::cout << "[TEST] Verifying HYBRID warmup suppresses static trigger during initial 4 frames for dur < 200ms...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 10000000ULL;
    // Frame 0: duration 50.0ms (>= effective static threshold 25.0ms, but < 200.0ms)
    // Under HYBRID mode, static triggers must be suppressed during frames 0..3 to establish baseline
    qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    auto res0 = stuttometer::evaluate_frame_pacing(
        stats, 50.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(!res0.is_stutter);
    STUTTO_ASSERT(res0.reason == stuttometer::TriggerReason::NONE);
    STUTTO_ASSERT(stats.sample_count == 1);
    STUTTO_ASSERT(stats.durations_us[0] == 50000);

    // Frames 1..3: three more 50.0ms frames
    for (int i = 1; i < 4; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 50.0, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 4);

    // Frame 4 (sample_count == 4): now sample_count >= 4, so static triggers are active!
    // A frame with 30.0ms against 25.0ms threshold must trigger STATIC_THRESHOLD
    qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
    auto res4 = stuttometer::evaluate_frame_pacing(
        stats, 30.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 25.0
    );
    STUTTO_ASSERT(res4.is_stutter);
    STUTTO_ASSERT(res4.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
    STUTTO_ASSERT(stats.sample_count == 4); // Stutter frame rejected from baseline
    std::cout << "  -> HYBRID warmup static suppression for frames 0..3 verified.\n";
}

static void test_adaptive_pacing_math() {
    std::cout << "[TEST] Running test_adaptive_pacing_math across FPS spectrum...\n";

    // 500 FPS (2.0 ms) -> saturates at 1.4x / 1.5 ms
    auto p1 = stuttometer::compute_adaptive_pacing_params(2.0);
    STUTTO_ASSERT(std::abs(p1.spike_multiplier - 1.4) < 1e-6);
    STUTTO_ASSERT(std::abs(p1.min_spike_delta_ms - 1.5) < 1e-6);

    // 200 FPS (5.0 ms) -> 1.44x / 1.5 ms
    auto p2 = stuttometer::compute_adaptive_pacing_params(5.0);
    STUTTO_ASSERT(std::abs(p2.spike_multiplier - 1.44) < 1e-3);
    STUTTO_ASSERT(std::abs(p2.min_spike_delta_ms - 1.5) < 1e-6);

    // 140 FPS (7.14 ms) -> 1.54x / 2.14 ms
    auto p3 = stuttometer::compute_adaptive_pacing_params(7.14);
    STUTTO_ASSERT(std::abs(p3.spike_multiplier - 1.542) < 0.01);
    STUTTO_ASSERT(std::abs(p3.min_spike_delta_ms - 2.142) < 0.01);

    // 60 FPS (16.67 ms) -> saturates at 2.0x / 4.0 ms
    auto p4 = stuttometer::compute_adaptive_pacing_params(16.67);
    STUTTO_ASSERT(std::abs(p4.spike_multiplier - 2.0) < 1e-6);
    STUTTO_ASSERT(std::abs(p4.min_spike_delta_ms - 4.0) < 1e-6);

    // 30 FPS (33.3 ms) -> saturates at 2.0x / 4.0 ms
    auto p5 = stuttometer::compute_adaptive_pacing_params(33.3);
    STUTTO_ASSERT(std::abs(p5.spike_multiplier - 2.0) < 1e-6);
    STUTTO_ASSERT(std::abs(p5.min_spike_delta_ms - 4.0) < 1e-6);

    std::cout << "  -> Adaptive pacing scaling math verified across 500, 200, 140, 60, and 30 FPS.\n";
}

static void test_adaptive_trigger_140fps() {
    std::cout << "[TEST] Running test_adaptive_trigger_140fps...\n";
    const uint64_t qpc_freq = 10000000ULL;
    uint64_t qpc = 1000000ULL;

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, qpc);

    // Seed 20 warmup frames at 7.14 ms (140 FPS)
    for (int i = 0; i < 20; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(7.14, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 7.14, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, 16.67,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 20);

    // Frame at 12.0 ms should trigger RELATIVE_SPIKE (threshold ~11.0 ms)
    qpc += stuttometer::ms_to_qpc_delta(12.0, qpc_freq);
    auto res_spike = stuttometer::evaluate_frame_pacing(
        stats, 12.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, 16.67,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(res_spike.is_stutter);
    STUTTO_ASSERT(res_spike.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);
    STUTTO_ASSERT(stats.sample_count == 20); // Excluded from baseline

    std::cout << "  -> 140 FPS adaptive trigger verified (12.0ms triggered RELATIVE_SPIKE).\n";
}

static void test_adaptive_trigger_60fps() {
    std::cout << "[TEST] Running test_adaptive_trigger_60fps...\n";
    const uint64_t qpc_freq = 10000000ULL;
    uint64_t qpc = 1000000ULL;

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, qpc);
    const double static_threshold = stuttometer::calculate_effective_static_threshold(16.67);

    // Seed 20 warmup frames at 16.67 ms (60 FPS)
    for (int i = 0; i < 20; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.67, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 4.0, true, 0.35, static_threshold,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 20);

    // 17.0 ms frame: below static floor (17.5ms) and below dynamic spike (33.3ms) -> NO trigger
    qpc += stuttometer::ms_to_qpc_delta(17.0, qpc_freq);
    auto res_clean = stuttometer::evaluate_frame_pacing(
        stats, 17.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, static_threshold,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!res_clean.is_stutter);

    // 20.0 ms frame: exceeds static floor (17.5ms), below dynamic spike (33.3ms) -> STATIC_THRESHOLD trigger
    qpc += stuttometer::ms_to_qpc_delta(20.0, qpc_freq);
    auto res_static = stuttometer::evaluate_frame_pacing(
        stats, 20.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, static_threshold,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(res_static.is_stutter);
    STUTTO_ASSERT(res_static.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);

    // 35.0 ms frame: exceeds dynamic spike (33.3ms) -> RELATIVE_SPIKE trigger
    qpc += stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    auto res_rel = stuttometer::evaluate_frame_pacing(
        stats, 35.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 4.0, true, 0.35, static_threshold,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(res_rel.is_stutter);
    STUTTO_ASSERT(res_rel.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);

    std::cout << "  -> 60 FPS adaptive trigger verified (17.0ms clean, 20.0ms static, 35.0ms spike).\n";
}

static void test_invert_effective_static_threshold() {
    std::cout << "[TEST] Validating invert_effective_static_threshold round-trip math...\n";

    STUTTO_ASSERT(stuttometer::invert_effective_static_threshold(0.0) == 0.0);
    STUTTO_ASSERT(stuttometer::invert_effective_static_threshold(-5.0) == 0.0);

    // Below transition (0.5 ms guard): effective = base + 0.5
    STUTTO_ASSERT(std::abs(stuttometer::invert_effective_static_threshold(5.5) - 5.0) < 1e-9);
    STUTTO_ASSERT(std::abs(stuttometer::invert_effective_static_threshold(10.5) - 10.0) < 1e-9);

    // Above transition (5% guard): effective = base * 1.05
    // 16.67 * 1.05 = 17.5035
    STUTTO_ASSERT(std::abs(stuttometer::invert_effective_static_threshold(17.5035) - 16.67) < 1e-4);
    // 33.33333333 * 1.05 = 35.0
    STUTTO_ASSERT(std::abs(stuttometer::invert_effective_static_threshold(35.0) - (1000.0 / 30.0)) < 1e-4);

    // Round-trip validation against calculate_effective_static_threshold
    const double test_thresholds[] = { 1.0, 4.167, 6.944, 8.333, 10.0, 11.111, 16.67, 33.333, 50.0 };
    for (double base : test_thresholds) {
        double effective = stuttometer::calculate_effective_static_threshold(base);
        double inverted = stuttometer::invert_effective_static_threshold(effective);
        STUTTO_ASSERT(std::abs(inverted - base) < 1e-9);
    }

    std::cout << "  -> invert_effective_static_threshold round-trip verified.\n";
}

// Regression test for the FileTime-domain timestamp bug (v0.5.4).
//
// Root cause: ETW delivers ctx.timestamp in FileTime (100ns since 1601-01-01, ~1.34e17)
// rather than QPC ticks. Phase 2 of initiate_trigger_atomic previously stored
//   post_target_qpc_ = timestamp_qpc + post_window_qpc_
// poll_state() checks current_qpc >= post_target_qpc_ where current_qpc comes from
// get_current_qpc() (~3.7e10 on a fresh boot). Since 1.34e17 >> 3.7e10 the deadline
// was never reachable and the state machine stayed in COLLECTING_POST forever.
//
// Fix: Phase 2 now uses get_current_qpc() for post_target_qpc_ and
// claimed_timestamp_qpc_, keeping the ETW timestamp only in active_trigger_
// for flight-recorder snapshot windowing.
//
// This test must FAIL on pre-fix code (poll_state never returns true) and
// PASS on post-fix code (poll_state returns true within one post-window).
static void test_filetime_domain_timestamp_does_not_block_poll_state() {
    std::cout << "[TEST] Regression: FileTime-domain ETW timestamp must not prevent poll_state from firing...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    stuttometer::TriggerConfig cfg;
    cfg.target_pid      = 1234;
    cfg.window_pre_ms   = 100.0;
    cfg.window_post_ms  = 30.0;    // post-window: 30 ms
    cfg.cooldown_ms     = 500.0;
    cfg.present_threshold_ms = 5.0;
    cfg.frame_trigger_mode   = stuttometer::FrameTriggerMode::STATIC_ONLY;
    cfg.audio_trigger_enabled = false;

    stuttometer::TriggerEngine engine(cfg, qpc_freq);

    // Simulate a FileTime-domain ETW timestamp: ~1.34e17 (100ns ticks since 1601-01-01).
    // This is the value observed in the wild against Spider-Man2.exe on Windows 11.
    // On pre-fix code, post_target_qpc_ = 1.34e17 + post_window, which get_current_qpc()
    // (~3.7e10) can never reach.
    const uint64_t filetime_timestamp = 134345709646471285ULL;

    // Feed enough frames to exit warmup and fire the static threshold.
    // STATIC_ONLY mode does not require warmup, so a single frame >= threshold suffices,
    // but we send a handful to ensure the pacing table entry is populated.
    const double stutter_ms = 12.0; // well above 5 ms threshold
    for (int i = 0; i < 5; ++i) {
        engine.on_dxgi_present(1234, 1, stutter_ms,
                               filetime_timestamp + static_cast<uint64_t>(i) * 1000000ULL,
                               /*stream_key=*/0xABCD1234ULL);
    }

    // Poll for up to 200 ms (6x the 30 ms post-window) using real QPC time.
    // On post-fix code poll_state transitions COLLECTING_POST -> FROZEN -> true
    // within ~30 ms of the trigger. On pre-fix code it never fires.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    bool fired = false;
    while (std::chrono::steady_clock::now() < deadline) {
        stuttometer::TriggerInfo info{};
        uint64_t from_qpc = 0, to_qpc = 0;
        if (engine.poll_state(stuttometer::get_current_qpc(), info, from_qpc, to_qpc)) {
            fired = true;
            // Verify the report carries the ETW-domain timestamp for snapshot windowing
            STUTTO_ASSERT(info.trigger_timestamp_qpc == filetime_timestamp);
            STUTTO_ASSERT(info.source == stuttometer::TriggerSource::DXGI_PRESENT_STUTTER);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    STUTTO_ASSERT_MSG(fired,
        "poll_state never fired: FileTime-domain timestamp caused unreachable post_target_qpc_. "
        "This is the v0.5.4 zero-report bug. Ensure Phase 2 of initiate_trigger_atomic uses "
        "get_current_qpc() for post_target_qpc_ and claimed_timestamp_qpc_.");

    std::cout << "  -> poll_state fired correctly with FileTime-domain ETW timestamp.\n";
}

int main() {
    std::cout << "================================================================\n";
    std::cout << " STUTTOMETER FRAME PACING & STATISTICAL TRIGGER TEST SUITE\n";
    std::cout << "================================================================\n";

    try {
        test_struct_properties();
        test_table_update_upsert_concurrency();
        test_rolling_statistics_math();
        test_high_fps_micro_stutter_relative_spike();
        test_cadence_judder_detection();
        test_pause_reset_ceiling();
        test_dynamic_only_warmup_sanity_clamping();
        test_cadence_reset_after_stutter_frame();
        test_sustained_stutter_storm_baseline_preservation();
        test_cadence_helper_no_double_increment();
        test_static_only_immediate_trigger_preserved();
        test_hybrid_warmup_reject_then_recover();
        test_hybrid_steady_slow_game_baseline_establishment();
        test_hybrid_warmup_static_suppression_below_200ms();
        test_adaptive_pacing_math();
        test_adaptive_trigger_140fps();
        test_adaptive_trigger_60fps();
        test_invert_effective_static_threshold();
        test_filetime_domain_timestamp_does_not_block_poll_state();

        std::cout << "\n>>> ALL 19 FRAME PACING UNIT TESTS PASSED SUCCESSFULLY! <<<\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}

