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
    static_assert(sizeof(stuttometer::RollingFrameStats) == 384, "RollingFrameStats must be strictly 384 bytes");
    static_assert(std::is_trivially_copyable_v<stuttometer::RollingFrameStats>, "RollingFrameStats must be trivially copyable");
    static_assert(sizeof(stuttometer::TriggerInfo) == 64, "TriggerInfo must be strictly 64 bytes");
    static_assert(std::is_trivially_copyable_v<stuttometer::TriggerInfo>, "TriggerInfo must be trivially copyable");

    STUTTO_ASSERT(sizeof(stuttometer::RollingFrameStats) == 384);
    STUTTO_ASSERT(sizeof(stuttometer::TriggerInfo) == 64);
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
    config.spike_multiplier = stuttometer::DEFAULT_SPIKE_MULTIPLIER;
    config.min_spike_delta_ms = stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS;

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
    config.judder_swing_ratio = stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO;

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

    // Deliver alternating pattern (13ms, 27ms, 13ms, 27ms, 13ms, 27ms -> 5 alternations >= 35% swing)
    qpc += stuttometer::ms_to_qpc_delta(13.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 13.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(27.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 27.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(13.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 13.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(27.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 27.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(13.0, qpc_freq);
    engine.on_dxgi_present(pid, tid, 13.0, qpc, stream_key, 0);

    qpc += stuttometer::ms_to_qpc_delta(27.0, qpc_freq);
    bool judder_during = engine.on_dxgi_present(pid, tid, 27.0, qpc, stream_key, 0);
    STUTTO_ASSERT(!judder_during); // Episode still active, not emitted until close
    const uint64_t last_alt_qpc = qpc;

    // Close episode on gap >= 500 ms (e.g. 505 ms) with non-alternating frame (27.0ms -> delta 0)
    qpc += stuttometer::ms_to_qpc_delta(505.0, qpc_freq);
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
    STUTTO_ASSERT(info.trigger_timestamp_qpc == last_alt_qpc);
    STUTTO_ASSERT(info.judder_alternations >= 5);

    std::cout << "  -> Cadence judder pattern successfully detected with reason CADENCE_JUDDER.\n";
}

static void test_pause_reset_ceiling() {
    std::cout << "[TEST] Verifying 2.0s pause / loading screen ceiling reset...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    // Warmup 20 frames
    for (int i = 0; i < 20; ++i) {
        stuttometer::push_clean_frame(stats, 16666, 1000 + (i * 1000));
    }
    STUTTO_ASSERT(stats.sample_count == 20);

    // Simulate 3 seconds gap (exceeds 2.0s pause ceiling)
    const uint64_t qpc_freq = 10000000ULL;
    const uint64_t now_qpc = stats.last_frame_timestamp_qpc + stuttometer::ms_to_qpc_delta(3000.0, qpc_freq);

    auto res = stuttometer::evaluate_frame_pacing(
        stats,
        16.67,
        now_qpc,
        qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER,
        stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
        true,
        stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
        25.0
    );

    // After pause reset, sample_count should be reset and frame incorporated in warmup
    STUTTO_ASSERT(!res.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 1);
    std::cout << "  -> 2.0s pause ceiling reset verified.\n";
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER,
        stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
        true,
        stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER,
        stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
        true,
        stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }

    // Single large stutter frame (50ms)
    qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    auto res_stutter = stuttometer::evaluate_frame_pacing(
        stats, 50.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);

    // 2. Feed 60 consecutive 50.0ms frames. Frames 1..59 accumulate; frame 60 promotes!
    for (int i = 1; i <= 60; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 50.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        if (i == 1) {
            STUTTO_ASSERT(res.is_stutter);
        } else {
            STUTTO_ASSERT(!res.is_stutter);
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        }
        if (i < 60) {
            STUTTO_ASSERT(stats.candidate_count == i);
            STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);
        } else {
            // Frame 60 promotes to 50.0ms baseline
            STUTTO_ASSERT(stats.candidate_count == 0);
            STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 50.0) < 0.1);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 50.0) < 0.1);
            STUTTO_ASSERT(res.spike_ratio == 1.0);
        }
    }

    // 3. Subsequent 50.0ms frames are clean under the promoted baseline
    for (int i = 0; i < 10; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
        auto clean_res = stuttometer::evaluate_frame_pacing(
            stats, 50.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!clean_res.is_stutter);
        STUTTO_ASSERT(clean_res.reason == stuttometer::TriggerReason::NONE);
        STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 50.0) < 0.1);
    }

    std::cout << "  -> Sustained transition to 50.0 ms promoted and subsequent frames clean PASSED.\n";
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
        2.5, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0
    );
    STUTTO_ASSERT(!res1.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 0);

    // Frame 2: -13.0ms swing (opposite sign) -> exactly 1 alternation count (must NOT be 2!)
    current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    auto res2 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0
    );
    STUTTO_ASSERT(!res2.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 1);

    // Frame 3: +13.0ms swing (opposite sign) -> exactly 2 alternation counts
    current_qpc += stuttometer::ms_to_qpc_delta(23.0, qpc_freq);
    auto res3 = stuttometer::evaluate_frame_pacing(
        stats, 23.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0
    );
    STUTTO_ASSERT(!res3.is_stutter);
    STUTTO_ASSERT(stats.alternating_cadence_count == 2);

    // Frame 4: -13.0ms swing (opposite sign) -> hits 3 alternations -> episode continues active
    current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    const uint64_t last_alt_qpc = current_qpc;
    auto res4 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0,
        stuttometer::PacingProfile::CUSTOM, 3
    );
    STUTTO_ASSERT(!res4.is_stutter);
    STUTTO_ASSERT(stats.judder_episode_alternations == 3);
    STUTTO_ASSERT(stats.judder_episode_active == 1);

    // Frame 5: Close episode on gap >= 500ms with non-alternating frame (10.0ms -> delta 0)
    current_qpc += stuttometer::ms_to_qpc_delta(505.0, qpc_freq);
    auto res5 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0,
        stuttometer::PacingProfile::CUSTOM, 3
    );
    STUTTO_ASSERT(res5.is_stutter);
    STUTTO_ASSERT(res5.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(res5.judder_alternations == 3);
    STUTTO_ASSERT(res5.trigger_timestamp_qpc == last_alt_qpc);
    STUTTO_ASSERT(stats.judder_episode_active == 0);

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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
    );
    STUTTO_ASSERT(res.is_stutter);
    STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
    STUTTO_ASSERT(stats.sample_count == 0); // Not pushed into baseline

    // Clean frame: 16.6ms < 25.0ms
    qpc += stuttometer::ms_to_qpc_delta(16.6, qpc_freq);
    auto res_clean = stuttometer::evaluate_frame_pacing(
        stats, 16.6, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::STATIC_ONLY,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    // 3 clamped warmup samples + 4 clean frames = 7 total samples
    STUTTO_ASSERT(stats.sample_count == 7);

    // Push frame 8 at 30.0ms. At sample_count == 7, mean is ~52.3 ms, which lifts the ceiling
    // to ~56.3 ms (sample_count >= 4). Frame 8 at 30.0 ms is clean.
    qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
    auto res_spike = stuttometer::evaluate_frame_pacing(
        stats, 30.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
    );
    STUTTO_ASSERT(!res_spike.is_stutter);
    STUTTO_ASSERT(res_spike.reason == stuttometer::TriggerReason::NONE);
    STUTTO_ASSERT(stats.sample_count == 8);
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 40.0
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 4);

    // Frame 4 (sample_count == 4): mean is 50.0 ms, which lifts the ceiling to 54.0 ms (sample_count >= 4).
    // The 30.0 ms frame is clean.
    qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
    auto res4 = stuttometer::evaluate_frame_pacing(
        stats, 30.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
    );
    STUTTO_ASSERT(!res4.is_stutter);
    STUTTO_ASSERT(res4.reason == stuttometer::TriggerReason::NONE);
    STUTTO_ASSERT(stats.sample_count == 5);
    std::cout << "  -> HYBRID warmup static suppression for frames 0..3 verified.\n";
}

