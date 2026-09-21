#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <string_view>
#include "frame_pacing_tracker.hpp"
#include "fixed_table.hpp"

namespace stuttometer {

class SessionBenchmark;

enum class TriggerSource : uint8_t {
    NONE                  = 0,
    DXGI_PRESENT_STUTTER  = 1,
    AUDIO_GLITCH          = 2,
    MANUAL                = 3,
    KERNEL_FRAME_STALL    = 4,
    DWM_GLITCH            = 5,
    FRAME_PACING_JUDDER   = 6
};

inline std::string_view trigger_source_to_string(TriggerSource src) noexcept {
    switch (src) {
        case TriggerSource::DXGI_PRESENT_STUTTER: return "DXGI_PRESENT_STUTTER";
        case TriggerSource::AUDIO_GLITCH:         return "AUDIO_GLITCH";
        case TriggerSource::MANUAL:               return "MANUAL";
        case TriggerSource::KERNEL_FRAME_STALL:   return "KERNEL_FRAME_STALL";
        case TriggerSource::DWM_GLITCH:           return "DWM_GLITCH";
        case TriggerSource::FRAME_PACING_JUDDER:  return "FRAME_PACING_JUDDER";
        default:                                  return "NONE";
    }
}

enum class TriggerState : uint8_t {
    ARMED           = 0,
    CLAIMED         = 1,
    COLLECTING_POST = 2,
    FROZEN          = 3,
    COOLDOWN        = 4
};

// POD trivially-copyable trigger record with explicit 8-byte alignment ordering
struct TriggerInfo {
    // 8-byte aligned
    uint64_t trigger_timestamp_qpc{0};
    double duration_ms{0.0};
    double baseline_avg_ms{0.0};
    double baseline_fps{0.0};
    double spike_ratio{0.0};

    // 4-byte aligned
    uint32_t glitch_count{0};
    uint32_t target_pid{0};
    uint32_t target_tid{0};

    // 1-byte aligned
    TriggerSource source{TriggerSource::NONE};
    TriggerReason reason{TriggerReason::NONE};
    uint8_t cpu_index{0};
    uint8_t _pad[1]{0};
};

static_assert(sizeof(TriggerInfo) == 56, "TriggerInfo must be exactly 56 bytes");
static_assert(std::is_trivially_copyable_v<TriggerInfo>, "TriggerInfo must be trivially copyable");

struct TriggerConfig {
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{16.67};
    double vblank_interval_ms{0.0};
    bool audio_trigger_enabled{true};
    double cooldown_ms{1000.0};
    uint32_t target_pid{0};             // 0 = auto-detect / all
    std::string target_process_name;   // resolved at startup/background watcher

    // Frame Pacing & Dynamic Relative Trigger configuration
    FrameTriggerMode frame_trigger_mode{FrameTriggerMode::HYBRID};
    PacingProfile pacing_profile{PacingProfile::AUTO_ADAPTIVE};
    double spike_multiplier{2.0};
    double min_spike_delta_ms{4.0};
    bool enable_judder_detection{true};
    double judder_swing_ratio{0.35};
};

class TriggerEngine {
public:
    explicit TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq);
    ~TriggerEngine() = default;

