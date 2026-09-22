#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/session_benchmark.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <algorithm>

namespace stuttometer {

TriggerEngine::TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq)
    : config_(config)
    , qpc_freq_(qpc_freq)
    , qpc_1s_delta_(ms_to_qpc_delta(1000.0, qpc_freq))
    , pre_window_qpc_(ms_to_qpc_delta(config.window_pre_ms, qpc_freq))
    , gpu_pre_window_qpc_(ms_to_qpc_delta(std::max(config.window_pre_ms, std::clamp(config.window_pre_ms * 1.5, 250.0, 1200.0)), qpc_freq))
    , post_window_qpc_(ms_to_qpc_delta(config.window_post_ms, qpc_freq))
    , cooldown_qpc_(ms_to_qpc_delta(config.cooldown_ms, qpc_freq))
    , watchdog_qpc_(ms_to_qpc_delta(5000.0, qpc_freq)) // 5.0s recovery
    , last_target_pid_(config.target_pid)
{
    const bool waiting = (!config.target_process_name.empty() && config.target_pid == 0);
    const uint64_t initial_state = (static_cast<uint64_t>(config.target_pid) << 32) | (waiting ? 1ULL : 0ULL);
    target_state_.store(initial_state, std::memory_order_relaxed);
    target_attach_qpc_.store(get_current_qpc(), std::memory_order_release);
}