static void test_hybrid_warmup_100fps_on_200hz_no_static_trigger() {
    std::cout << "[TEST] Verifying HYBRID warmup at 100 FPS on 200 Hz display produces zero static triggers...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 10000000ULL;
    // 30 frames of 10.0 ms (100 FPS) against effective_static_threshold_ms = 5.25 (200 Hz display)
    for (int i = 0; i < 30; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.0, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.25,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
        STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
    }
    std::cout << "  -> HYBRID warmup 100 FPS on 200 Hz verified zero false triggers.\n";
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 16.67,
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 16.67,
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
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
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
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!res_clean.is_stutter);

    // 20.0 ms frame: exceeds static floor (17.5ms), below dynamic spike (33.3ms) -> STATIC_THRESHOLD trigger
    qpc += stuttometer::ms_to_qpc_delta(20.0, qpc_freq);
    auto res_static = stuttometer::evaluate_frame_pacing(
        stats, 20.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(res_static.is_stutter);
    STUTTO_ASSERT(res_static.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);

    // 35.0 ms frame: exceeds dynamic spike (33.3ms) -> RELATIVE_SPIKE trigger
    qpc += stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    auto res_rel = stuttometer::evaluate_frame_pacing(
        stats, 35.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
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

// Regression test for the FileTime-domain timestamp bug.
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
        "This is the zero-report bug. Ensure Phase 2 of initiate_trigger_atomic uses "
        "get_current_qpc() for post_target_qpc_ and claimed_timestamp_qpc_.");

    std::cout << "  -> poll_state fired correctly with FileTime-domain ETW timestamp.\n";
}

static void test_cadence_adaptation_173fps_to_100fps() {
    std::cout << "[TEST] Verifying cadence adaptation from 173 FPS to 100 FPS (60-frame convergence)...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL; // 10 MHz
    uint64_t current_qpc = 1000000ULL;

    // 1. Establish 173 FPS baseline (64 frames of 5.78ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 5.78, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.78) < 0.05);

    // 2. Feed Frame 1 at 10.2ms:
    // Trips RELATIVE_SPIKE, anchors candidate accumulation
    current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
    auto res1 = stuttometer::evaluate_frame_pacing(
        stats, 10.2, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(res1.is_stutter);
    STUTTO_ASSERT(res1.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);
    STUTTO_ASSERT(stats.candidate_count == 1);
    STUTTO_ASSERT(std::abs(res1.baseline_avg_ms - 5.78) < 0.05);

    // 3. Feed 59 additional frames at 10.2ms (total frames 2 through 60)
    for (int i = 2; i <= 60; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.2, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
        STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);

        if (i < 60) {
            STUTTO_ASSERT(stats.candidate_count == i);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 5.78) < 0.05);
        } else {
            // Frame 60: promotion executed!
            STUTTO_ASSERT(stats.candidate_count == 0);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 10.2) < 0.05);
            STUTTO_ASSERT(std::abs(res.baseline_fps - (1000.0 / 10.2)) < 0.5);
            STUTTO_ASSERT(res.spike_ratio == 1.0);
        }
    }

    // 4. Feed Frame 61 at 10.2ms: evaluated against the newly promoted 10.2ms baseline
    current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
    auto res61 = stuttometer::evaluate_frame_pacing(
        stats, 10.2, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!res61.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 10.2) < 0.1);

    std::cout << "  -> Cadence adaptation 173 FPS to 100 FPS PASSED.\n";
}

static void test_drs_flapping_resistance() {
    std::cout << "[TEST] Verifying Dynamic Resolution Scaling (DRS) flapping resistance...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t current_qpc = 1000000ULL;

    // Establish 200 FPS baseline (5.0ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 5.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.0) < 0.05);

    // Feed alternating frames (10.0ms, 15.0ms) for 100 frames
    for (int i = 0; i < 50; ++i) {
        // Frame at 10.0ms
        current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
        auto res10 = stuttometer::evaluate_frame_pacing(
            stats, 10.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(res10.is_stutter);
        STUTTO_ASSERT(stats.candidate_count == 1);

        // Frame at 15.0ms: |15.0 - 10.0| = 5.0ms > consistency tol (1.5ms) -> candidate reset
        current_qpc += stuttometer::ms_to_qpc_delta(15.0, qpc_freq);
        auto res15 = stuttometer::evaluate_frame_pacing(
            stats, 15.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0
        );
        STUTTO_ASSERT(res15.is_stutter);
        // Candidate consistency check failed; reset to 0 (frame is discarded without seeding)
        STUTTO_ASSERT(stats.candidate_count == 0);

        // Baseline must NOT be corrupted or promoted
        STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.0) < 0.05);
    }

    std::cout << "  -> DRS flapping resistance PASSED.\n";
}