    // Evaluates DXGI present latency against dynamic baseline & static thresholds (lock-free, zero-allocation, noexcept)
    bool on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint64_t stream_key = 0, uint8_t cpu_index = 0) noexcept;

    // Evaluates Kernel-level frame delivery latency against dynamic baseline & static thresholds (lock-free, zero-allocation, noexcept)
    bool on_kernel_frame_stall(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint64_t stream_key = 0, uint8_t cpu_index = 0) noexcept;

    // Evaluates DWM composition glitch (lock-free, zero-allocation, noexcept)
    bool on_dwm_glitch(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc, uint8_t cpu_index = 0) noexcept;

    // Evaluates AudioGlitch event (lock-free, zero-allocation, noexcept)
    bool on_audio_glitch(uint32_t pid, uint32_t tid, uint32_t glitch_count, uint64_t timestamp_qpc, uint8_t cpu_index = 0) noexcept;

    // Polls the state machine (called exclusively from the single analysis/monitoring thread)
    // Contract: Exactly one consumer thread must poll poll_state() to maintain single-consumer state transitions.
    bool poll_state(uint64_t current_qpc, TriggerInfo& out_trigger, uint64_t& out_from_qpc, uint64_t& out_to_qpc);

    // Notifies that analysis/reporting is done, moving state to COOLDOWN
    void on_report_completed(uint64_t current_qpc) noexcept;

    // Sets the benchmark sink for session pacing ingestion
    void set_benchmark_sink(SessionBenchmark* sink) noexcept {
        benchmark_sink_.store(sink, std::memory_order_release);
    }

    // Dynamically update the active target PID from the background watcher (indivisible 64-bit state)
    void update_target_pid(uint32_t pid, bool waiting_for_process = false) noexcept {
        const uint64_t val = (static_cast<uint64_t>(pid) << 32) | (waiting_for_process ? 1ULL : 0ULL);
        target_state_.store(val, std::memory_order_release);
        on_target_changed(pid);
    }

    // Dynamic atomic CAS target state transitions (thread-safe, lock-free)
    bool try_attach_pid(uint32_t pid) noexcept;
    bool try_detach_pid(uint32_t pid) noexcept;
    bool on_process_launched(uint32_t pid, std::string_view process_name) noexcept;
    void on_process_terminated(uint32_t pid) noexcept;

    uint32_t active_target_pid() const noexcept {
        const uint64_t state = target_state_.load(std::memory_order_acquire);
        return static_cast<uint32_t>(state >> 32);
    }

    bool is_target_waiting() const noexcept {
        const uint64_t state = target_state_.load(std::memory_order_acquire);
        return (state & 1ULL) != 0;
    }

    TriggerState current_state() const noexcept { return state_.load(std::memory_order_relaxed); }
    uint64_t suppressed_trigger_count() const noexcept { return suppressed_triggers_.load(std::memory_order_relaxed); }
    [[nodiscard]] double vblank_interval_ms() const noexcept {
        return (config_.vblank_interval_ms > 0.0) 
            ? config_.vblank_interval_ms 
            : config_.present_threshold_ms;
    }

    void evict_stale_pacing_entries(uint64_t current_qpc, uint64_t max_age_qpc) noexcept {
        pacing_table_.evict_stale(current_qpc, max_age_qpc, [](const RollingFrameStats& s) {
            return s.last_frame_timestamp_qpc;
        });
    }

    // Test-only seam: must call reset_test_seams() prior to test scenarios that rely on these hooks
    void set_pause_after_claim_for_test(bool p) noexcept {
        pause_after_claim_for_test_.store(p, std::memory_order_release);
    }
    void set_pause_only_generation_for_test(uint64_t gen) noexcept {
        pause_only_generation_for_test_.store(gen, std::memory_order_release);
    }
    bool claim_phase_entered_for_test() const noexcept {
        return claim_phase_entered_for_test_.load(std::memory_order_acquire);
    }
    uint64_t current_claim_generation_for_test() const noexcept {
        return claim_generation_.load(std::memory_order_acquire);
    }
    void reset_test_seams() noexcept {
        // Note: claim_generation_ is monotonic and intentionally NOT reset.
        // Tests targeting a specific generation should use current_claim_generation_for_test() + 1.
        pause_after_claim_for_test_.store(false, std::memory_order_release);
        claim_phase_entered_for_test_.store(false, std::memory_order_release);
        pause_only_generation_for_test_.store(0, std::memory_order_release);
    }

    // Test-only: returns the current rolling baseline for the specified stream key (or derived pid/tid).
    // Returns 0.0 if the stream is not present or has <1 sample. Not used in production.
    double current_stream_baseline_ms_for_test(uint64_t stream_key, uint32_t pid, uint32_t tid) const noexcept;

