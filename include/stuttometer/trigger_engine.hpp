#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace stuttometer {

enum class TriggerSource : uint8_t {
    NONE                  = 0,
    DXGI_PRESENT_STUTTER  = 1,
    AUDIO_GLITCH          = 2,
    MANUAL                = 3
};

inline std::string_view trigger_source_to_string(TriggerSource src) noexcept {
    switch (src) {
        case TriggerSource::DXGI_PRESENT_STUTTER: return "DXGI_PRESENT_STUTTER";
        case TriggerSource::AUDIO_GLITCH:         return "AUDIO_GLITCH";
        case TriggerSource::MANUAL:               return "MANUAL";
        default:                                  return "NONE";
    }
}

enum class TriggerState : uint8_t {
    ARMED           = 0,
    TRIGGERED       = 1,
    COLLECTING_POST = 2,
    FROZEN          = 3,
    COOLDOWN        = 4
};

struct TriggerInfo {
    TriggerSource source{TriggerSource::NONE};
    uint64_t trigger_timestamp_qpc{0};
    double duration_ms{0.0};
    uint32_t target_pid{0};
    uint32_t target_tid{0};
    std::string target_process;
};

struct TriggerConfig {
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{25.0};
    bool audio_trigger_enabled{true};
    double cooldown_ms{1000.0};
    uint32_t target_pid{0};             // 0 = auto-detect / all
    std::string target_process_name;   // resolved at startup/background watcher
};

class TriggerEngine {
public:
    explicit TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq);
    ~TriggerEngine() = default;

    // Evaluates DXGI present latency; returns true if it initiated a new trigger (zero syscalls)
    bool on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc) noexcept;

    // Evaluates AudioGlitch event; returns true if it initiated a new trigger (zero syscalls)
    bool on_audio_glitch(uint32_t pid, uint32_t tid, uint32_t glitch_count, uint64_t timestamp_qpc) noexcept;

    // Polls the state machine. When a capture window is finalized (FROZEN), returns true
    // and populates out_trigger, out_from_qpc, and out_to_qpc.
    bool poll_state(uint64_t current_qpc, TriggerInfo& out_trigger, uint64_t& out_from_qpc, uint64_t& out_to_qpc);

    // Notifies that analysis/reporting is done, moving state to COOLDOWN
    void on_report_completed(uint64_t current_qpc);

    // Dynamically update the active target PID from the background watcher (zero lock contention)
    void update_target_pid(uint32_t pid) noexcept {
        active_target_pid_.store(pid, std::memory_order_release);
    }

    uint32_t active_target_pid() const noexcept {
        return active_target_pid_.load(std::memory_order_relaxed);
    }

    TriggerState current_state() const noexcept { return state_.load(std::memory_order_relaxed); }
    uint64_t suppressed_trigger_count() const noexcept { return suppressed_triggers_.load(std::memory_order_relaxed); }

private:
    inline bool should_trigger_on_process(uint32_t pid) const noexcept {
        const uint32_t target = active_target_pid_.load(std::memory_order_relaxed);
        return (target == 0 || target == pid);
    }

    void initiate_trigger(TriggerSource src, uint64_t timestamp_qpc, double duration_ms, uint32_t pid, uint32_t tid);

    const TriggerConfig config_;
    const uint64_t qpc_freq_;
    const uint64_t pre_window_qpc_;
    const uint64_t post_window_qpc_;
    const uint64_t cooldown_qpc_;

    std::atomic<uint32_t> active_target_pid_{0};
    std::atomic<TriggerState> state_{TriggerState::ARMED};
    std::atomic<uint64_t> suppressed_triggers_{0};

    std::mutex trigger_mutex_;
    TriggerInfo active_trigger_{};
    uint64_t post_target_qpc_{0};
    uint64_t cooldown_target_qpc_{0};
};

} // namespace stuttometer
