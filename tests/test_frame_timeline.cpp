#include "test_common.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <vector>

// Helper to generate DXGI Present Stop records around a trigger timestamp
static std::vector<stuttometer::EtwEventRecord> generate_frames(
    uint32_t target_pid,
    uint64_t trigger_qpc,
    uint64_t qpc_freq,
    size_t pre_count,
    size_t post_count // includes the anchor at trigger_qpc
) {
    std::vector<stuttometer::EtwEventRecord> records;
    records.reserve(pre_count + post_count);

    // Assume 16.67ms per frame
    const uint64_t frame_interval_ticks = stuttometer::ms_to_qpc_delta(16.67, qpc_freq);

    // Pre-frames: strictly preceding trigger_qpc
    for (size_t i = pre_count; i > 0; --i) {
        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
        rec.event_id = 43;
        rec.pid = target_pid;
        rec.tid = 5000;
        rec.duration_us = 16670;
        rec.qpc_timestamp = trigger_qpc - (i * frame_interval_ticks);
        records.push_back(rec);
    }

    // Anchor frame at trigger_qpc
    if (post_count > 0) {
        stuttometer::EtwEventRecord anchor{};
        anchor.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
        anchor.event_id = 43;
        anchor.pid = target_pid;
        anchor.tid = 5000;
        anchor.duration_us = 45000; // Trigger stutter
        anchor.qpc_timestamp = trigger_qpc;
        records.push_back(anchor);
    }

    // Post-frames: strictly following trigger_qpc
    for (size_t i = 1; i < post_count; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
        rec.event_id = 43;
        rec.pid = target_pid;
        rec.tid = 5000;
        rec.duration_us = 16670;
        rec.qpc_timestamp = trigger_qpc + (i * frame_interval_ticks);
        records.push_back(rec);
    }

    return records;
}

static void test_uncapped_timeline() {
    std::cout << "[TEST] Validating Branch 1: Uncapped timeline (< 1024 frames)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t pid = 1234;

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = pid;
    trigger.target_tid = 5000;

    // 200 pre + 500 post = 700 total frames
    auto snapshot = generate_frames(pid, trigger.trigger_timestamp_qpc, qpc_freq, 200, 500);

    stuttometer::CorrelateOptions opts;
    opts.present_threshold_ms = 16.67;
    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts);

    STUTTO_ASSERT(report.frame_timeline.size() == 700);
    // Find anchor point
    bool found_trigger = false;
    for (const auto& pt : report.frame_timeline) {
        if (pt.is_trigger_frame) {
            STUTTO_ASSERT(pt.relative_index == 0);
            STUTTO_ASSERT(pt.is_pacing_stall);
            STUTTO_ASSERT(pt.frame_index == 200);
            found_trigger = true;
            break;
        }
    }
    STUTTO_ASSERT(found_trigger);
    std::cout << "  -> Retained 700/700 frames uncapped. PASSED.\n";
}

static void test_symmetric_capping() {
    std::cout << "[TEST] Validating Branch 2: Symmetric Capping (1000 pre + 1000 post)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t pid = 1234;

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = pid;

    // 1000 pre + 1000 post
    auto snapshot = generate_frames(pid, trigger.trigger_timestamp_qpc, qpc_freq, 1000, 1000);

    stuttometer::CorrelateOptions opts;
    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts);

    STUTTO_ASSERT(report.frame_timeline.size() == 1024);
    // Exactly 512 pre and 512 post (including anchor)
    // Anchor frame should be at index 512
    STUTTO_ASSERT(report.frame_timeline[512].is_trigger_frame);
    STUTTO_ASSERT(report.frame_timeline[512].relative_index == 0);
    STUTTO_ASSERT(report.frame_timeline.front().relative_index == -512);
    STUTTO_ASSERT(report.frame_timeline.back().relative_index == 511);
    std::cout << "  -> Retained 1024 frames symmetrically (512 pre, 512 post). PASSED.\n";
}

