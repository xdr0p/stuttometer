#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

TriggerEngine::TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq)
    : config_(config)
    , qpc_freq_(qpc_freq)
    , pre_window_qpc_(ms_to_qpc_delta(config.window_pre_ms, qpc_freq))
    , post_window_qpc_(ms_to_qpc_delta(config.window_post_ms, qpc_freq))
    , cooldown_qpc_(ms_to_qpc_delta(config.cooldown_ms, qpc_freq))
    , watchdog_qpc_(ms_to_qpc_delta(5000.0, qpc_freq)) // 5.0s recovery
{
    active_target_pid_.store(config.target_pid, std::memory_order_relaxed);
}

bool TriggerEngine::initiate_trigger_atomic(
    TriggerSource src,
    uint64_t timestamp_qpc,
    double duration_ms,
    uint32_t pid,
    uint32_t tid
) noexcept {
    // Step 1: Atomic CAS ARMED -> CLAIMED
    TriggerState expected = TriggerState::ARMED;
    if (!state_.compare_exchange_strong(expected, TriggerState::CLAIMED, std::memory_order_acq_rel)) {
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Step 2: Populate metadata (zero allocations, raw numeric IDs only)
    active_trigger_.source = src;
    active_trigger_.trigger_timestamp_qpc = timestamp_qpc;
    active_trigger_.duration_ms = duration_ms;
    active_trigger_.target_pid = pid;
    active_trigger_.target_tid = tid;
    active_trigger_.target_process.clear(); // resolved on correlation thread

    post_target_qpc_ = timestamp_qpc + post_window_qpc_;

    // Step 3: Transition state to COLLECTING_POST or FROZEN with release semantics
    const TriggerState next_state = (config_.window_post_ms > 0.0) 
        ? TriggerState::COLLECTING_POST 
        : TriggerState::FROZEN;

    state_.store(next_state, std::memory_order_release);
    return true;
}

bool TriggerEngine::on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc) noexcept {
    if (duration_ms < config_.present_threshold_ms) {
        return false;
    }
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    return initiate_trigger_atomic(TriggerSource::DXGI_PRESENT_STUTTER, timestamp_qpc, duration_ms, pid, tid);
}

bool TriggerEngine::on_audio_glitch(uint32_t pid, uint32_t tid, uint32_t glitch_count, uint64_t timestamp_qpc) noexcept {
    if (!config_.audio_trigger_enabled || glitch_count == 0) {
        return false;
    }
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    return initiate_trigger_atomic(TriggerSource::AUDIO_GLITCH, timestamp_qpc, static_cast<double>(glitch_count), pid, tid);
}

bool TriggerEngine::poll_state(
    uint64_t current_qpc,
    TriggerInfo& out_trigger,
    uint64_t& out_from_qpc,
    uint64_t& out_to_qpc
) {
    TriggerState current = state_.load(std::memory_order_acquire);

    // Ignore CLAIMED state while metadata is being populated
    if (current == TriggerState::CLAIMED) {
        return false;
    }

    if (current == TriggerState::COLLECTING_POST) {
        if (current_qpc >= post_target_qpc_) {
            frozen_timestamp_qpc_ = current_qpc;
            state_.store(TriggerState::FROZEN, std::memory_order_release);
            current = TriggerState::FROZEN;
        }
    }

    if (current == TriggerState::FROZEN) {
        // Check 5-second watchdog to prevent getting permanently stuck
        if (frozen_timestamp_qpc_ > 0 && (current_qpc - frozen_timestamp_qpc_) > watchdog_qpc_) {
            on_report_completed(current_qpc);
            return false;
        }

        out_trigger = active_trigger_;
        const uint64_t trig_qpc = active_trigger_.trigger_timestamp_qpc;
        out_from_qpc = (trig_qpc > pre_window_qpc_) ? (trig_qpc - pre_window_qpc_) : 0;
        out_to_qpc   = trig_qpc + post_window_qpc_;
        return true;
    }

    if (current == TriggerState::COOLDOWN) {
        if (current_qpc >= cooldown_target_qpc_) {
            state_.store(TriggerState::ARMED, std::memory_order_release);
        }
    }

    return false;
}

void TriggerEngine::on_report_completed(uint64_t current_qpc) noexcept {
    cooldown_target_qpc_ = current_qpc + cooldown_qpc_;
    frozen_timestamp_qpc_ = 0;
    state_.store(TriggerState::COOLDOWN, std::memory_order_release);
}

} // namespace stuttometer
