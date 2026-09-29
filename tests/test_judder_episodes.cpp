#include "test_common.hpp"
#include "stuttometer/frame_pacing_tracker.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <vector>
#include <cmath>

static void test_judder_close_on_500ms_gap() {
    std::cout << "[TEST] Validating judder episode close on 500ms gap...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t current_qpc = 10000000ULL;

    // Warmup 20 frames at 16.666ms baseline
    for (int i = 0; i < 20; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        stuttometer::push_clean_frame(stats, 16666, current_qpc);
    }
    STUTTO_ASSERT(stats.sample_count == 20);

    // Feed alternating pattern: 23ms, 10ms, 23ms, 10ms, 23ms, 10ms
    // Frame 1 (23ms): first swing (+6.33ms) -> alt 0
    // Frames 2..6: alt 1, 2, 3, 4, 5
    const double durs[6] = {23.0, 10.0, 23.0, 10.0, 23.0, 10.0};
    uint64_t last_alt_qpc = 0;

    for (int i = 0; i < 6; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(durs[i], qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, durs[i], current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.5, 10.0, true, 0.35, 50.0,
            stuttometer::PacingProfile::CUSTOM, 5
        );
        STUTTO_ASSERT(!res.is_stutter); // No stutter emitted while episode is actively accumulating
        if (i > 0) {
            STUTTO_ASSERT(stats.judder_episode_active == 1);
            STUTTO_ASSERT(stats.judder_episode_alternations == i);
            last_alt_qpc = current_qpc;
        }
    }

    STUTTO_ASSERT(stats.judder_episode_alternations == 5);
    STUTTO_ASSERT(stats.judder_episode_last_alt_qpc == last_alt_qpc);

    // Frame with gap < 500ms (e.g. 50ms) and non-alternating (10.0ms -> same sign)
    // Must NOT close the episode yet
    current_qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    auto res_subgap = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );
    STUTTO_ASSERT(!res_subgap.is_stutter);
    STUTTO_ASSERT(stats.judder_episode_active == 1); // Still open!

    // Now introduce gap >= 500ms from last alternation (e.g. 505ms) with non-alternating frame
    current_qpc = last_alt_qpc + stuttometer::ms_to_qpc_delta(505.0, qpc_freq);
    auto res_close = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.5, 10.0, true, 0.35, 50.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );

    STUTTO_ASSERT(res_close.is_stutter);
    STUTTO_ASSERT(res_close.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(res_close.judder_alternations == 5);
    STUTTO_ASSERT(res_close.trigger_timestamp_qpc == last_alt_qpc);
    STUTTO_ASSERT(stats.judder_episode_active == 0);
    STUTTO_ASSERT(stats.judder_episode_alternations == 0);

    std::cout << "  -> Judder episode close on 500ms gap PASSED.\n";
}