static void test_asymmetric_expand_pre() {
    std::cout << "[TEST] Validating Branch 3: Asymmetric Expand Pre (1500 pre + 100 post)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t pid = 1234;

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = pid;

    // 1500 pre + 100 post -> take_post = 100, take_pre = min(1500, 1024 - 100) = 924
    auto snapshot = generate_frames(pid, trigger.trigger_timestamp_qpc, qpc_freq, 1500, 100);

    stuttometer::CorrelateOptions opts;
    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts);

    STUTTO_ASSERT(report.frame_timeline.size() == 1024);
    // Anchor frame should be at index 924
    STUTTO_ASSERT(report.frame_timeline[924].is_trigger_frame);
    STUTTO_ASSERT(report.frame_timeline[924].relative_index == 0);
    STUTTO_ASSERT(report.frame_timeline.front().relative_index == -924);
    STUTTO_ASSERT(report.frame_timeline.back().relative_index == 99);
    std::cout << "  -> Retained 1024 frames (924 pre, 100 post). PASSED.\n";
}

static void test_asymmetric_expand_post() {
    std::cout << "[TEST] Validating Branch 4: Asymmetric Expand Post (100 pre + 1500 post)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t pid = 1234;

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
    trigger.duration_ms = 45.0;
    trigger.target_pid = pid;

    // 100 pre + 1500 post -> take_pre = 100, take_post = min(1500, 1024 - 100) = 924
    auto snapshot = generate_frames(pid, trigger.trigger_timestamp_qpc, qpc_freq, 100, 1500);

    stuttometer::CorrelateOptions opts;
    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts);

    STUTTO_ASSERT(report.frame_timeline.size() == 1024);
    // Anchor frame should be at index 100
    STUTTO_ASSERT(report.frame_timeline[100].is_trigger_frame);
    STUTTO_ASSERT(report.frame_timeline[100].relative_index == 0);
    STUTTO_ASSERT(report.frame_timeline.front().relative_index == -100);
    STUTTO_ASSERT(report.frame_timeline.back().relative_index == 923);
    std::cout << "  -> Retained 1024 frames (100 pre, 924 post). PASSED.\n";
}

static void test_non_frame_trigger() {
    std::cout << "[TEST] Validating Non-Frame Trigger (Audio Glitch / No Frames)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    // 1. Audio Glitch with target_pid = 0
    stuttometer::TriggerInfo audio_trigger;
    audio_trigger.source = stuttometer::TriggerSource::AUDIO_GLITCH;
    audio_trigger.trigger_timestamp_qpc = base_qpc;
    audio_trigger.duration_ms = 10.0;
    audio_trigger.target_pid = 0;

    std::vector<stuttometer::EtwEventRecord> snapshot;
    stuttometer::CorrelateOptions opts;
    auto report1 = correlator.correlate(snapshot, audio_trigger, qpc_freq, opts);
    STUTTO_ASSERT(report1.frame_timeline.empty());

    // 2. target_pid != 0 but no DXGI Present events in snapshot
    stuttometer::TriggerInfo frame_trigger;
    frame_trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    frame_trigger.trigger_timestamp_qpc = base_qpc;
    frame_trigger.target_pid = 9999;
    auto report2 = correlator.correlate(snapshot, frame_trigger, qpc_freq, opts);
    STUTTO_ASSERT(report2.frame_timeline.empty());

    // 3. Audio Glitch with target_pid != 0 AND DXGI frames present in snapshot
    stuttometer::TriggerInfo audio_trigger_with_pid;
    audio_trigger_with_pid.source = stuttometer::TriggerSource::AUDIO_GLITCH;
    audio_trigger_with_pid.trigger_timestamp_qpc = base_qpc;
    audio_trigger_with_pid.duration_ms = 10.0;
    audio_trigger_with_pid.target_pid = 1234;

    auto frames_snapshot = generate_frames(1234, base_qpc, qpc_freq, 10, 10);
    auto report3 = correlator.correlate(frames_snapshot, audio_trigger_with_pid, qpc_freq, opts);
    STUTTO_ASSERT(report3.frame_timeline.empty());

    std::cout << "  -> Empty timeline on non-frame or no-DXGI trigger PASSED.\n";
}

