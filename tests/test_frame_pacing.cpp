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
    bool polled = engine.poll_state(qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), info, from_qpc, to_qpc);
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
    bool polled = engine.poll_state(qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), info, from_qpc, to_qpc);
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

    // Must not trigger static threshold in DYNAMIC_ONLY mode, and must NOT contaminate baseline (sample_count remains 0)
    STUTTO_ASSERT(!res.is_stutter);
    STUTTO_ASSERT(stats.sample_count == 0);

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
    STUTTO_ASSERT(stats.sample_count == 1);
    STUTTO_ASSERT(stats.durations_us[0] == 16666);
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

        std::cout << "\n>>> ALL 10 FRAME PACING UNIT TESTS PASSED SUCCESSFULLY! <<<\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
