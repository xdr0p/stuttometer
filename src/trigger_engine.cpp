#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

TriggerEngine::TriggerEngine(const TriggerConfig& config, uint64_t qpc_freq)
    : config_(config)
    , qpc_freq_(qpc_freq)
    , pre_window_qpc_(ms_to_qpc_delta(config.window_pre_ms, qpc_freq))
    , post_window_qpc_(ms_to_qpc_delta(config.window_post_ms, qpc_freq))
    , cooldown_qpc_(ms_to_qpc_delta(config.cooldown_ms, qpc_freq))
{
}

bool TriggerEngine::should_trigger_on_process(uint32_t pid) const {
    if (config_.target_pid != 0 && config_.target_pid != pid) {
        return false;
    }
    if (!config_.target_process_name.empty()) {
        const std::string proc_name = get_process_name_by_pid(pid);
        if (proc_name.find(config_.target_process_name) == std::string::npos) {
            return false;
        }
    }
    return true;
}

void TriggerEngine::initiate_trigger(
    TriggerSource src,
    uint64_t timestamp_qpc,
    double duration_ms,
    uint32_t pid,
    uint32_t tid
) {
    std::lock_guard<std::mutex> lock(trigger_mutex_);
    active_trigger_.source = src;
    active_trigger_.trigger_timestamp_qpc = timestamp_qpc;
    active_trigger_.duration_ms = duration_ms;
    active_trigger_.target_pid = pid;
    active_trigger_.target_tid = tid;
    active_trigger_.target_process = get_process_name_by_pid(pid);

    post_target_qpc_ = timestamp_qpc + post_window_qpc_;

    if (config_.window_post_ms > 0.0) {
        state_.store(TriggerState::COLLECTING_POST, std::memory_order_release);
    } else {
        state_.store(TriggerState::FROZEN, std::memory_order_release);
    }
}

bool TriggerEngine::on_dxgi_present(uint32_t pid, uint32_t tid, double duration_ms, uint64_t timestamp_qpc) {
    if (duration_ms < config_.present_threshold_ms) {
        return false;
    }
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    TriggerState current = state_.load(std::memory_order_acquire);
    if (current == TriggerState::ARMED) {
        initiate_trigger(TriggerSource::DXGI_PRESENT_STUTTER, timestamp_qpc, duration_ms, pid, tid);
        return true;
    } else {
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool TriggerEngine::on_audio_glitch(uint32_t pid, uint32_t tid, uint32_t glitch_count, uint64_t timestamp_qpc) {
    if (!config_.audio_trigger_enabled || glitch_count == 0) {
        return false;
    }
    if (!should_trigger_on_process(pid)) {
        return false;
    }

    TriggerState current = state_.load(std::memory_order_acquire);
    if (current == TriggerState::ARMED) {
        initiate_trigger(TriggerSource::AUDIO_GLITCH, timestamp_qpc, static_cast<double>(glitch_count), pid, tid);
        return true;
    } else {
        suppressed_triggers_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool TriggerEngine::poll_state(
    uint64_t current_qpc,
    TriggerInfo& out_trigger,
    uint64_t& out_from_qpc,
    uint64_t& out_to_qpc
) {
    TriggerState current = state_.load(std::memory_order_acquire);

    if (current == TriggerState::COLLECTING_POST) {
        std::lock_guard<std::mutex> lock(trigger_mutex_);
        if (current_qpc >= post_target_qpc_) {
            state_.store(TriggerState::FROZEN, std::memory_order_release);
            current = TriggerState::FROZEN;
        }
    }

    if (current == TriggerState::FROZEN) {
        std::lock_guard<std::mutex> lock(trigger_mutex_);
        out_trigger = active_trigger_;
        const uint64_t trig_qpc = active_trigger_.trigger_timestamp_qpc;
        out_from_qpc = (trig_qpc > pre_window_qpc_) ? (trig_qpc - pre_window_qpc_) : 0;
        out_to_qpc   = trig_qpc + post_window_qpc_;
        return true;
    }

    if (current == TriggerState::COOLDOWN) {
        std::lock_guard<std::mutex> lock(trigger_mutex_);
        if (current_qpc >= cooldown_target_qpc_) {
            state_.store(TriggerState::ARMED, std::memory_order_release);
        }
    }

    return false;
}

void TriggerEngine::on_report_completed(uint64_t current_qpc) {
    std::lock_guard<std::mutex> lock(trigger_mutex_);
    cooldown_target_qpc_ = current_qpc + cooldown_qpc_;
    state_.store(TriggerState::COOLDOWN, std::memory_order_release);
}

} // namespace stuttometer