static void test_high_refresh_timeline_tagging() {
    std::cout << "[TEST] Validating high-refresh is_pacing_stall timeline tagging (144 Hz)...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    const uint32_t pid = 1234;

    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(500.0, qpc_freq);
    trigger.duration_ms = 20.0;
    trigger.baseline_avg_ms = 6.94; // 144 Hz cadence -> effective threshold = 6.94 + 0.5 = 7.44 ms
    trigger.baseline_fps = 144.0;
    trigger.spike_ratio = 20.0 / 6.94;
    trigger.target_pid = pid;
    trigger.target_tid = 5000;

    // Generate frames around trigger:
    const uint64_t interval_ticks = stuttometer::ms_to_qpc_delta(6.94, qpc_freq);
    std::vector<stuttometer::EtwEventRecord> snapshot;

    // Frame 1: clean frame (6.94 ms)
    stuttometer::EtwEventRecord f1{};
    f1.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    f1.event_id = 43;
    f1.pid = pid;
    f1.tid = 5000;
    f1.duration_us = 6940;
    f1.qpc_timestamp = trigger.trigger_timestamp_qpc - (2 * interval_ticks);
    snapshot.push_back(f1);

    // Frame 2: 8.5 ms frame (above 7.44ms, but below default 16.67ms present threshold)
    stuttometer::EtwEventRecord f2{};
    f2.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    f2.event_id = 43;
    f2.pid = pid;
    f2.tid = 5000;
    f2.duration_us = 8500;
    f2.qpc_timestamp = trigger.trigger_timestamp_qpc - interval_ticks;
    snapshot.push_back(f2);

    // Anchor frame: 20.0 ms
    stuttometer::EtwEventRecord anchor{};
    anchor.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    anchor.event_id = 43;
    anchor.pid = pid;
    anchor.tid = 5000;
    anchor.duration_us = 20000;
    anchor.qpc_timestamp = trigger.trigger_timestamp_qpc;
    snapshot.push_back(anchor);

    stuttometer::CorrelateOptions opts;
    opts.present_threshold_ms = 16.67; // Notice static threshold is 16.67, but baseline_avg_ms is 6.94
    auto report = correlator.correlate(snapshot, trigger, qpc_freq, opts);

    STUTTO_ASSERT(report.frame_timeline.size() == 3);
    STUTTO_ASSERT(!report.frame_timeline[0].is_pacing_stall); // 6.94 ms < 7.44 ms
    STUTTO_ASSERT(report.frame_timeline[1].is_pacing_stall);  // 8.5 ms >= 7.44 ms (tagged as stall because baseline is 144Hz!)
    STUTTO_ASSERT(report.frame_timeline[2].is_pacing_stall);  // 20.0 ms >= 7.44 ms

    // Fallback when baseline_avg_ms is 0.0 -> uses present_threshold_ms (16.67 ms -> threshold = 17.5035 ms)
    trigger.baseline_avg_ms = 0.0;
    auto report_fallback = correlator.correlate(snapshot, trigger, qpc_freq, opts);
    STUTTO_ASSERT(report_fallback.frame_timeline.size() == 3);
    STUTTO_ASSERT(!report_fallback.frame_timeline[0].is_pacing_stall); // 6.94 ms < 17.5 ms
    STUTTO_ASSERT(!report_fallback.frame_timeline[1].is_pacing_stall); // 8.5 ms < 17.5 ms (not a stall under 16.67ms threshold!)
    STUTTO_ASSERT(report_fallback.frame_timeline[2].is_pacing_stall);  // 20.0 ms >= 17.5 ms

    std::cout << "  -> High-refresh is_pacing_stall timeline tagging PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Frame Timeline Unit Tests ===\n";
    try {
        test_uncapped_timeline();
        test_symmetric_capping();
        test_asymmetric_expand_pre();
        test_asymmetric_expand_post();
        test_non_frame_trigger();
        test_high_refresh_timeline_tagging();
        std::cout << ">>> All Frame Timeline tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