static void test_rolling_frame_stats_alignment_and_cache_lines() {
    std::cout << "[TEST] Verifying RollingFrameStats layout, size (384 bytes), and cache alignment...\n";

    static_assert(sizeof(stuttometer::RollingFrameStats) == 384, "RollingFrameStats must be exactly 384 bytes (6 cache lines)");
    static_assert(alignof(stuttometer::RollingFrameStats) == 64, "RollingFrameStats must be 64-byte cache line aligned");
    static_assert(std::is_trivially_copyable_v<stuttometer::RollingFrameStats>, "RollingFrameStats must be trivially copyable");

    STUTTO_ASSERT(sizeof(stuttometer::RollingFrameStats) == 384);
    STUTTO_ASSERT(alignof(stuttometer::RollingFrameStats) == 64);
    STUTTO_ASSERT(std::is_trivially_copyable_v<stuttometer::RollingFrameStats>);

    // Verify member offset alignment
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, durations_us) == 0);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, sum_dur_us) == 256);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, sum_sq_dur_us) == 264);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, last_frame_timestamp_qpc) == 272);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, last_delta_us) == 280);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, sample_count) == 284);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, write_idx) == 286);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, alternating_cadence_count) == 288);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_consecutive_skips) == 289);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_count) == 290);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_clean_frames_since_last_match) == 292);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_first_qpc) == 296);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_sum_us) == 304);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, candidate_sum_sq_us) == 312);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, judder_episode_active) == 320);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, judder_episode_start_qpc) == 328);
    STUTTO_ASSERT(offsetof(stuttometer::RollingFrameStats, judder_episode_last_alt_qpc) == 336);

    std::cout << "  -> RollingFrameStats alignment and cache line verification PASSED.\n";
}

static void test_cadence_adaptation_clean_frame_interleaving() {
    std::cout << "[TEST] Verifying cadence adaptation with clean frame interleaving...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t current_qpc = 1000000ULL;

    // 1. Establish 173 FPS baseline (5.78ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 5.78, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.78) < 0.05);

    // 2. Feed 30 frames at 10.2ms (candidate reaches count = 30)
    for (int i = 1; i <= 30; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.2, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (i == 1) {
            STUTTO_ASSERT(res.is_stutter);
        } else {
            STUTTO_ASSERT(!res.is_stutter);
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        }
        STUTTO_ASSERT(stats.candidate_count == i);
    }
    STUTTO_ASSERT(stats.candidate_count == 30);

    // 3. Feed 1 clean frame at 5.78ms (candidate survives with count intact)
    current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
    auto clean_res = stuttometer::evaluate_frame_pacing(
        stats, 5.78, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!clean_res.is_stutter);
    STUTTO_ASSERT(stats.candidate_count == 30);
    STUTTO_ASSERT(stats.candidate_clean_frames_since_last_match == 1);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.78) < 0.05);

    // 4. Feed 30 additional frames at 10.2ms (completes 60 frames -> promotion executes on frame 60)
    for (int i = 31; i <= 60; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.2, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
        STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        if (i < 60) {
            STUTTO_ASSERT(stats.candidate_count == i);
        } else {
            // Promotion executes on frame 60
            STUTTO_ASSERT(stats.candidate_count == 0);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 10.2) < 0.05);
            STUTTO_ASSERT(res.spike_ratio == 1.0);
        }
    }

    // 5. Next frame at 10.2ms is clean under new baseline
    current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
    auto res_after = stuttometer::evaluate_frame_pacing(
        stats, 10.2, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!res_after.is_stutter);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 10.2) < 0.1);

    std::cout << "  -> Cadence adaptation with clean frame interleaving PASSED.\n";
}

static void test_candidate_reset_then_reaccumulate_promotes() {
    std::cout << "[TEST] Verifying candidate reset on deviation then re-accumulation to promotion...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t current_qpc = 1000000ULL;

    // 1. Establish 173 FPS baseline (5.78ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 5.78, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.78) < 0.05);

    // 2. Feed 20 frames at 30.0ms: accumulates candidate to 20
    for (int i = 1; i <= 20; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(30.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 30.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (i == 1) {
            STUTTO_ASSERT(res.is_stutter);
        } else {
            STUTTO_ASSERT(!res.is_stutter);
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        }
        STUTTO_ASSERT(stats.candidate_count == i);
    }
    STUTTO_ASSERT(stats.candidate_count == 20);

    // 3. Feed 1 frame at 10.2ms:
    // Consistency check against candidate mean (30.0ms) fails (|10.2 - 30.0| = 19.8 > 4.5).
    // Acts as a reset trigger: candidate_count -> 0; frame is NOT seeded.
    current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
    auto reset_res = stuttometer::evaluate_frame_pacing(
        stats, 10.2, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(reset_res.is_stutter);
    STUTTO_ASSERT(stats.candidate_count == 0);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 5.78) < 0.05);

    // 4. Feed 60 frames at 10.2ms:
    // Frame 1 seeds candidate_count = 1; 59 frames accumulate to 60.
    // Promotion executes on the 60th frame!
    for (int i = 1; i <= 60; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.2, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (i == 1) {
            STUTTO_ASSERT(res.is_stutter);
        } else {
            STUTTO_ASSERT(!res.is_stutter);
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        }
        if (i < 60) {
            STUTTO_ASSERT(stats.candidate_count == i);
        } else {
            // Frame 60: promotion executed!
            STUTTO_ASSERT(stats.candidate_count == 0);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 10.2) < 0.05);
            STUTTO_ASSERT(res.spike_ratio == 1.0);
        }
    }

    // 5. Frame 61 is clean
    current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
    auto clean_res = stuttometer::evaluate_frame_pacing(
        stats, 10.2, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!clean_res.is_stutter);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 10.2) < 0.1);

    std::cout << "  -> Candidate reset then re-accumulation to promotion PASSED.\n";
}

static void test_hybrid_144fps_to_30fps_promotes_cleanly() {
    std::cout << "[TEST] Verifying 144 FPS -> 30 FPS transition promotes cleanly under HYBRID mode (ceiling 5.5ms)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 144 FPS baseline (64 frames of 6.94ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 6.94) < 0.05);

    // 2. Feed 70 frames at 33.33 ms with ceiling 5.5 ms.
    // Promotion must occur by frame <= 65 of 33.33 ms regime.
    bool promoted = false;
    int promotion_frame = -1;
    for (int i = 1; i <= 70; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        // Detect promotion via baseline mean shifting to ~33.33 ms per C10
        if (!promoted && std::abs(stuttometer::calculate_mean_ms(stats) - 33.33) < 0.1) {
            promoted = true;
            promotion_frame = i;
        }
    }
    STUTTO_ASSERT(promoted);
    STUTTO_ASSERT(promotion_frame <= 65);
    std::cout << "  -> 144 FPS -> 30 FPS promoted cleanly at frame " << promotion_frame << ".\n";
}

static void test_hybrid_candidate_accumulation_progresses_past_seed() {
    std::cout << "[TEST] Verifying candidate accumulation progresses past seed (regression guard)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // Establish baseline at 144 FPS (64 frames of 6.94ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }
    // Feed 5 matching frames at 33.33 ms
    for (int i = 1; i <= 5; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(stats.candidate_count == static_cast<uint16_t>(i));
    }
    STUTTO_ASSERT(stats.candidate_count == 5);
    std::cout << "  -> Candidate accumulation progressed to count 5 as expected.\n";
}