void TriggerEngine::on_target_changed(uint32_t new_pid) noexcept {
    // Skip redundant reset if target PID didn't change (Resolves M-10-1)
    if (last_target_pid_.exchange(new_pid, std::memory_order_acq_rel) == new_pid) {
        return;
    }
    dxgi_observed_for_target_.store(false, std::memory_order_release);
    last_dxgi_timestamp_qpc_.store(0, std::memory_order_release);
    target_attach_qpc_.store(get_current_qpc(), std::memory_order_release);
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
    // ---- Phase 1: Claim state + generation, atomic together under writer_lock_ ----
    uint64_t my_gen = 0;
    {
        while (writer_lock_.test_and_set(std::memory_order_acquire)) {
            cpu_pause();
        }

        TriggerState expected = TriggerState::ARMED;
        if (!state_.compare_exchange_strong(expected, TriggerState::CLAIMED,
                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
            writer_lock_.clear(std::memory_order_release);
            suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        my_gen = claim_generation_.fetch_add(1, std::memory_order_relaxed) + 1;

        writer_lock_.clear(std::memory_order_release);
    }

    // ---- Phase 2: Atomic metadata updates (no lock required) ----
    // NOTE: ETW timestamps (timestamp_qpc) are in FileTime domain (~1.34e17, 100ns since 1601),
    // not QPC domain (~get_current_qpc()). poll_state compares against get_current_qpc(), so
    // post_target_qpc_ and claimed_timestamp_qpc_ must be in QPC domain or the deadline is
    // unreachable and the state machine sticks permanently in COLLECTING_POST.
    // active_trigger_.trigger_timestamp_qpc (Phase 3) stays in ETW domain for snapshot windowing.
    const uint64_t now_qpc = get_current_qpc();
    claimed_timestamp_qpc_.store(now_qpc, std::memory_order_release);
    active_source_.store(src, std::memory_order_release);
    post_target_qpc_.store(now_qpc + post_window_qpc_, std::memory_order_release);
    report_consumed_.store(false, std::memory_order_release);

    // Test-only seam: signal that Phase 1 & 2 are complete before entering pause loop
    claim_phase_entered_for_test_.store(true, std::memory_order_release);

    // Bounded generation-targeted test seam: pauses matching thread (0 = no thread pauses; UINT64_MAX = all threads pause)
    const uint64_t target_pause_gen = pause_only_generation_for_test_.load(std::memory_order_acquire);
    if (target_pause_gen != 0 && (target_pause_gen == UINT64_MAX || target_pause_gen == my_gen)) {
        for (size_t i = 0; i < 1'000'000'000ULL &&
                           pause_after_claim_for_test_.load(std::memory_order_acquire) &&
                           (pause_only_generation_for_test_.load(std::memory_order_acquire) == UINT64_MAX ||
                            pause_only_generation_for_test_.load(std::memory_order_acquire) == my_gen); ++i) {
            cpu_pause();
        }
    }

    // ---- Phase 3: Publish via seqlock under writer_lock_ with generation guard ----
    {
        while (writer_lock_.test_and_set(std::memory_order_acquire)) {
            cpu_pause();
        }

        // Only the newest claim generation is permitted to publish into active_trigger_
        if (claim_generation_.load(std::memory_order_acquire) != my_gen) {
            writer_lock_.clear(std::memory_order_release);
            suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint64_t seq = active_trigger_seq_.load(std::memory_order_relaxed);
        active_trigger_seq_.store(seq + 1, std::memory_order_release); // Mark odd (write in progress)
        std::atomic_thread_fence(std::memory_order_release);

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

        std::atomic_thread_fence(std::memory_order_release);
        active_trigger_seq_.store(seq + 2, std::memory_order_release); // Mark even (valid data published)

        writer_lock_.clear(std::memory_order_release);
    }

    // ---- Phase 4: Transition state to COLLECTING_POST or FROZEN ----
    // Crucial invariant: Phase 4 runs strictly AFTER Phase 3 completes, guaranteeing that
    // when poll_state observes COLLECTING_POST or FROZEN, active_trigger_ is fully published.
    if (claim_generation_.load(std::memory_order_acquire) != my_gen) {
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    TriggerState expected_claimed = TriggerState::CLAIMED;
    const TriggerState next_state = (config_.window_post_ms > 0.0)
        ? TriggerState::COLLECTING_POST
        : TriggerState::FROZEN;

    if (next_state == TriggerState::FROZEN) {
        frozen_timestamp_qpc_.store(timestamp_qpc, std::memory_order_release);
    }

    if (!state_.compare_exchange_strong(expected_claimed, next_state,
                                        std::memory_order_release, std::memory_order_acquire)) {
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

    const double effective_static_threshold = calculate_effective_static_threshold(config_.present_threshold_ms);

    const uint64_t key = (stream_key != 0) ? stream_key : (static_cast<uint64_t>(pid) << 32 | tid);
    out_pacing_res = FramePacingResult{};

    RollingFrameStats default_stats{};
    reset_frame_stats(default_stats, timestamp_qpc);

    bool upsert_ok = pacing_table_.upsert(key, default_stats, [&](RollingFrameStats& stats) {
        double eff_mult = config_.spike_multiplier;
        double eff_delta = config_.min_spike_delta_ms;
        if (config_.pacing_profile == PacingProfile::AUTO_ADAPTIVE) {
            const double current_mean = calculate_mean_ms(stats);
            auto params = compute_adaptive_pacing_params(current_mean);
            eff_mult = params.spike_multiplier;
            eff_delta = params.min_spike_delta_ms;
        } else if (config_.pacing_profile == PacingProfile::HIGH_REFRESH) {
            eff_mult = HIGH_REFRESH_SPIKE_MULTIPLIER;
            eff_delta = HIGH_REFRESH_MIN_DELTA_MS;
        } else if (config_.pacing_profile == PacingProfile::CONSERVATIVE) {
            eff_mult = CONSERVATIVE_SPIKE_MULTIPLIER;
            eff_delta = CONSERVATIVE_MIN_DELTA_MS;
        }

        out_pacing_res = evaluate_frame_pacing(
            stats,
            duration_ms,
            timestamp_qpc,
            qpc_freq_,
            config_.frame_trigger_mode,
            eff_mult,
            eff_delta,
            config_.enable_judder_detection,
            config_.judder_swing_ratio,
            effective_static_threshold,
            config_.pacing_profile
        );

        SessionBenchmark* sink = benchmark_sink_.load(std::memory_order_acquire);
        if (sink && active_target_pid() != 0 && pid == active_target_pid() && stats.sample_count >= 8) {
            const double pushed_baseline = out_pacing_res.is_stutter ? out_pacing_res.baseline_avg_ms : calculate_mean_ms(stats);
            sink->update_pacing_telemetry(pushed_baseline, eff_mult, eff_delta);
        }
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

    fprintf(stderr, "[D-B] upsert_ok=%d is_stutter=%d reason=%d dur=%.3f thr=%.3f mode=%d\n",
            (int)upsert_ok,
            (int)out_pacing_res.is_stutter,
            (int)out_pacing_res.reason,
            duration_ms,
            effective_static_threshold,
            (int)config_.frame_trigger_mode);
    return out_pacing_res.is_stutter;
}

bool TriggerEngine::on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint64_t stream_key, uint8_t cpu_index) noexcept {
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    SessionBenchmark* sink = benchmark_sink_.load(std::memory_order_acquire);
    if (sink) {
        // Note N-3: intentional: only latch when a target is actively benchmarked.
        dxgi_observed_for_target_.store(true, std::memory_order_release);
        last_dxgi_timestamp_qpc_.store(timestamp_qpc, std::memory_order_release);
        sink->ingest_frame(pid, duration_ms, timestamp_qpc);
    }

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
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    SessionBenchmark* sink = benchmark_sink_.load(std::memory_order_acquire);
    if (sink && !dxgi_observed_for_target_.load(std::memory_order_acquire)) {
        const uint64_t attach_qpc = target_attach_qpc_.load(std::memory_order_acquire);
        if (timestamp_qpc > attach_qpc && (timestamp_qpc - attach_qpc) > qpc_1s_delta_) {
            sink->ingest_frame(pid, duration_ms, timestamp_qpc);
        }
    }

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

bool TriggerEngine::on_dwm_glitch(uint32_t /*pid*/, uint32_t /*tid*/, double duration_ms, uint64_t timestamp_qpc, uint8_t cpu_index) noexcept {
    const double vb = vblank_interval_ms();
    const double vblank_ms = (vb > 0.0) ? vb : 16.67;
    const double jitter_guard = std::max(0.5, vblank_ms * 0.05);
    const double effective_threshold = std::max(1.0, vblank_ms - jitter_guard);

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
    const uint32_t effective_pid = (target_pid != 0) ? target_pid : 0;
    const uint32_t effective_tid = 0;

    const double spike_ratio = (vblank_ms > 0.0) ? (duration_ms / vblank_ms) : 1.0;
    const double baseline_fps = (vblank_ms > 0.0) ? (1000.0 / vblank_ms) : 60.0;

    return initiate_trigger_atomic(
        TriggerSource::DWM_GLITCH,
        TriggerReason::DWM_COMPOSITOR_GLITCH,
        timestamp_qpc,
        duration_ms,
        effective_pid,
        effective_tid,
        cpu_index,
        0,
        vblank_ms,
        baseline_fps,
        spike_ratio
    );
}

bool TriggerEngine::on_audio_glitch(uint32_t /*pid*/, uint32_t /*tid*/, uint32_t glitch_count, uint64_t timestamp_qpc, uint8_t cpu_index) noexcept {
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
    const uint32_t effective_pid = (target_pid != 0) ? target_pid : 0;
    const uint32_t effective_tid = 0;

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

        // Single-emission guard: guarantee poll_state returns true only once per trigger cycle
        if (report_consumed_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        // Step 1: Seqlock read into out_trigger
        uint64_t s1 = 0, s2 = 0;
        do {
            s1 = active_trigger_seq_.load(std::memory_order_acquire);
            while ((s1 % 2) != 0) {
                cpu_pause();
                s1 = active_trigger_seq_.load(std::memory_order_acquire);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            out_trigger = active_trigger_;
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = active_trigger_seq_.load(std::memory_order_acquire);
        } while (s1 != s2);

        // Step 2: Apply staged GPU upgrade to the LOCAL out_trigger copy (never write back to active_trigger_)
        uint32_t staged_us = staged_gpu_duration_us_.exchange(0, std::memory_order_acq_rel);
        if (staged_us > 0) {
            const double staged_ms = staged_us / 1000.0;
            if (staged_ms > out_trigger.duration_ms) {
                out_trigger.source = TriggerSource::KERNEL_FRAME_STALL;
                out_trigger.duration_ms = staged_ms;
                if (out_trigger.baseline_avg_ms > 0.0) {
                    out_trigger.spike_ratio = out_trigger.duration_ms / out_trigger.baseline_avg_ms;
                }
            }
        }

        // Step 3: Compute capture bounds based on out_trigger
        const uint64_t trig_qpc = out_trigger.trigger_timestamp_qpc;
        const TriggerSource trig_src = out_trigger.source;
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
            on_target_changed(pid);
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
            on_target_changed(0);
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

double TriggerEngine::current_stream_baseline_ms_for_test(uint64_t stream_key, uint32_t pid, uint32_t tid) const noexcept {
    const uint64_t key = (stream_key != 0) ? stream_key : (static_cast<uint64_t>(pid) << 32 | tid);
    RollingFrameStats stats{};
    if (pacing_table_.lookup(key, stats)) {
        return calculate_mean_ms(stats);
    }
    return 0.0;
}

} // namespace stuttometer
