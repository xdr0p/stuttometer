#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <algorithm>

namespace stuttometer {

TriggerEngine::TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq)
    : config_(config)
    , qpc_freq_(qpc_freq)
    , pre_window_qpc_(ms_to_qpc_delta(config.window_pre_ms, qpc_freq))
    , gpu_pre_window_qpc_(ms_to_qpc_delta(std::max(config.window_pre_ms, std::clamp(config.window_pre_ms * 1.5, 250.0, 1200.0)), qpc_freq))
    , post_window_qpc_(ms_to_qpc_delta(config.window_post_ms, qpc_freq))
    , cooldown_qpc_(ms_to_qpc_delta(config.cooldown_ms, qpc_freq))
    , watchdog_qpc_(ms_to_qpc_delta(5000.0, qpc_freq)) // 5.0s recovery
{
    const bool waiting = (!config.target_process_name.empty() && config.target_pid == 0);
    const uint64_t initial_state = (static_cast<uint64_t>(config.target_pid) << 32) | (waiting ? 1ULL : 0ULL);
    target_state_.store(initial_state, std::memory_order_relaxed);
}

bool TriggerEngine::initiate_trigger_atomic(
    TriggerSource src,
    TriggerReason reason,
    uint64_t timestamp_qpc,
    double duration_ms,
    uint32_t pid,
    uint32_t tid,
    uint8_t cpu_index,
    uint32_t glitch_count,
    double baseline_avg_ms,
    double baseline_fps,
    double spike_ratio
) noexcept {
    // Step 1: Atomic CAS ARMED -> CLAIMED
    TriggerState expected = TriggerState::ARMED;
    if (!state_.compare_exchange_strong(expected, TriggerState::CLAIMED, std::memory_order_acq_rel)) {
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 2: Populate metadata under mutex (zero allocations, raw numeric IDs only)
    claimed_timestamp_qpc_.store(timestamp_qpc, std::memory_order_release);
    active_source_.store(src, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(active_trigger_mutex_);
        active_trigger_.source = src;
        active_trigger_.reason = reason;
        active_trigger_.trigger_timestamp_qpc = timestamp_qpc;
        active_trigger_.duration_ms = duration_ms;
        active_trigger_.baseline_avg_ms = baseline_avg_ms;
        active_trigger_.baseline_fps = baseline_fps;
        active_trigger_.spike_ratio = spike_ratio;
        active_trigger_.glitch_count = glitch_count;
        active_trigger_.target_pid = pid;
        active_trigger_.target_tid = tid;
        active_trigger_.cpu_index = cpu_index;
    }

    post_target_qpc_.store(timestamp_qpc + post_window_qpc_, std::memory_order_release);
    report_consumed_.store(false, std::memory_order_release);

    // Step 3: Transition state to COLLECTING_POST or FROZEN with release semantics via CAS
    TriggerState expected_claimed = TriggerState::CLAIMED;
    const TriggerState next_state = (config_.window_post_ms > 0.0)
        ? TriggerState::COLLECTING_POST
        : TriggerState::FROZEN;

    if (next_state == TriggerState::FROZEN) {
        frozen_timestamp_qpc_.store(timestamp_qpc, std::memory_order_release);
    }

    if (!state_.compare_exchange_strong(expected_claimed, next_state,
                                        std::memory_order_release, std::memory_order_acquire)) {
        // Watchdog expired and reset engine while we were populating trigger metadata
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool TriggerEngine::evaluate_frame_pacing_common(
    uint32_t pid,
    uint32_t tid,
    double duration_ms,
    uint64_t timestamp_qpc,
    uint64_t stream_key,
    FramePacingResult& out_pacing_res
) noexcept {
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    const double jitter_guard = std::max(0.5, config_.present_threshold_ms * 0.05);
    const double effective_static_threshold = config_.present_threshold_ms + jitter_guard;

    const uint64_t key = (stream_key != 0) ? stream_key : (static_cast<uint64_t>(pid) << 32 | tid);
    out_pacing_res = FramePacingResult{};

    RollingFrameStats default_stats{};
    reset_frame_stats(default_stats, timestamp_qpc);

    bool upsert_ok = pacing_table_.upsert(key, default_stats, [&](RollingFrameStats& stats) {
        out_pacing_res = evaluate_frame_pacing(
            stats,
            duration_ms,
            timestamp_qpc,
            qpc_freq_,
            config_.frame_trigger_mode,
            config_.spike_multiplier,
            config_.min_spike_delta_ms,
            config_.enable_judder_detection,
            config_.judder_swing_ratio,
            effective_static_threshold
        );
    });

    if (!upsert_ok) {
        // Fallback if table is at capacity or CAS retry exhausted: evaluate static threshold
        if (duration_ms >= effective_static_threshold) {
            out_pacing_res.is_stutter = true;
            out_pacing_res.reason = TriggerReason::STATIC_THRESHOLD;
            out_pacing_res.baseline_avg_ms = 0.0;
            out_pacing_res.baseline_fps = 0.0;
            out_pacing_res.spike_ratio = 1.0;
        }
    }

    return out_pacing_res.is_stutter;
}

bool TriggerEngine::on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint64_t stream_key, uint8_t cpu_index) noexcept {
    FramePacingResult pacing_res{};
    if (!evaluate_frame_pacing_common(pid, tid, duration_ms, timestamp_qpc, stream_key, pacing_res)) {
        return false;
    }

    TriggerSource src = (pacing_res.reason == TriggerReason::CADENCE_JUDDER)
        ? TriggerSource::FRAME_PACING_JUDDER
        : TriggerSource::DXGI_PRESENT_STUTTER;

    return initiate_trigger_atomic(
        src,
        pacing_res.reason,
        timestamp_qpc,
        duration_ms,
        pid,
        tid,
        cpu_index,
        0,
        pacing_res.baseline_avg_ms,
        pacing_res.baseline_fps,
        pacing_res.spike_ratio
    );
}

bool TriggerEngine::on_kernel_frame_stall(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint64_t stream_key, uint8_t cpu_index) noexcept {
    FramePacingResult pacing_res{};
    if (!evaluate_frame_pacing_common(pid, tid, duration_ms, timestamp_qpc, stream_key, pacing_res)) {
        return false;
    }

    TriggerState current = state_.load(std::memory_order_acquire);
    TriggerSource src = (pacing_res.reason == TriggerReason::CADENCE_JUDDER)
        ? TriggerSource::FRAME_PACING_JUDDER
        : TriggerSource::KERNEL_FRAME_STALL;

    if (current == TriggerState::ARMED) {
        return initiate_trigger_atomic(
            src,
            pacing_res.reason,
            timestamp_qpc,
            duration_ms,
            pid,
            tid,
            cpu_index,
            0,
            pacing_res.baseline_avg_ms,
            pacing_res.baseline_fps,
            pacing_res.spike_ratio
        );
    }

    // Thread-safe trigger upgrade: if collecting post-window from a CPU Present trigger, stage GPU stall upgrade (worst-case duration)
    if (current == TriggerState::COLLECTING_POST && active_source_.load(std::memory_order_acquire) == TriggerSource::DXGI_PRESENT_STUTTER) {
        const uint32_t candidate_us = static_cast<uint32_t>(std::clamp(duration_ms * 1000.0, 1.0, 10000000.0));
        uint32_t prev_us = staged_gpu_duration_us_.load(std::memory_order_relaxed);
        while (candidate_us > prev_us &&
               !staged_gpu_duration_us_.compare_exchange_weak(prev_us, candidate_us,
                   std::memory_order_release, std::memory_order_relaxed)) {}
        return true;
    }

    suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool TriggerEngine::on_dwm_glitch(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint8_t cpu_index) noexcept {
    const double jitter_guard = std::max(0.5, config_.present_threshold_ms * 0.05);
    const double effective_threshold = std::max(1.0, config_.present_threshold_ms - jitter_guard);

    if (duration_ms < effective_threshold) {
        return false;
    }

    // DWM glitches originate from dwm.exe, not the game PID directly.
    // Instead of filtering via should_trigger_on_process(pid) which would reject dwm.exe,
    // we attribute the glitch trigger to the configured target PID (with tid = 0).
    const uint64_t target_state = target_state_.load(std::memory_order_acquire);
    const bool waiting = (target_state & 1ULL) != 0;
    if (waiting) {
        return false;
    }
    const uint32_t target_pid = static_cast<uint32_t>(target_state >> 32);
    const uint32_t effective_pid = (target_pid != 0) ? target_pid : pid;
    const uint32_t effective_tid = (target_pid != 0) ? 0 : tid;

    return initiate_trigger_atomic(
        TriggerSource::DWM_GLITCH,
        TriggerReason::DWM_COMPOSITOR_GLITCH,
        timestamp_qpc,
        duration_ms,
        effective_pid,
        effective_tid,
        cpu_index,
        0
    );
}

bool TriggerEngine::on_audio_glitch(uint32_t pid, uint32_t tid, uint32_t glitch_count, uint64_t timestamp_qpc, uint8_t cpu_index) noexcept {
    if (!config_.audio_trigger_enabled || glitch_count == 0) {
        return false;
    }
    // If target process is configured but not yet running, suppress audio triggers
    const uint64_t target_state = target_state_.load(std::memory_order_acquire);
    const bool waiting = (target_state & 1ULL) != 0;
    if (waiting) {
        return false;
    }

    // Audio glitches originate from audiodg.exe or audio service, not the game PID directly.
    // If a target PID is actively configured and running, attribute trigger to target PID with target_tid = 0.
    const uint32_t target_pid = static_cast<uint32_t>(target_state >> 32);
    const uint32_t effective_pid = (target_pid != 0) ? target_pid : pid;
    const uint32_t effective_tid = (target_pid != 0) ? 0 : tid;

    return initiate_trigger_atomic(
        TriggerSource::AUDIO_GLITCH,
        TriggerReason::AUDIO_BUFFER_UNDERRUN,
        timestamp_qpc,
        0.0,
        effective_pid,
        effective_tid,
        cpu_index,
        glitch_count
    );
}

bool TriggerEngine::poll_state(
    uint64_t current_qpc,
    TriggerInfo& out_trigger,
    uint64_t& out_from_qpc,
    uint64_t& out_to_qpc
) {
    TriggerState current = state_.load(std::memory_order_acquire);

    // Check CLAIMED state with watchdog to auto-recover from abandoned claims
    if (current == TriggerState::CLAIMED) {
        const uint64_t claimed_ts = claimed_timestamp_qpc_.load(std::memory_order_acquire);
        if (claimed_ts > 0 && current_qpc >= claimed_ts && (current_qpc - claimed_ts) > watchdog_qpc_) {
            on_report_completed(current_qpc);
        }
        return false;
    }

    if (current == TriggerState::COLLECTING_POST) {
        const uint64_t target_qpc = post_target_qpc_.load(std::memory_order_acquire);
        if (current_qpc >= target_qpc) {
            frozen_timestamp_qpc_.store(current_qpc, std::memory_order_release);
            report_consumed_.store(false, std::memory_order_release);
            state_.store(TriggerState::FROZEN, std::memory_order_release);
            current = TriggerState::FROZEN;

            // Apply staged GPU upgrade safely on single analysis thread after state is FROZEN (only upgrade if worse)
            uint32_t staged_us = staged_gpu_duration_us_.exchange(0, std::memory_order_acq_rel);
            if (staged_us > 0) {
                const double staged_ms = staged_us / 1000.0;
                std::lock_guard<std::mutex> lock(active_trigger_mutex_);
                if (staged_ms > active_trigger_.duration_ms) {
                    active_source_.store(TriggerSource::KERNEL_FRAME_STALL, std::memory_order_release);
                    active_trigger_.source = TriggerSource::KERNEL_FRAME_STALL;
                    active_trigger_.duration_ms = staged_ms;
                    if (active_trigger_.baseline_avg_ms > 0.0) {
                        active_trigger_.spike_ratio = active_trigger_.duration_ms / active_trigger_.baseline_avg_ms;
                    }
                }
            }
        }
    }

    if (current == TriggerState::FROZEN) {
        uint64_t frozen_ts = frozen_timestamp_qpc_.load(std::memory_order_acquire);
        if (frozen_ts == 0) {
            frozen_timestamp_qpc_.store(current_qpc, std::memory_order_release);
            frozen_ts = current_qpc;
        }
        // Check 5-second watchdog to prevent getting permanently stuck
        if (current_qpc >= frozen_ts && (current_qpc - frozen_ts) > watchdog_qpc_) {
            on_report_completed(current_qpc);
            return false;
        }

        // Apply any late staged GPU duration before report emission
        if (!report_consumed_.load(std::memory_order_acquire)) {
            uint32_t staged_us = staged_gpu_duration_us_.exchange(0, std::memory_order_acq_rel);
            if (staged_us > 0) {
                const double staged_ms = staged_us / 1000.0;
                std::lock_guard<std::mutex> lock(active_trigger_mutex_);
                if (staged_ms > active_trigger_.duration_ms) {
                    active_source_.store(TriggerSource::KERNEL_FRAME_STALL, std::memory_order_release);
                    active_trigger_.source = TriggerSource::KERNEL_FRAME_STALL;
                    active_trigger_.duration_ms = staged_ms;
                    if (active_trigger_.baseline_avg_ms > 0.0) {
                        active_trigger_.spike_ratio = active_trigger_.duration_ms / active_trigger_.baseline_avg_ms;
                    }
                }
            }
        }

        // Single-emission guard: guarantee poll_state returns true only once per trigger cycle
        if (report_consumed_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        uint64_t trig_qpc = 0;
        TriggerSource trig_src = TriggerSource::NONE;
        {
            std::lock_guard<std::mutex> lock(active_trigger_mutex_);
            out_trigger = active_trigger_;
            trig_qpc = active_trigger_.trigger_timestamp_qpc;
            trig_src = active_trigger_.source;
        }

        const uint64_t pre_qpc = (trig_src == TriggerSource::KERNEL_FRAME_STALL)
            ? gpu_pre_window_qpc_ : pre_window_qpc_;

        out_from_qpc = (trig_qpc > pre_qpc) ? (trig_qpc - pre_qpc) : 0;
        out_to_qpc   = trig_qpc + post_window_qpc_;
        return true;
    }

    if (current == TriggerState::COOLDOWN) {
        if (current_qpc >= cooldown_target_qpc_.load(std::memory_order_acquire)) {
            state_.store(TriggerState::ARMED, std::memory_order_release);
        }
    }

    return false;
}

void TriggerEngine::on_report_completed(uint64_t current_qpc) noexcept {
    cooldown_target_qpc_.store(current_qpc + cooldown_qpc_, std::memory_order_release);
    frozen_timestamp_qpc_.store(0, std::memory_order_release);
    claimed_timestamp_qpc_.store(0, std::memory_order_release);
    active_source_.store(TriggerSource::NONE, std::memory_order_release);
    staged_gpu_duration_us_.store(0, std::memory_order_relaxed);
    report_consumed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(active_trigger_mutex_);
        active_trigger_ = TriggerInfo{};
    }
    state_.store(TriggerState::COOLDOWN, std::memory_order_release);
}

bool TriggerEngine::try_attach_pid(uint32_t pid) noexcept {
    uint64_t expected = target_state_.load(std::memory_order_acquire);
    while (true) {
        if ((expected & 1ULL) == 0) return false; // Already attached or monitor-all
        const uint64_t desired = (static_cast<uint64_t>(pid) << 32) | 0ULL;
        if (target_state_.compare_exchange_weak(expected, desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return true;
        }
    }
}

bool TriggerEngine::try_detach_pid(uint32_t pid) noexcept {
    if (config_.target_process_name.empty()) return false; // monitor-all mode: no-op
    uint64_t expected = target_state_.load(std::memory_order_acquire);
    while (true) {
        const uint32_t cur_pid = static_cast<uint32_t>(expected >> 32);
        const bool waiting = (expected & 1ULL) != 0;
        if (waiting || cur_pid != pid) return false;
        const uint64_t desired = (0ULL << 32) | 1ULL;
        if (target_state_.compare_exchange_weak(expected, desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return true;
        }
    }
}

bool TriggerEngine::on_process_launched(uint32_t pid, std::string_view process_name) noexcept {
    if (!is_target_waiting()) return false;
    if (process_name.empty() || config_.target_process_name.empty()) return false;
    if (matches_process_name(process_name, config_.target_process_name)) {
        return try_attach_pid(pid);
    }
    return false;
}

void TriggerEngine::on_process_terminated(uint32_t pid) noexcept {
    try_detach_pid(pid);
}

} // namespace stuttometer