static void test_dynamic_mode_144fps_to_30fps_promotes() {
    std::cout << "[TEST] Verifying DYNAMIC_ONLY mode 144 FPS -> 30 FPS promotes without consulting vblank threshold...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::DYNAMIC_ONLY,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }

    bool promoted = false;
    for (int i = 1; i <= 65; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::DYNAMIC_ONLY,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (std::abs(stuttometer::calculate_mean_ms(stats) - 33.33) < 0.1) {
            promoted = true;
            break;
        }
    }
    STUTTO_ASSERT(promoted);
    std::cout << "  -> DYNAMIC_ONLY 144 FPS -> 30 FPS promoted successfully.\n";
}

static void test_hybrid_low_framerate_sigma_relative() {
    std::cout << "[TEST] Verifying low-framerate (20 FPS / 50ms) adaptation with +/-3ms jitter promotes...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 60 FPS baseline (16.666 ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.666, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);

    // 2. Feed frames around 50.0 ms with +/-3.0 ms jitter (values: 50.0, 53.0, 47.0 ms)
    // stddev is ~2.45 ms > 1.0 ms. Under relative sigma (10% of 50ms = 5.0ms), it promotes!
    bool promoted = false;
    for (int i = 1; i <= 65; ++i) {
        const double jitter = (i % 3 == 0) ? 0.0 : ((i % 3 == 1) ? 3.0 : -3.0);
        const double dur = 50.0 + jitter;
        qpc += stuttometer::ms_to_qpc_delta(dur, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, dur, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (std::abs(stuttometer::calculate_mean_ms(stats) - 50.0) < 1.0) {
            promoted = true;
            break;
        }
    }
    STUTTO_ASSERT(promoted);
    std::cout << "  -> Low-framerate relative sigma adaptation PASSED.\n";
}

static void test_hybrid_noisy_transition_grace_band_preserves_mean() {
    std::cout << "[TEST] Verifying noisy transition with grace band skips preserves candidate mean...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 144 FPS baseline (6.94ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }

    // 2. Feed 60 frames of 33.33 ms with 5 injected 40.0 ms frames (at intervals)
    // 40.0ms vs 33.33ms: dev = 6.67ms (tol = max(1.5, 5.0) = 5.0ms, 2*tol = 10.0ms -> grace skip)
    // Candidate count should only increment on 33.33 ms frames!
    int strict_count = 0;
    int stutter_reports = 0;
    bool promoted = false;
    for (int i = 1; i <= 65; ++i) {
        bool is_jitter = (i == 10 || i == 20 || i == 30 || i == 40 || i == 50);
        double dur = is_jitter ? 40.0 : 33.33;
        qpc += stuttometer::ms_to_qpc_delta(dur, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, dur, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (res.is_stutter) {
            ++stutter_reports;
        }
        if (!is_jitter && !promoted) {
            ++strict_count;
            if (strict_count < 60) {
                STUTTO_ASSERT(stats.candidate_count == strict_count);
                double cand_mean = stuttometer::resolve_candidate_mean_ms(stats);
                STUTTO_ASSERT(std::abs(cand_mean - 33.33) < 0.5);
            }
        }
        if (std::abs(stuttometer::calculate_mean_ms(stats) - 33.33) < 0.5) {
            promoted = true;
        }
    }
    // Contract C2 invariant: <= 2 reports total (1 detection + at most 1 reset)
    STUTTO_ASSERT(stutter_reports <= 2);
    STUTTO_ASSERT(promoted);
    std::cout << "  -> Noisy transition grace band preserved mean and promoted (stutter reports=" << stutter_reports << " <= 2) PASSED.\n";
}

static void test_hybrid_grace_band_seed_trap_bound() {
    std::cout << "[TEST] Verifying grace band seed trap bound (STALLED after 10 skips)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 144 FPS baseline (6.94ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }

    // 2. Seed with a 40.0 ms jitter frame -> candidate_count = 1, cand_mean = 40.0 ms
    // tol = max(1.5, 6.0) = 6.0 ms, 2*tol = 12.0 ms
    qpc += stuttometer::ms_to_qpc_delta(40.0, qpc_freq);
    stuttometer::evaluate_frame_pacing(
        stats, 40.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(stats.candidate_count == 1);

    // 3. Feed frames at 33.33 ms: dev = |33.33 - 40.0| = 6.67 ms (between 6.0 and 12.0 ms -> grace skip)
    // Frames 1..10 skip; Frame 11 trips consecutive_skips > 10 (STALLED) -> clears candidate!
    for (int i = 1; i <= 10; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(stats.candidate_count == 1);
        STUTTO_ASSERT(stats.candidate_consecutive_skips == i);
    }
    // Frame 11: consecutive_skips reaches 11 > 10 -> STALLED, candidate cleared!
    qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
    stuttometer::evaluate_frame_pacing(
        stats, 33.33, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(stats.candidate_count == 0);

    // Frame 12: cleanly seeds at 33.33 ms!
    qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
    stuttometer::evaluate_frame_pacing(
        stats, 33.33, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(stats.candidate_count == 1);
    STUTTO_ASSERT(std::abs(stuttometer::resolve_candidate_mean_ms(stats) - 33.33) < 0.1);

    std::cout << "  -> Grace band seed trap bound PASSED.\n";
}

static void test_hybrid_clean_frame_preserves_active_candidate() {
    std::cout << "[TEST] Verifying clean frame preserves active candidate (30 + 1 clean + 30 -> promote)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }

    // 30 candidate frames at 33.33 ms
    for (int i = 1; i <= 30; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(stats.candidate_count == i);
    }

    // 1 old-baseline clean frame at 6.94 ms
    qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
    auto clean_res = stuttometer::evaluate_frame_pacing(
        stats, 6.94, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!clean_res.is_stutter);
    STUTTO_ASSERT(stats.candidate_count == 30);
    STUTTO_ASSERT(stats.candidate_clean_frames_since_last_match == 1);

    // 30 more candidate frames at 33.33 ms -> completes 60 accumulated frames!
    for (int i = 31; i <= 60; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 33.33) < 0.5);
    std::cout << "  -> Clean frame candidate preservation PASSED.\n";
}

static void test_hybrid_periodic_hitches_not_silenced() {
    std::cout << "[TEST] Verifying periodic hitches are not silenced across varying gaps (60 FPS & 30 FPS)...\n";
    const uint64_t qpc_freq = 10000000ULL;

    // Part A: 60 FPS (16.67 ms baseline, 40.0 ms hitches) across gaps of 30, 60, 90, and 120 frames
    const std::vector<int> gaps_60fps = {30, 60, 90, 120};
    for (int gap : gaps_60fps) {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);

        uint64_t qpc = 1000000ULL;
        for (int i = 0; i < 64; ++i) {
            qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
            stuttometer::evaluate_frame_pacing(
                stats, 16.67, qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        }

        for (int cycle = 0; cycle < 4; ++cycle) {
            // Hitch frame: 40.0 ms
            qpc += stuttometer::ms_to_qpc_delta(40.0, qpc_freq);
            auto res = stuttometer::evaluate_frame_pacing(
                stats, 40.0, qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
            STUTTO_ASSERT(res.is_stutter);

            // Clean frames: gap frames at 16.67 ms
            for (int f = 0; f < gap; ++f) {
                qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
                auto clean_res = stuttometer::evaluate_frame_pacing(
                    stats, 16.67, qpc, qpc_freq,
                    stuttometer::FrameTriggerMode::HYBRID,
                    stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                    stuttometer::PacingProfile::AUTO_ADAPTIVE
                );
                STUTTO_ASSERT(!clean_res.is_stutter);
            }
            // Candidate cleared because gap >= 30 > 10 MAX_CLEAN_FRAMES_BEFORE_STALE_CLEAR
            STUTTO_ASSERT(stats.candidate_count == 0);
        }
    }

    // Part B: 30 FPS case (baseline 33.33 ms, 70.0 ms hitches) with 60-frame gaps
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);

        uint64_t qpc = 1000000ULL;
        for (int i = 0; i < 64; ++i) {
            qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
            stuttometer::evaluate_frame_pacing(
                stats, 33.33, qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        }

        for (int cycle = 0; cycle < 4; ++cycle) {
            // Hitch frame: 70.0 ms
            qpc += stuttometer::ms_to_qpc_delta(70.0, qpc_freq);
            auto res = stuttometer::evaluate_frame_pacing(
                stats, 70.0, qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
            STUTTO_ASSERT(res.is_stutter);

            // 60 clean frames at 33.33 ms
            for (int f = 0; f < 60; ++f) {
                qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
                auto clean_res = stuttometer::evaluate_frame_pacing(
                    stats, 33.33, qpc, qpc_freq,
                    stuttometer::FrameTriggerMode::HYBRID,
                    stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 50.0,
                    stuttometer::PacingProfile::AUTO_ADAPTIVE
                );
                STUTTO_ASSERT(!clean_res.is_stutter);
            }
            // Candidate cleared after 10 clean frames (60 > 10)
            STUTTO_ASSERT(stats.candidate_count == 0);
        }
    }
    std::cout << "  -> Periodic hitches not silenced PASSED.\n";
}

static void test_one_off_spike_no_cascade() {
    std::cout << "[TEST] Verifying one-off spike does not cascade (candidate cleared after 10 clean frames)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        stuttometer::evaluate_frame_pacing(
            stats, 16.67, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
    }

    int stutter_count = 0;
    // 1 spike frame at 45.0 ms
    qpc += stuttometer::ms_to_qpc_delta(45.0, qpc_freq);
    auto res_spike1 = stuttometer::evaluate_frame_pacing(
        stats, 45.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    if (res_spike1.is_stutter) ++stutter_count;
    STUTTO_ASSERT(res_spike1.is_stutter);
    STUTTO_ASSERT(stutter_count == 1);
    STUTTO_ASSERT(stats.candidate_count == 1);

    // 60 clean frames at 16.67 ms
    for (int f = 0; f < 60; ++f) {
        qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        auto res_clean = stuttometer::evaluate_frame_pacing(
            stats, 16.67, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (res_clean.is_stutter) ++stutter_count;
    }
    STUTTO_ASSERT(stutter_count == 1);
    // Candidate accumulator cleared after 10 clean frames (D6a threshold = 10)
    STUTTO_ASSERT(stats.candidate_count == 0);

    // Inject an identical spike at 45.0 ms 60 frames later
    qpc += stuttometer::ms_to_qpc_delta(45.0, qpc_freq);
    auto res_spike2 = stuttometer::evaluate_frame_pacing(
        stats, 45.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    if (res_spike2.is_stutter) ++stutter_count;
    STUTTO_ASSERT(res_spike2.is_stutter);
    STUTTO_ASSERT(stutter_count == 2);
    STUTTO_ASSERT(stats.candidate_count == 1);

    std::cout << "  -> One-off spike no cascade PASSED.\n";
}

static void test_hybrid_multistage_descending_transition() {
    std::cout << "[TEST] Verifying multi-stage descending transition (60 -> 45 -> 30 -> 20 FPS)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    int stutter_reports = 0;

    // 20 frames at 16.67 ms (baseline)
    for (int i = 0; i < 20; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.67, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        if (res.is_stutter) ++stutter_reports;
    }

    // 15 frames at 22.2 ms (45 FPS)
    for (int i = 0; i < 15; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(22.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 22.2, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        if (res.is_stutter) ++stutter_reports;
    }

    // 15 frames at 33.33 ms (30 FPS)
    for (int i = 0; i < 15; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        if (res.is_stutter) ++stutter_reports;
    }

    // Feed frames at 50.0 ms (20 FPS): 1 boundary reset frame + 60 accumulation frames -> promotes by frame 61!
    for (int i = 0; i < 65; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 50.0, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        if (res.is_stutter) ++stutter_reports;
    }

    // Contract C9 invariant: <= 3 reports total
    STUTTO_ASSERT(stutter_reports <= 3);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 50.0) < 0.5);
    std::cout << "  -> Multi-stage descending transition (stutter reports=" << stutter_reports << " <= 3) PASSED.\n";
}

static void test_pause_reset_clears_candidate_accumulation() {
    std::cout << "[TEST] Verifying pause reset clears in-progress candidate accumulation...\n";

    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t current_qpc = 1000000ULL;

    // Establish 173 FPS baseline (5.78ms)
    for (int i = 0; i < 64; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 5.78, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);

    // Feed 30 candidate frames at 10.2ms
    for (int i = 1; i <= 30; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 10.2, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (i == 1) {
            STUTTO_ASSERT(res.is_stutter);
        } else {
            STUTTO_ASSERT(!res.is_stutter);
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
        }
        STUTTO_ASSERT(stats.candidate_count == i);
    }
    STUTTO_ASSERT(stats.candidate_count == 30);
    STUTTO_ASSERT(stats.candidate_sum_us > 0);
    STUTTO_ASSERT(stats.candidate_first_qpc > 0);

    // 3-second pause (exceeds 2.0s pause ceiling)
    current_qpc += stuttometer::ms_to_qpc_delta(3000.0, qpc_freq);
    auto pause_res = stuttometer::evaluate_frame_pacing(
        stats, 16.67, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::AUTO_ADAPTIVE
    );
    STUTTO_ASSERT(!pause_res.is_stutter);
    // Candidate fields must be cleanly cleared
    STUTTO_ASSERT(stats.candidate_count == 0);
    STUTTO_ASSERT(stats.candidate_sum_us == 0);
    STUTTO_ASSERT(stats.candidate_sum_sq_us == 0);
    STUTTO_ASSERT(stats.candidate_first_qpc == 0);
    STUTTO_ASSERT(stats.sample_count == 1);

    std::cout << "  -> Pause reset candidate clearing PASSED.\n";
}

static void test_candidate_jitter_tolerance_and_rejection() {
    std::cout << "[TEST] Verifying candidate micro-jitter tolerance and rejection boundaries...\n";

    const uint64_t qpc_freq = 10000000ULL;

    // Case A: Jitter within consistency tolerance (cand_mean ~ 10.0ms, tolerance = max(1.5, 1.5) = 1.5ms)
    // Values alternating between 9.5ms and 10.5ms (|delta| = 0.5ms <= 1.5ms, stddev ~ 0.5ms < 1.0ms)
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);
        uint64_t current_qpc = 1000000ULL;

        // Establish 173 FPS baseline (5.78ms)
        for (int i = 0; i < 64; ++i) {
            current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
            stuttometer::evaluate_frame_pacing(
                stats, 5.78, current_qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        }

        // Feed 60 frames alternating 9.5ms and 10.5ms
        for (int i = 1; i <= 60; ++i) {
            const double dur = (i % 2 == 1) ? 9.5 : 10.5;
            current_qpc += stuttometer::ms_to_qpc_delta(dur, qpc_freq);
            auto res = stuttometer::evaluate_frame_pacing(
                stats, dur, current_qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::CUSTOM
            );
            if (i == 1) {
                STUTTO_ASSERT(res.is_stutter);
            } else {
                STUTTO_ASSERT(!res.is_stutter);
                STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
            }
            if (i < 60) {
                STUTTO_ASSERT(stats.candidate_count == i);
            } else {
                // Promoted at frame 60!
                STUTTO_ASSERT(stats.candidate_count == 0);
                STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 10.0) < 0.2);
                STUTTO_ASSERT(res.spike_ratio == 1.0);
            }
        }
    }

    // Case B: Jitter exceeding consistency tolerance (e.g. 10.0ms vs 12.0ms: delta = 2.0ms > 1.5ms)
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);
        uint64_t current_qpc = 1000000ULL;

        // Establish 173 FPS baseline (5.78ms)
        for (int i = 0; i < 64; ++i) {
            current_qpc += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
            stuttometer::evaluate_frame_pacing(
                stats, 5.78, current_qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        }

        // Frame 1: 10.0ms -> candidate_count = 1
        current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
        auto r1 = stuttometer::evaluate_frame_pacing(
            stats, 10.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(r1.is_stutter);
        STUTTO_ASSERT(stats.candidate_count == 1);

        // Frame 2: 12.0ms -> |12.0 - 10.0| = 2.0ms <= 2 * tol (3.0ms) -> grace skip!
        // candidate_count remains 1, consecutive_skips = 1, stutter is suppressed under Phase 6
        current_qpc += stuttometer::ms_to_qpc_delta(12.0, qpc_freq);
        auto r2 = stuttometer::evaluate_frame_pacing(
            stats, 12.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(!r2.is_stutter);
        STUTTO_ASSERT(r2.reason == stuttometer::TriggerReason::NONE);
        STUTTO_ASSERT(stats.candidate_count == 1);
        STUTTO_ASSERT(stats.candidate_consecutive_skips == 1);

        // Frame 3: 14.0ms -> |14.0 - 10.0| = 4.0ms > 2 * tol (3.0ms) -> catastrophic reset, candidate resets to 0!
        current_qpc += stuttometer::ms_to_qpc_delta(14.0, qpc_freq);
        auto r3 = stuttometer::evaluate_frame_pacing(
            stats, 14.0, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::HIGH_REFRESH_SPIKE_MULTIPLIER, stuttometer::HIGH_REFRESH_MIN_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(r3.is_stutter);
        STUTTO_ASSERT(stats.candidate_count == 0);
    }

    std::cout << "  -> Candidate micro-jitter tolerance and rejection PASSED.\n";
}

static void test_independent_stream_candidate_isolation() {
    std::cout << "[TEST] Verifying multi-stream candidate isolation in FixedInFlightTable...\n";

    stuttometer::FixedInFlightTable<stuttometer::RollingFrameStats, 256> table;
    const uint64_t stream_a = 0xAAAA0001ULL;
    const uint64_t stream_b = 0xBBBB0002ULL;

    const uint64_t qpc_freq = 10000000ULL;
    uint64_t qpc_a = 1000000ULL;
    uint64_t qpc_b = 1000000ULL;

    stuttometer::RollingFrameStats init_stats{};

    // Initialize Stream A (173 FPS) and Stream B (60 FPS)
    for (int i = 0; i < 64; ++i) {
        qpc_a += stuttometer::ms_to_qpc_delta(5.78, qpc_freq);
        table.upsert(stream_a, init_stats, [&](stuttometer::RollingFrameStats& s) {
            stuttometer::evaluate_frame_pacing(
                s, 5.78, qpc_a, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        });

        qpc_b += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        table.upsert(stream_b, init_stats, [&](stuttometer::RollingFrameStats& s) {
            stuttometer::evaluate_frame_pacing(
                s, 16.666, qpc_b, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
        });
    }

    // Interleave 60 frames: Stream A shifts to 10.2ms (accumulates and promotes), Stream B stays clean at 16.666ms
    for (int i = 1; i <= 60; ++i) {
        qpc_a += stuttometer::ms_to_qpc_delta(10.2, qpc_freq);
        table.upsert(stream_a, init_stats, [&](stuttometer::RollingFrameStats& s) {
            auto res_a = stuttometer::evaluate_frame_pacing(
                s, 10.2, qpc_a, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
            if (i == 1) {
                STUTTO_ASSERT(res_a.is_stutter);
            } else {
                STUTTO_ASSERT(!res_a.is_stutter);
                STUTTO_ASSERT(res_a.reason == stuttometer::TriggerReason::NONE);
            }
            if (i < 60) {
                STUTTO_ASSERT(s.candidate_count == i);
            } else {
                STUTTO_ASSERT(s.candidate_count == 0);
                STUTTO_ASSERT(std::abs(res_a.baseline_avg_ms - 10.2) < 0.05);
            }
        });

        qpc_b += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        table.upsert(stream_b, init_stats, [&](stuttometer::RollingFrameStats& s) {
            auto res_b = stuttometer::evaluate_frame_pacing(
                s, 16.666, qpc_b, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
            STUTTO_ASSERT(!res_b.is_stutter);
            STUTTO_ASSERT(s.candidate_count == 0);
            STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(s) - 16.666) < 0.05);
        });
    }

    std::cout << "  -> Multi-stream candidate isolation PASSED.\n";
}

static void test_scene_transition_reset_2s_boundary() {
    std::cout << "[TEST] Verifying 2.0s scene transition boundary (2.5s reset vs 1.5s evaluation)...\n";
    const uint64_t qpc_freq = 10000000ULL;

    // 2.5s Reset Case
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);
        uint64_t current_qpc = 1000;
        for (int i = 0; i < 20; ++i) {
            current_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
            stuttometer::push_clean_frame(stats, 16666, current_qpc);
        }
        STUTTO_ASSERT(stats.sample_count == 20);

        // Exercises the pacing tracker's internal ceiling as a defensive backstop (in production, the DXGI/kernel handlers filter the pause frame upstream before reaching this layer).
        current_qpc += stuttometer::ms_to_qpc_delta(2500.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats,
            2500.0,
            current_qpc,
            qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER,
            stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
            true,
            stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
            25.0
        );

        STUTTO_ASSERT(res.is_stutter == false);
        STUTTO_ASSERT(stats.sample_count == 1);
        STUTTO_ASSERT(stats.candidate_count == 0);

        // Subsequent gameplay frame (16.67ms) resumes clean warmup without false stutter
        current_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        auto res_post = stuttometer::evaluate_frame_pacing(
            stats,
            16.67,
            current_qpc,
            qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER,
            stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
            true,
            stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
            25.0
        );
        STUTTO_ASSERT(res_post.is_stutter == false);
        STUTTO_ASSERT(stats.sample_count == 2);
    }

    // Exact 2.000s Reset Case (delta_us >= PAUSE_CEILING_US triggers pause reset)
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);
        uint64_t current_qpc = 1000;
        for (int i = 0; i < 20; ++i) {
            current_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
            stuttometer::push_clean_frame(stats, 16666, current_qpc);
        }
        STUTTO_ASSERT(stats.sample_count == 20);

        current_qpc += stuttometer::ms_to_qpc_delta(2000.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats,
            2000.0,
            current_qpc,
            qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER,
            stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
            true,
            stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
            25.0
        );

        STUTTO_ASSERT(res.is_stutter == false);
        STUTTO_ASSERT(stats.sample_count == 1);
        STUTTO_ASSERT(stats.candidate_count == 0);
    }

    // 1.5s Evaluation Case
    {
        stuttometer::RollingFrameStats stats{};
        stuttometer::reset_frame_stats(stats, 1000);
        uint64_t current_qpc = 1000;
        for (int i = 0; i < 20; ++i) {
            current_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
            stuttometer::push_clean_frame(stats, 16666, current_qpc);
        }
        STUTTO_ASSERT(stats.sample_count == 20);

        current_qpc += stuttometer::ms_to_qpc_delta(1500.0, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats,
            1500.0,
            current_qpc,
            qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER,
            stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS,
            true,
            stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO,
            25.0
        );

        STUTTO_ASSERT(res.is_stutter == true);
        STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);
    }

    std::cout << "  -> 2.0s scene transition boundary test PASSED.\n";
}

static void test_hybrid_transition_report_count() {
    std::cout << "[TEST] Verifying 144 FPS -> 30 FPS transition reports at most 1 stutter event under Phase 6 suppression...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 144 FPS baseline (64 frames of 6.94ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 6.94, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 6.94) < 0.05);

    // 2. Feed 70 frames at 33.33 ms (30 FPS step transition).
    // Count every stutter report across the transition window.
    int stutter_report_count = 0;
    for (int i = 1; i <= 70; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(33.33, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 33.33, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 5.5,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (res.is_stutter) {
            ++stutter_report_count;
        }
    }

    // Phase 6 invariant: Exactly 1 report at seed frame, remaining frames suppressed during accumulation and promotion.
    STUTTO_ASSERT(stutter_report_count <= 1);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 33.33) < 0.5);
    std::cout << "  -> Transition produced " << stutter_report_count << " stutter report(s) (<= 1 invariant satisfied) PASSED.\n";
}

static void test_hybrid_static_fallback_cadence_routing() {
    std::cout << "[TEST] Verifying HYBRID static fallback cadence candidate routing (60 -> 45 FPS)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 60 FPS baseline (64 frames of 16.666ms) with ceiling at 20.67 ms
    const double static_threshold = 20.67;
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.666, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.666) < 0.05);

    // 2. Feed 60 frames at 22.2ms (45 FPS):
    // 22.2 / 16.666 = 1.332 < 2.0 (does NOT trip dynamic relative spike)
    // 22.2 >= 20.67 (trips static fallback threshold!)
    // In HYBRID mode, static fallback routes through process_cadence_candidate().
    int static_reports = 0;
    for (int i = 1; i <= 60; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(22.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 22.2, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
            stuttometer::PacingProfile::CUSTOM
        );
        if (res.is_stutter) {
            ++static_reports;
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
        }
        if (i < 60) {
            STUTTO_ASSERT(stats.candidate_count == i);
        } else {
            // Frame 60: promotion executed!
            STUTTO_ASSERT(stats.candidate_count == 0);
            STUTTO_ASSERT(std::abs(res.baseline_avg_ms - 22.2) < 0.1);
        }
    }
    // Exactly 1 report at frame 1 (SEEDED), frames 2..59 ACCUMULATED (suppressed), frame 60 PROMOTED (suppressed)
    STUTTO_ASSERT(static_reports == 1);
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 22.2) < 0.1);

    // 3. Subsequent 10 frames at 22.2ms are evaluated against newly promoted baseline -> zero reports
    for (int i = 0; i < 10; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(22.2, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 22.2, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, 10.0, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, static_threshold,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(!res.is_stutter);
        STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::NONE);
    }

    std::cout << "  -> Static fallback cadence routing and promotion PASSED.\n";
}