private:
    inline bool should_trigger_on_process(uint32_t pid) const noexcept {
        const uint64_t state = target_state_.load(std::memory_order_acquire);
        const bool waiting = (state & 1ULL) != 0;
        if (waiting) {
            return false; // Process specified but not currently running -> suppress triggers
        }
        const uint32_t target = static_cast<uint32_t>(state >> 32);
        return (target == 0 || target == pid);
    }

    bool evaluate_frame_pacing_common(
        uint32_t pid,
        uint32_t tid,
        double duration_ms,
        uint64_t timestamp_qpc,
        uint64_t stream_key,
        FramePacingResult& out_pacing_res
    ) noexcept;

    bool initiate_trigger_atomic(
        TriggerSource src,
        TriggerReason reason,
        uint64_t timestamp_qpc,
        double duration_ms,
        uint32_t pid,
        uint32_t tid,
        uint8_t cpu_index,
        uint32_t glitch_count = 0,
        double baseline_avg_ms = 0.0,
        double baseline_fps = 0.0,
        double spike_ratio = 0.0
    ) noexcept;

    void on_target_changed(uint32_t new_pid) noexcept;

    const TriggerConfig config_;
    const uint64_t qpc_freq_;
    const uint64_t qpc_1s_delta_;
    const uint64_t pre_window_qpc_;
    const uint64_t gpu_pre_window_qpc_;
    const uint64_t post_window_qpc_;
    const uint64_t cooldown_qpc_;
    const uint64_t watchdog_qpc_;

    std::atomic<SessionBenchmark*> benchmark_sink_{nullptr};
    // Note: latch is per-TriggerEngine-instance; relies on sink being null in monitor-all (Resolves M-10-4)
    std::atomic<bool> dxgi_observed_for_target_{false};
    std::atomic<uint64_t> last_dxgi_timestamp_qpc_{0};
    std::atomic<uint64_t> target_attach_qpc_{0};
    std::atomic<uint32_t> last_target_pid_{0};

    // Indivisible 64-bit atomic target state: High 32-bits = target_pid, Low 32-bits = waiting_for_process flag
    std::atomic<uint64_t> target_state_{0};
    std::atomic<TriggerState> state_{TriggerState::ARMED};
    std::atomic<TriggerSource> active_source_{TriggerSource::NONE};
    std::atomic<uint64_t> suppressed_triggers_{0};

    // Staged GPU trigger upgrade duration during COLLECTING_POST phase (thread-safe lock-free)
    std::atomic<uint32_t> staged_gpu_duration_us_{0};

    // Stored trigger metadata populated under CLAIMED state, published lock-free via seqlock
    alignas(64) std::atomic<uint64_t> active_trigger_seq_{0};
    alignas(64) std::atomic<uint64_t> claim_generation_{0};
    std::atomic_flag writer_lock_{};
    TriggerInfo active_trigger_{};

    // Test-only seams (unconditional member variables, matching set_running_for_test pattern)
    std::atomic<bool> pause_after_claim_for_test_{false};
    std::atomic<bool> claim_phase_entered_for_test_{false};
    // 0 = no thread pauses; otherwise only the thread whose my_gen matches pause_only_generation_for_test_ pauses
    std::atomic<uint64_t> pause_only_generation_for_test_{0};

    std::atomic<uint64_t> claimed_timestamp_qpc_{0};
    std::atomic<uint64_t> post_target_qpc_{0};
    std::atomic<uint64_t> frozen_timestamp_qpc_{0};
    std::atomic<uint64_t> cooldown_target_qpc_{0};
    std::atomic<bool> report_consumed_{false};

    // Stream-level lock-free rolling statistics table
    FixedInFlightTable<RollingFrameStats, 256> pacing_table_;
};

} // namespace stuttometer

