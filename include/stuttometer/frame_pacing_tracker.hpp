#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <type_traits>
#include <string_view>
#include <optional>
#include "privilege_utils.hpp"

namespace stuttometer {

// Strongly-typed trigger reason identifier
enum class TriggerReason : uint8_t {
    NONE                  = 0,
    STATIC_THRESHOLD      = 1,
    RELATIVE_SPIKE        = 2,
    STATISTICAL_OUTLIER   = 3,
    CADENCE_JUDDER        = 4,
    AUDIO_BUFFER_UNDERRUN = 5,
    DWM_COMPOSITOR_GLITCH = 6
};

inline std::string_view trigger_reason_to_string(TriggerReason r) noexcept {
    switch (r) {
        case TriggerReason::STATIC_THRESHOLD:      return "STATIC_THRESHOLD";
        case TriggerReason::RELATIVE_SPIKE:        return "RELATIVE_SPIKE";
        case TriggerReason::STATISTICAL_OUTLIER:   return "STATISTICAL_OUTLIER";
        case TriggerReason::CADENCE_JUDDER:        return "CADENCE_JUDDER";
        case TriggerReason::AUDIO_BUFFER_UNDERRUN: return "AUDIO_BUFFER_UNDERRUN";
        case TriggerReason::DWM_COMPOSITOR_GLITCH: return "DWM_COMPOSITOR_GLITCH";
        default:                                   return "NONE";
    }
}

enum class FrameTriggerMode : uint8_t {
    HYBRID       = 0, // Dynamic relative spike + cadence judder + static floor/ceiling (Recommended)
    DYNAMIC_ONLY = 1, // Pure rolling baseline relative spike & judder only
    STATIC_ONLY  = 2  // Legacy absolute threshold only
};

inline std::string_view frame_trigger_mode_to_string(FrameTriggerMode m) noexcept {
    switch (m) {
        case FrameTriggerMode::HYBRID:       return "hybrid";
        case FrameTriggerMode::DYNAMIC_ONLY: return "dynamic";
        case FrameTriggerMode::STATIC_ONLY:  return "static";
        default:                             return "hybrid";
    }
}

enum class PacingProfile : uint8_t {
    AUTO_ADAPTIVE = 0,
    HIGH_REFRESH  = 1,
    CONSERVATIVE  = 2,
    CUSTOM        = 3
};

inline constexpr double HIGH_REFRESH_SPIKE_MULTIPLIER = 1.4;
inline constexpr double HIGH_REFRESH_MIN_DELTA_MS     = 1.5;
inline constexpr double CONSERVATIVE_SPIKE_MULTIPLIER = 2.0;
inline constexpr double CONSERVATIVE_MIN_DELTA_MS     = 4.0;

inline std::string_view pacing_profile_to_string(PacingProfile p) noexcept {
    switch (p) {
        case PacingProfile::AUTO_ADAPTIVE: return "auto_adaptive";
        case PacingProfile::HIGH_REFRESH:  return "high_refresh";
        case PacingProfile::CONSERVATIVE:  return "conservative";
        case PacingProfile::CUSTOM:        return "custom";
        default:                           return "auto_adaptive";
    }
}

// Case-insensitive deserializer for JSON and settings.json
inline PacingProfile pacing_profile_from_string(std::string_view s) noexcept {
    auto iequals = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
            if (ca != cb) return false;
        }
        return true;
    };

    if (iequals(s, "auto_adaptive") || iequals(s, "auto-adaptive") || iequals(s, "auto")) {
        return PacingProfile::AUTO_ADAPTIVE;
    }
    if (iequals(s, "high_refresh") || iequals(s, "high-refresh")) {
        return PacingProfile::HIGH_REFRESH;
    }
    if (iequals(s, "conservative")) {
        return PacingProfile::CONSERVATIVE;
    }
    if (iequals(s, "custom")) {
        return PacingProfile::CUSTOM;
    }
    return PacingProfile::AUTO_ADAPTIVE;
}