static void test_judder_close_on_5s_cap() {
    std::cout << "[TEST] Validating judder episode close on 5000ms hard cap...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t current_qpc = 10000000ULL;

    // Warmup 20 frames at 16.666ms baseline
    for (int i = 0; i < 20; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        stuttometer::push_clean_frame(stats, 16666, current_qpc);
    }

    // Deliver alternating frames continuously until 5000ms duration is reached
    // Each pair is 23ms + 10ms = 33ms. ~152 pairs = ~304 frames = 5000ms.
    bool cap_fired = false;

    for (int i = 0; i < 400; ++i) {
        const double d = (i % 2 == 0) ? 23.0 : 10.0;
        current_qpc += stuttometer::ms_to_qpc_delta(d, qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, d, current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.5, 10.0, true, 0.35, 50.0,
            stuttometer::PacingProfile::CUSTOM, 5
        );

        if (res.is_stutter) {
            STUTTO_ASSERT(res.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
            STUTTO_ASSERT(res.duration_ms >= 5000.0);
            STUTTO_ASSERT(res.judder_alternations >= 5);
            STUTTO_ASSERT(stats.judder_episode_active == 0);
            cap_fired = true;
            break;
        }
    }

    STUTTO_ASSERT(cap_fired);
    std::cout << "  -> Judder episode close on 5000ms cap PASSED.\n";
}

static void test_judder_mid_episode_spike_bridge() {
    std::cout << "[TEST] Validating mid-episode spike bridge across RELATIVE_SPIKE...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::RollingFrameStats stats{};
    stuttometer::reset_frame_stats(stats, 1000);

    uint64_t current_qpc = 10000000ULL;

    // Warmup 20 frames at 16.666ms baseline
    for (int i = 0; i < 20; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        stuttometer::push_clean_frame(stats, 16666, current_qpc);
    }

    // 1. Deliver 3 alternations (23ms, 10ms, 23ms, 10ms)
    // Frame 1 (23ms): alt 0
    // Frame 2 (10ms): alt 1
    // Frame 3 (23ms): alt 2
    // Frame 4 (10ms): alt 3
    const double pattern[4] = {23.0, 10.0, 23.0, 10.0};
    for (int i = 0; i < 4; ++i) {
        current_qpc += stuttometer::ms_to_qpc_delta(pattern[i], qpc_freq);
        auto res = stuttometer::evaluate_frame_pacing(
            stats, pattern[i], current_qpc, qpc_freq,
            stuttometer::FrameTriggerMode::HYBRID,
            2.0, 10.0, true, 0.35, 500.0,
            stuttometer::PacingProfile::CUSTOM, 5
        );
        STUTTO_ASSERT(!res.is_stutter);
    }
    STUTTO_ASSERT(stats.judder_episode_active == 1);
    STUTTO_ASSERT(stats.judder_episode_alternations == 3);

    // 2. Deliver a 150ms relative spike (150ms / ~16.66ms baseline = 9x >= 2.0x spike multiplier)
    current_qpc += stuttometer::ms_to_qpc_delta(150.0, qpc_freq);
    auto res_spike = stuttometer::evaluate_frame_pacing(
        stats, 150.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 10.0, true, 0.35, 500.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );
    STUTTO_ASSERT(res_spike.is_stutter);
    STUTTO_ASSERT(res_spike.reason == stuttometer::TriggerReason::RELATIVE_SPIKE);

    // Invariant: reset_cadence_state preserved judder_episode_active and episode alternations!
    STUTTO_ASSERT(stats.judder_episode_active == 1);
    STUTTO_ASSERT(stats.judder_episode_alternations == 3);

    // 3. Cadence resumes with alternating frame: 23ms (positive swing from baseline, alternates from 10ms which was negative!)
    current_qpc += stuttometer::ms_to_qpc_delta(23.0, qpc_freq);
    auto res_resume1 = stuttometer::evaluate_frame_pacing(
        stats, 23.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 10.0, true, 0.35, 500.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );
    STUTTO_ASSERT(!res_resume1.is_stutter);
    STUTTO_ASSERT(stats.judder_episode_active == 1);
    STUTTO_ASSERT(stats.judder_episode_alternations == 4);

    // Next alternating frame: 10ms (reaches 5 alternations)
    current_qpc += stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
    auto res_resume2 = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 10.0, true, 0.35, 500.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );
    STUTTO_ASSERT(!res_resume2.is_stutter);
    STUTTO_ASSERT(stats.judder_episode_active == 1);
    STUTTO_ASSERT(stats.judder_episode_alternations == 5);
    const uint64_t last_alt = current_qpc;

    // 4. Close on 505ms gap with non-alternating frame (10.0ms)
    current_qpc = last_alt + stuttometer::ms_to_qpc_delta(505.0, qpc_freq);
    auto res_end = stuttometer::evaluate_frame_pacing(
        stats, 10.0, current_qpc, qpc_freq,
        stuttometer::FrameTriggerMode::HYBRID,
        2.0, 10.0, true, 0.35, 500.0,
        stuttometer::PacingProfile::CUSTOM, 5
    );
    STUTTO_ASSERT(res_end.is_stutter);
    STUTTO_ASSERT(res_end.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(res_end.judder_alternations == 5);
    STUTTO_ASSERT(res_end.trigger_timestamp_qpc == last_alt);
    STUTTO_ASSERT(stats.judder_episode_active == 0);

    std::cout << "  -> Mid-episode spike bridge across RELATIVE_SPIKE PASSED.\n";
}

static void test_correlator_timestamp_anchor_verification() {
    std::cout << "[TEST] Validating end-to-end correlator timestamp anchor to last alternation...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 50.0;
    config.window_post_ms = 30.0;
    config.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    config.enable_judder_detection = true;
    config.judder_swing_ratio = 0.35;
    config.judder_min_alternations = 5;
    config.min_report_severity = stuttometer::ReportSeverity::ALL;

    stuttometer::TriggerEngine engine(config, qpc_freq);
    const uint32_t pid = 9999;
    const uint32_t tid = 1111;
    const uint64_t stream_key = 0xFEEDFACEULL;
    engine.update_target_pid(pid);

    uint64_t qpc = stuttometer::get_current_qpc();

    // Warmup 15 frames at 16.666ms
    for (int i = 0; i < 15; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(16.666, qpc_freq);
        engine.on_dxgi_present(pid, tid, 16.666, qpc, stream_key, 0);
    }

    // Deliver alternating pattern: 23ms, 10ms, 23ms, 10ms, 23ms, 10ms (6 frames -> 5 alternations)
    const double durs[6] = {23.0, 10.0, 23.0, 10.0, 23.0, 10.0};
    uint64_t expected_last_alt_qpc = 0;
    for (int i = 0; i < 6; ++i) {
        qpc += stuttometer::ms_to_qpc_delta(durs[i], qpc_freq);
        bool fired = engine.on_dxgi_present(pid, tid, durs[i], qpc, stream_key, 0);
        STUTTO_ASSERT(!fired);
        expected_last_alt_qpc = qpc;
    }

    // Now introduce 505ms gap with non-alternating frame (10.0ms)
    qpc += stuttometer::ms_to_qpc_delta(505.0, qpc_freq);
    const uint64_t gap_frame_qpc = qpc;
    bool fired = engine.on_dxgi_present(pid, tid, 10.0, gap_frame_qpc, stream_key, 0);
    STUTTO_ASSERT(fired);

    // Poll trigger
    stuttometer::TriggerInfo info{};
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = stuttometer::get_current_qpc() + stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc, info, from_qpc, to_qpc));
    engine.on_report_completed(poll_qpc);

    // Invariant: The trigger timestamp must be anchored to the last alternation, NOT the gap frame!
    STUTTO_ASSERT(info.reason == stuttometer::TriggerReason::CADENCE_JUDDER);
    STUTTO_ASSERT(info.trigger_timestamp_qpc == expected_last_alt_qpc);
    STUTTO_ASSERT(info.trigger_timestamp_qpc != gap_frame_qpc);
    STUTTO_ASSERT(info.judder_alternations == 5);

    std::cout << "  -> End-to-end correlator timestamp anchor PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Judder Episode Tracker Tests ===\n";
    try {
        test_judder_close_on_500ms_gap();
        test_judder_close_on_5s_cap();
        test_judder_mid_episode_spike_bridge();
        test_correlator_timestamp_anchor_verification();
        std::cout << ">>> All Judder Episode tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