static void test_hybrid_candidate_staleness_sweep_10s() {
    std::cout << "[TEST] Verifying D6b 10.0s candidate staleness sweep (isolated from D6a)...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 60 FPS baseline (16.67 ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.67, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);

    // 2. Isolate D6b from D6a using 8 cycles of 1.5 s synthetic QPC advancement
    // (12.0 s total elapsed time from candidate_first_qpc) with 5 clean frames
    // at 16.67 ms per cycle (5 <= 10, keeping D6a completely silent throughout).
    // On cycle 8 (elapsed > 10.0 s), D6b's 10.0 s staleness sweep fires and clears
    // the candidate (stats.candidate_count drops to 0 specifically due to D6b).
    int hitches_observed = 0;
    bool staleness_sweep_observed = false;

    for (int cycle = 1; cycle <= 8; ++cycle) {
        // 5 clean frames at 16.67 ms (5 <= 10, keeping D6a completely silent)
        for (int f = 0; f < 5; ++f) {
            qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
            auto res_clean = stuttometer::evaluate_frame_pacing(
                stats, 16.67, qpc, qpc_freq,
                stuttometer::FrameTriggerMode::HYBRID,
                stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
                stuttometer::PacingProfile::AUTO_ADAPTIVE
            );
            STUTTO_ASSERT(!res_clean.is_stutter);
        }

        // On cycle 8, elapsed time from candidate_first_qpc exceeds 10.0 s (~10.6 s),
        // so D6b fires on the clean frames and drops candidate_count to 0.
        if (cycle == 8) {
            STUTTO_ASSERT(stats.candidate_count == 0);
            staleness_sweep_observed = true;
        }

        // Hitch frame: 40.0 ms
        qpc += stuttometer::ms_to_qpc_delta(40.0, qpc_freq);
        auto res_hitch = stuttometer::evaluate_frame_pacing(
            stats, 40.0, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::AUTO_ADAPTIVE
        );
        if (res_hitch.is_stutter) {
            ++hitches_observed;
        }
        if (cycle == 1 || cycle == 8) {
            // First seed on cycle 1, and reseed on cycle 8 after D6b staleness sweep
            STUTTO_ASSERT(res_hitch.is_stutter);
            STUTTO_ASSERT(stats.candidate_count == 1);
        } else {
            // Cycles 2..7 accumulate into candidate; stutter suppressed per Bug B (D3)
            STUTTO_ASSERT(!res_hitch.is_stutter);
            STUTTO_ASSERT(stats.candidate_count == cycle);
        }

        // Advance QPC by 1.5 s synthetically per cycle (1.5s < 2.0s PAUSE_CEILING_US)
        qpc += stuttometer::ms_to_qpc_delta(1500.0, qpc_freq);
    }

    STUTTO_ASSERT(staleness_sweep_observed);
    STUTTO_ASSERT(hitches_observed == 2);
    // Baseline never falsely promoted to 40 ms
    STUTTO_ASSERT(std::abs(stuttometer::calculate_mean_ms(stats) - 16.67) < 0.1);
    std::cout << "  -> D6b 10.0s staleness sweep and periodic protection PASSED.\n";
}