// Strictly used by CLI parser
inline std::optional<PacingProfile> pacing_profile_from_cli_string(std::string_view s) noexcept {
    if (s == "auto") {
        return PacingProfile::AUTO_ADAPTIVE;
    }
    if (s == "high-refresh") {
        return PacingProfile::HIGH_REFRESH;
    }
    if (s == "conservative") {
        return PacingProfile::CONSERVATIVE;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr inline double calculate_effective_static_threshold(double present_threshold_ms) noexcept {
    const double jitter_guard = std::max(0.5, present_threshold_ms * 0.05);
    return present_threshold_ms + jitter_guard;
}

[[nodiscard]] constexpr inline double invert_effective_static_threshold(double effective_ms) noexcept {
    if (effective_ms <= 0.0) return 0.0;
    constexpr double TRANSITION_EFFECTIVE_MS = 10.5; // 10.0 ms + 0.5 ms clamp
    return (effective_ms <= TRANSITION_EFFECTIVE_MS) 
        ? std::max(0.0, effective_ms - 0.5) 
        : (effective_ms / 1.05);
}

struct AdaptivePacingParams {
    double spike_multiplier{2.0};
    double min_spike_delta_ms{4.0};
};

[[nodiscard]] inline AdaptivePacingParams compute_adaptive_pacing_params(double mean_ms) noexcept {
    if (mean_ms <= 0.0) {
        return { CONSERVATIVE_SPIKE_MULTIPLIER, CONSERVATIVE_MIN_DELTA_MS };
    }
    constexpr double T_240_FPS_MS = 1000.0 / 240.0;
    constexpr double T_60_FPS_MS  = 1000.0 / 60.0;

    const double clamped_t = std::clamp(mean_ms, T_240_FPS_MS, T_60_FPS_MS);
    const double factor = (clamped_t - T_240_FPS_MS) / (T_60_FPS_MS - T_240_FPS_MS);
    const double mult = HIGH_REFRESH_SPIKE_MULTIPLIER + factor * (CONSERVATIVE_SPIKE_MULTIPLIER - HIGH_REFRESH_SPIKE_MULTIPLIER);
    const double delta = std::clamp(mean_ms * 0.3, HIGH_REFRESH_MIN_DELTA_MS, CONSERVATIVE_MIN_DELTA_MS);

    return { mult, delta };
}

// 64-slot lock-free power-of-two circular buffer for stream frame delivery statistics
struct alignas(64) RollingFrameStats {
    uint32_t durations_us[64]{0};
    uint64_t sum_dur_us{0};
    uint64_t sum_sq_dur_us{0};
    uint64_t last_frame_timestamp_qpc{0};
    int32_t  last_delta_us{0};
    uint16_t sample_count{0};
    uint16_t write_idx{0};
    uint8_t  alternating_cadence_count{0};
    uint8_t  _pad[3]{0};
};

static_assert(std::is_trivially_copyable_v<RollingFrameStats>, "RollingFrameStats must be trivially copyable");

inline void reset_frame_stats(RollingFrameStats& stats, uint64_t qpc_ts = 0) noexcept {
    for (size_t i = 0; i < 64; ++i) {
        stats.durations_us[i] = 0;
    }
    stats.sum_dur_us = 0;
    stats.sum_sq_dur_us = 0;
    stats.last_frame_timestamp_qpc = qpc_ts;
    stats.last_delta_us = 0;
    stats.sample_count = 0;
    stats.write_idx = 0;
    stats.alternating_cadence_count = 0;
}

inline double calculate_mean_ms(const RollingFrameStats& stats) noexcept {
    if (stats.sample_count == 0) return 0.0;
    return (static_cast<double>(stats.sum_dur_us) / stats.sample_count) / 1000.0;
}

inline double calculate_stddev_ms(const RollingFrameStats& stats) noexcept {
    if (stats.sample_count < 2) return 0.0;
    const double mean_us = static_cast<double>(stats.sum_dur_us) / stats.sample_count;
    const double mean_sq_us = static_cast<double>(stats.sum_sq_dur_us) / stats.sample_count;
    const double var_us = std::max(0.0, mean_sq_us - (mean_us * mean_us));
    return std::sqrt(var_us) / 1000.0;
}

struct CadenceDeltaResult {
    int32_t delta_us{0};
    bool is_alternating{false};
};

inline CadenceDeltaResult calculate_cadence_delta(
    const RollingFrameStats& stats,
    uint32_t dur_us,
    double swing_ratio
) noexcept {
    if (stats.sample_count == 0) return {};
    const uint16_t prev_idx = (stats.write_idx - 1) & 63;
    const uint32_t prev_dur = stats.durations_us[prev_idx];
    const int32_t delta = static_cast<int32_t>(dur_us) - static_cast<int32_t>(prev_dur);

    const double mean_us = static_cast<double>(stats.sum_dur_us) / stats.sample_count;
    const double swing_threshold_us = mean_us * swing_ratio;

    bool is_alt = false;
    if (std::abs(delta) >= swing_threshold_us && stats.last_delta_us != 0) {
        const bool sign_curr = (delta > 0);
        const bool sign_prev = (stats.last_delta_us > 0);
        is_alt = (sign_curr != sign_prev);
    }
    return { delta, is_alt };
}

// Pushes a non-stuttering clean frame into the rolling window
inline void push_clean_frame(RollingFrameStats& stats, uint32_t dur_us, uint64_t qpc_ts, double swing_ratio = 0.35) noexcept {
    if (dur_us == 0) return;

    if (stats.sample_count > 0) {
        const auto cadence = calculate_cadence_delta(stats, dur_us, swing_ratio);
        if (cadence.is_alternating) {
            if (stats.alternating_cadence_count < 255) {
                ++stats.alternating_cadence_count;
            }
        } else {
            stats.alternating_cadence_count = 0;
        }
        stats.last_delta_us = cadence.delta_us;
    } else {
        stats.last_delta_us = 0;
        stats.alternating_cadence_count = 0;
    }

    // Update circular buffer with O(1) sum adjustments
    if (stats.sample_count == 64) {
        const uint32_t old_val = stats.durations_us[stats.write_idx];
        stats.sum_dur_us -= old_val;
        stats.sum_sq_dur_us -= (static_cast<uint64_t>(old_val) * old_val);
    } else {
        ++stats.sample_count;
    }

    stats.durations_us[stats.write_idx] = dur_us;
    stats.sum_dur_us += dur_us;
    stats.sum_sq_dur_us += (static_cast<uint64_t>(dur_us) * dur_us);
    stats.write_idx = (stats.write_idx + 1) & 63;
    if (qpc_ts > stats.last_frame_timestamp_qpc || stats.last_frame_timestamp_qpc == 0) {
        stats.last_frame_timestamp_qpc = qpc_ts;
    }
}

struct FramePacingResult {
    bool is_stutter{false};
    TriggerReason reason{TriggerReason::NONE};
    double baseline_avg_ms{0.0};
    double baseline_fps{0.0};
    double spike_ratio{0.0};
};

// Evaluates a frame against the stream's rolling statistics
inline FramePacingResult evaluate_frame_pacing(
    RollingFrameStats& stats,
    double dur_ms,
    uint64_t timestamp_qpc,
    uint64_t qpc_freq,
    FrameTriggerMode mode,
    double spike_multiplier,
    double min_spike_delta_ms,
    bool enable_judder,
    double judder_swing_ratio,
    double effective_static_threshold_ms,
    PacingProfile profile = PacingProfile::CUSTOM
) noexcept {
    FramePacingResult res{};

    bool pause_reset_occurred = false;
    // Check 10.0s pause ceiling (loading screens / Alt-Tab)
    if (stats.last_frame_timestamp_qpc > 0 && timestamp_qpc > stats.last_frame_timestamp_qpc) {
        const uint64_t delta_qpc = timestamp_qpc - stats.last_frame_timestamp_qpc;
        const double delta_us = qpc_delta_to_us(delta_qpc, qpc_freq);
        if (delta_us >= 10000000.0) { // 10.0s reset ceiling
            reset_frame_stats(stats, timestamp_qpc);
            pause_reset_occurred = true;
        }
    }
    // Maintain last frame timestamp across all frames (only advance forward in time)
    if (timestamp_qpc > stats.last_frame_timestamp_qpc || stats.last_frame_timestamp_qpc == 0) {
        stats.last_frame_timestamp_qpc = timestamp_qpc;
    }

    if (pause_reset_occurred) {
        // Post-pause frame: seed baseline without evaluating as a stutter or polluting with pause duration
        const double seed_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
        push_clean_frame(stats, static_cast<uint32_t>(seed_us), timestamp_qpc, judder_swing_ratio);
        return res;
    }

    const double mean_ms = calculate_mean_ms(stats);
    res.baseline_avg_ms = mean_ms;
    res.baseline_fps = (mean_ms > 0.0) ? (1000.0 / mean_ms) : 0.0;
    res.spike_ratio = (mean_ms > 0.0) ? (dur_ms / mean_ms) : 1.0;

    if (profile == PacingProfile::AUTO_ADAPTIVE) {
        const auto params = compute_adaptive_pacing_params(mean_ms);
        spike_multiplier = params.spike_multiplier;
        min_spike_delta_ms = params.min_spike_delta_ms;
    } else if (profile == PacingProfile::HIGH_REFRESH) {
        spike_multiplier = HIGH_REFRESH_SPIKE_MULTIPLIER;
        min_spike_delta_ms = HIGH_REFRESH_MIN_DELTA_MS;
    } else if (profile == PacingProfile::CONSERVATIVE) {
        spike_multiplier = CONSERVATIVE_SPIKE_MULTIPLIER;
        min_spike_delta_ms = CONSERVATIVE_MIN_DELTA_MS;
    }

    if (stats.sample_count < 8) {
        if (mode == FrameTriggerMode::STATIC_ONLY) {
            // STATIC_ONLY: Evaluates static threshold immediately from frame 0
            if (dur_ms >= effective_static_threshold_ms) {
                res.is_stutter = true;
                res.reason = TriggerReason::STATIC_THRESHOLD;
                stats.last_delta_us = 0;
                stats.alternating_cadence_count = 0;
                return res;
            }
            const double clamped_warmup_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
            push_clean_frame(stats, static_cast<uint32_t>(clamped_warmup_us), timestamp_qpc, judder_swing_ratio);
            return res;
        } else if (mode == FrameTriggerMode::HYBRID) {
            // HYBRID Warmup:
            if (stats.sample_count < 4) {
                // Suppress static trigger during initial 4 frames to let baseline stabilize.
                const double base_present_ms = invert_effective_static_threshold(effective_static_threshold_ms);
                const double catastrophic_cutoff_ms = std::clamp(12.0 * base_present_ms, 50.0, 500.0);
                if (dur_ms >= catastrophic_cutoff_ms) {
                    // Push a clamped sample so sample_count advances and warmup can complete.
                    // Without this, every catastrophic stall keeps sample_count=0 forever (starvation).
                    const double clamped_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
                    push_clean_frame(stats, static_cast<uint32_t>(clamped_us), timestamp_qpc, judder_swing_ratio);
                    stats.last_delta_us = 0;
                    stats.alternating_cadence_count = 0;
                    return res; // Still suppress the trigger, but populate the baseline
                }
            } else {
                // sample_count in [4, 7]: baseline has >= 4 valid samples; static triggers active.
                if (dur_ms >= effective_static_threshold_ms) {
                    // Push a clamped sample before returning so sample_count advances and warmup
                    // can complete. Without this, a high-refresh display running below refresh
                    // rate (e.g. 200 Hz / 5.25 ms threshold with ~8.5 ms frames) would trip
                    // STATIC_THRESHOLD on every frame in this window, keeping sample_count pinned
                    // at 4 forever and preventing the post-warmup adaptive floor from ever
                    // activating.
                    const double clamped_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
                    push_clean_frame(stats, static_cast<uint32_t>(clamped_us), timestamp_qpc, judder_swing_ratio);
                    res.is_stutter = true;
                    res.reason = TriggerReason::STATIC_THRESHOLD;
                    stats.last_delta_us = 0;
                    stats.alternating_cadence_count = 0;
                    return res;
                }
            }
            // Tighter 100ms clamp during warmup prevents early baseline skew with small sample counts
            const double clamped_warmup_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
            push_clean_frame(stats, static_cast<uint32_t>(clamped_warmup_us), timestamp_qpc, judder_swing_ratio);
            return res;
        } else if (mode == FrameTriggerMode::DYNAMIC_ONLY) {
            // DYNAMIC_ONLY: Suppress static trigger; push a clamped sample so warmup can complete.
            // Without pushing, a display where every frame >= threshold keeps sample_count=0 forever
            // (e.g. 200 Hz display with 5 ms threshold, 8 ms actual frames -> permanent starvation).
            if (dur_ms >= effective_static_threshold_ms) {
                const double clamped_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
                push_clean_frame(stats, static_cast<uint32_t>(clamped_us), timestamp_qpc, judder_swing_ratio);
                stats.last_delta_us = 0;
                stats.alternating_cadence_count = 0;
                return res; // Still suppress the trigger, but populate the baseline
            }
            const double clamped_warmup_us = std::clamp(dur_ms * 1000.0, 1.0, 100000.0);
            push_clean_frame(stats, static_cast<uint32_t>(clamped_warmup_us), timestamp_qpc, judder_swing_ratio);
            return res;
        }
    }

    // 1. Dynamic Relative Spike Check
    if (mode == FrameTriggerMode::HYBRID || mode == FrameTriggerMode::DYNAMIC_ONLY) {
        if (res.spike_ratio >= spike_multiplier && (dur_ms - mean_ms) >= min_spike_delta_ms) {
            res.is_stutter = true;
            res.reason = TriggerReason::RELATIVE_SPIKE;
            // Baseline Pollution Protection: do not push stuttering frame into baseline
            // Reset cadence state so delta is not computed across the stutter gap on next clean frame
            stats.last_delta_us = 0;
            stats.alternating_cadence_count = 0;
            return res;
        }

        // 2. Cadence Judder Check: >= 3 sign alternations (4 consecutive oscillating deltas)
        if (enable_judder && stats.sample_count > 0) {
            const auto cadence = calculate_cadence_delta(stats, static_cast<uint32_t>(dur_ms * 1000.0), judder_swing_ratio);
            if (cadence.is_alternating && (stats.alternating_cadence_count + 1) >= 3) {
                res.is_stutter = true;
                res.reason = TriggerReason::CADENCE_JUDDER;
                stats.last_delta_us = 0;
                stats.alternating_cadence_count = 0;
                return res;
            }
        }
    }

    // 3. Static Threshold Ceiling Fallback
    // In HYBRID mode, once the baseline is established (>= 8 samples), lift the static ceiling above
    // the observed mean if and only if the threshold would otherwise fire on normal-cadence frames.
    // This corrects vblank-derived thresholds that sit below the game's actual frame time
    // (e.g. effective_static = 5.25 ms at 200 Hz vs ~8.5 ms actual for a 120 FPS title).
    // Guard: only raise when threshold < mean (i.e. threshold would fire on a normal-cadence frame).
    // At 60 FPS (mean 16.67 ms, threshold 17.5 ms), threshold already exceeds mean → no adjustment.
    // effective_static_threshold_ms is passed by value; does NOT apply to STATIC_ONLY mode.
    if (mode == FrameTriggerMode::HYBRID && stats.sample_count >= 8 &&
        effective_static_threshold_ms < mean_ms) {
        effective_static_threshold_ms = std::max(
            effective_static_threshold_ms,
            mean_ms + min_spike_delta_ms
        );
    }

    if (mode == FrameTriggerMode::HYBRID || mode == FrameTriggerMode::STATIC_ONLY) {
        if (dur_ms >= effective_static_threshold_ms) {
            res.is_stutter = true;
            res.reason = TriggerReason::STATIC_THRESHOLD;
            stats.last_delta_us = 0;
            stats.alternating_cadence_count = 0;
            return res;
        }
    }

    // Clean frame: push into rolling statistics
    push_clean_frame(stats, static_cast<uint32_t>(std::clamp(dur_ms * 1000.0, 1.0, 10000000.0)), timestamp_qpc, judder_swing_ratio);
    return res;
}

} // namespace stuttometer