static void test_hybrid_candidate_1000_sample_reseed_policy() {
    std::cout << "[TEST] Verifying D8 1000-sample cap clear-and-reseed policy...\n";
    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t qpc = 1000000ULL;
    // 1. Establish 60 FPS baseline (16.666 ms)
    for (int i = 0; i < 64; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, 16.666, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.sample_count == 64);

    // 2. Feed frames around 150.0 ms oscillating by +/- 6.0 ms (144.0 ms and 156.0 ms):
    // Mean = 150.0 ms, tol = max(1.5, 150 * 0.15) = 22.5 ms.
    // dev = 6.0 ms << 22.5 ms tol -> 100% strict matches (zero grace skips).
    // stddev = 6.0 ms >= sigma_threshold (5.0 ms) -> promotion gate fails, candidate accumulates continuously.
    // Frames 1..60 arrive in 9.0s (< 10.0s D6b staleness sweep), disarming D6b once count reaches 60.

    // Frame 1: seeds candidate (count = 1, outcome SEEDED -> is_stutter == true)
    qpc += stuttometer::ms_to_qpc_delta(150.0, qpc_freq);
    auto res_seed = stuttometer::evaluate_frame_pacing(
        stats, 150.0, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::CUSTOM
    );
    STUTTO_ASSERT(res_seed.is_stutter);
    STUTTO_ASSERT(stats.candidate_count == 1);

    // Frames 2..1000: accumulate without promotion (outcome ACCUMULATED -> is_stutter == false)
    for (int i = 2; i <= 1000; ++i) {
        const double dur = (i % 2 == 0) ? 144.0 : 156.0;
        qpc += stuttometer::ms_to_qpc_delta(dur, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, dur, qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
            stuttometer::PacingProfile::CUSTOM
        );
        STUTTO_ASSERT(!res.is_stutter);
        STUTTO_ASSERT(stats.candidate_count == i);
    }
    STUTTO_ASSERT(stats.candidate_count == 1000);

    // Frame 1001: reaches candidate_count >= 1000 cap!
    // Must clear and reseed: candidate_count = 1, outcome = SEEDED -> is_stutter == true!
    const double dur_reseed = 150.0;
    qpc += stuttometer::ms_to_qpc_delta(dur_reseed, qpc_freq);
    auto res_reseed = stuttometer::evaluate_frame_pacing(
        stats, dur_reseed, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::CUSTOM
    );
    STUTTO_ASSERT(res_reseed.is_stutter);
    STUTTO_ASSERT(res_reseed.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);
    STUTTO_ASSERT(stats.candidate_count == 1);
    STUTTO_ASSERT(stats.candidate_sum_us == static_cast<uint64_t>(dur_reseed * 1000.0));
    STUTTO_ASSERT(stats.candidate_consecutive_skips == 0);

    // Frame 1002: cleanly accumulates past reseed
    const double dur_post = 150.0;
    qpc += stuttometer::ms_to_qpc_delta(dur_post, qpc_freq);
    auto res_post = stuttometer::evaluate_frame_pacing(
        stats, dur_post, qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        stuttometer::DEFAULT_SPIKE_MULTIPLIER, stuttometer::DEFAULT_MIN_SPIKE_DELTA_MS, true, stuttometer::pacing_tuning::DEFAULT_JUDDER_SWING_RATIO, 25.0,
        stuttometer::PacingProfile::CUSTOM
    );
    STUTTO_ASSERT(!res_post.is_stutter);
    STUTTO_ASSERT(stats.candidate_count == 2);

    std::cout << "  -> D8 1000-sample cap clear-and-reseed policy PASSED.\n";
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
        test_hybrid_warmup_100fps_on_200hz_no_static_trigger();
        test_adaptive_pacing_math();
        test_adaptive_trigger_140fps();
        test_adaptive_trigger_60fps();
        test_invert_effective_static_threshold();
        test_filetime_domain_timestamp_does_not_block_poll_state();
        test_cadence_adaptation_173fps_to_100fps();
        test_drs_flapping_resistance();
        test_rolling_frame_stats_alignment_and_cache_lines();
        test_cadence_adaptation_clean_frame_interleaving();
        test_candidate_reset_then_reaccumulate_promotes();
        test_hybrid_144fps_to_30fps_promotes_cleanly();
        test_hybrid_candidate_accumulation_progresses_past_seed();
        test_dynamic_mode_144fps_to_30fps_promotes();
        test_hybrid_low_framerate_sigma_relative();
        test_hybrid_noisy_transition_grace_band_preserves_mean();
        test_hybrid_grace_band_seed_trap_bound();
        test_hybrid_clean_frame_preserves_active_candidate();
        test_hybrid_periodic_hitches_not_silenced();
        test_one_off_spike_no_cascade();
        test_hybrid_multistage_descending_transition();
        test_pause_reset_clears_candidate_accumulation();
        test_candidate_jitter_tolerance_and_rejection();
        test_independent_stream_candidate_isolation();
        test_scene_transition_reset_2s_boundary();
        test_hybrid_transition_report_count();
        test_hybrid_static_fallback_cadence_routing();
        test_hybrid_candidate_staleness_sweep_10s();
        test_hybrid_candidate_1000_sample_reseed_policy();

        std::cout << "\n>>> ALL 43 FRAME PACING UNIT TESTS PASSED SUCCESSFULLY! <<<\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}

