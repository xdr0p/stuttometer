#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <type_traits>
#include <string_view>
#include <optional>
#include "privilege_utils.hpp"
#include "constants.hpp"
#include "stuttometer/internal/judder_thresholds.hpp"

namespace stuttometer {

namespace pacing_tuning {
    inline constexpr double   CONSISTENCY_FLOOR_MS       = 1.5;
    inline constexpr double   CONSISTENCY_RATIO          = 0.15;
    inline constexpr double   CONSISTENCY_REJECT_FACTOR  = 2.0;
    inline constexpr double   SIGMA_SCALE                = 0.10;
    inline constexpr double   SIGMA_FLOOR_MS             = 0.5;
    inline constexpr double   SIGMA_CEIL_MS              = 5.0;
    inline constexpr uint32_t CANDIDATE_MAX_SAMPLES      = 1000;
    inline constexpr uint32_t PROMOTION_MIN_SAMPLES      = 60;
    inline constexpr uint32_t POST_WARMUP_MIN_SAMPLES    = 8;  // sample_count >= 8: baseline active
    inline constexpr uint32_t INITIAL_WARMUP_SAMPLES     = 4;  // sample_count < 4: catastrophic-only gate
    inline constexpr double   DELTA_SCALE_FACTOR         = 0.3;
    inline constexpr double   CATASTROPHIC_FACTOR        = 12.0;
    // CATASTROPHIC_FLOOR_MS and CATASTROPHIC_CEIL_MS are hardcoded constants rather than
    // display-derived fractions. They establish absolute boundaries for the early-warmup
    // catastrophic stall detector (sample_count < 4) independent of display cadence, ensuring
    // high-refresh displays (e.g. 500 Hz) do not suffer from an overly narrow warmup gate.
    inline constexpr double   CATASTROPHIC_FLOOR_MS      = 50.0;
    inline constexpr double   CATASTROPHIC_CEIL_MS       = 500.0;
    inline constexpr double   DEFAULT_JUDDER_SWING_RATIO = 0.35;
}

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

enum class ReportSeverity : uint8_t {
    ALL     = 0,  // Forensic
    WARNING = 1,  // Balanced, Competitive
    DANGER  = 2   // Conservative, OSD default
};

inline std::string_view report_severity_to_string(ReportSeverity s) noexcept {
    switch (s) {
        case ReportSeverity::ALL:     return "all";
        case ReportSeverity::WARNING: return "warning";
        case ReportSeverity::DANGER:  return "danger";
    }
    return "warning";
}

inline ReportSeverity report_severity_from_string(std::string_view s) noexcept {
    if (s == "all")     return ReportSeverity::ALL;
    if (s == "warning") return ReportSeverity::WARNING;
    if (s == "danger")  return ReportSeverity::DANGER;
    return ReportSeverity::WARNING;
}

enum class PacingProfile : uint8_t {
    AUTO_ADAPTIVE = 0,
    HIGH_REFRESH  = 1,
    CONSERVATIVE  = 2,
    CUSTOM        = 3
};

inline constexpr double DEFAULT_SPIKE_MULTIPLIER      = 2.0;
inline constexpr double DEFAULT_MIN_SPIKE_DELTA_MS    = 4.0;
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

// Accepts CLI-canonical and GUI-serialized aliases; returns nullopt on unrecognized input.
// Note: "custom" is deliberately rejected on CLI/cli_string (users specify overrides or --preset custom).
inline std::optional<PacingProfile> pacing_profile_from_cli_string(std::string_view s) noexcept {
    if (s == "auto" || s == "auto_adaptive" || s == "auto-adaptive") {
        return PacingProfile::AUTO_ADAPTIVE;
    }
    if (s == "high-refresh" || s == "high_refresh") {
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
    const double delta = std::clamp(mean_ms * pacing_tuning::DELTA_SCALE_FACTOR, HIGH_REFRESH_MIN_DELTA_MS, CONSERVATIVE_MIN_DELTA_MS);

    return { mult, delta };
}

[[nodiscard]] inline AdaptivePacingParams resolve_pacing_params(
    PacingProfile profile,
    double baseline_mean_ms,
    double custom_mult,
    double custom_delta
) noexcept {
    switch (profile) {
        case PacingProfile::AUTO_ADAPTIVE:
            return compute_adaptive_pacing_params(baseline_mean_ms);
        case PacingProfile::HIGH_REFRESH:
            return { HIGH_REFRESH_SPIKE_MULTIPLIER, HIGH_REFRESH_MIN_DELTA_MS };
        case PacingProfile::CONSERVATIVE:
            return { CONSERVATIVE_SPIKE_MULTIPLIER, CONSERVATIVE_MIN_DELTA_MS };
        case PacingProfile::CUSTOM:
        default:
            return { custom_mult, custom_delta };
    }
}

enum class CandidateOutcome : uint8_t {
    NONE         = 0,
    SEEDED       = 1,
    ACCUMULATED  = 2,
    SKIPPED      = 3,
    RESET        = 4,
    PROMOTED     = 5,
    STALLED      = 6
};

// 64-slot lock-free circular buffer with candidate adaptation and judder episode tracking
struct alignas(64) RollingFrameStats {
    uint32_t durations_us[64]{0};                       // Bytes 0..255   (Cache lines 0-3)
    uint64_t sum_dur_us{0};                             // Bytes 256..263 (Cache line 4 start)
    uint64_t sum_sq_dur_us{0};                          // Bytes 264..271
    uint64_t last_frame_timestamp_qpc{0};               // Bytes 272..279
    int32_t  last_delta_us{0};                          // Bytes 280..283
    uint16_t sample_count{0};                           // Bytes 284..285
    uint16_t write_idx{0};                              // Bytes 286..287
    uint8_t  alternating_cadence_count{0};              // Byte  288
    uint8_t  candidate_consecutive_skips{0};            // Byte  289
    uint16_t candidate_count{0};                        // Bytes 290..291 (2-byte aligned)
    uint32_t candidate_clean_frames_since_last_match{0};// Bytes 292..295
    uint64_t candidate_first_qpc{0};                    // Bytes 296..303
    uint64_t candidate_sum_us{0};                       // Bytes 304..311
    uint64_t candidate_sum_sq_us{0};                    // Bytes 312..319
    uint8_t  judder_episode_active{0};                  // Byte  320 (Cache line 5 start)
    uint8_t  _judder_reserved_0{0};                     // Byte  321
    uint16_t judder_episode_alternations{0};            // Bytes 322..323
    uint16_t judder_episode_max_swing_q100{0};          // Bytes 324..325
    uint16_t _judder_pad{0};                            // Bytes 326..327
    uint64_t judder_episode_start_qpc{0};               // Bytes 328..335
    uint64_t judder_episode_last_alt_qpc{0};            // Bytes 336..343
    uint64_t _judder_reserved[5]{};                     // Bytes 344..383 (40 bytes)
};
static_assert(sizeof(RollingFrameStats) == 384, "RollingFrameStats must be exactly 384 bytes (6 cache lines)");
static_assert(offsetof(RollingFrameStats, judder_episode_active) == 320);
static_assert(offsetof(RollingFrameStats, judder_episode_start_qpc) == 328);
static_assert(offsetof(RollingFrameStats, judder_episode_last_alt_qpc) == 336);
static_assert(std::is_trivially_copyable_v<RollingFrameStats>, "RollingFrameStats must be trivially copyable");

inline void reset_cadence_state(RollingFrameStats& stats) noexcept {
    if (stats.judder_episode_active == 0) {
        stats.last_delta_us = 0;
    }
    stats.alternating_cadence_count = 0;
}

// Clears all four candidate accumulator fields. Used at 7 sites: reset_frame_stats,
// pause reset, promotion success, inconsistent-candidate rejection, CADENCE_JUDDER trigger,
// STATIC_THRESHOLD fallback, and clean-frame tail.
inline void clear_cadence_candidate(RollingFrameStats& stats) noexcept {
    stats.candidate_consecutive_skips = 0;
    stats.candidate_clean_frames_since_last_match = 0;
    stats.candidate_count     = 0;
    stats.candidate_sum_us    = 0;
    stats.candidate_sum_sq_us = 0;
    stats.candidate_first_qpc = 0;
}

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
    stats.candidate_consecutive_skips = 0;
    stats.candidate_clean_frames_since_last_match = 0;
    clear_cadence_candidate(stats);
    stats.judder_episode_active = 0;
    stats.judder_episode_alternations = 0;
    stats.judder_episode_max_swing_q100 = 0;
    stats.judder_episode_start_qpc = 0;
    stats.judder_episode_last_alt_qpc = 0;
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

[[nodiscard]] inline double resolve_candidate_mean_ms(const RollingFrameStats& stats) noexcept {
    if (stats.candidate_count == 0) return 0.0;
    return (static_cast<double>(stats.candidate_sum_us) / stats.candidate_count) / 1000.0;
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
inline void push_clean_frame(RollingFrameStats& stats, uint32_t dur_us, uint64_t qpc_ts, double swing_ratio = pacing_tuning::DEFAULT_JUDDER_SWING_RATIO) noexcept {
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

// Clamps dur_ms into [1 µs, max_us] and forwards to push_clean_frame.
// The default max_us of WARMUP_CLAMP_US matches the warmup-phase call sites;
// the post-warmup clean-frame tail passes DEFENSIVE_DURATION_CLAMP_US explicitly.
inline void push_clamped_clean_frame(
    RollingFrameStats& stats,
    double dur_ms,
    uint64_t timestamp_qpc,
    double swing_ratio,
    double max_us = WARMUP_CLAMP_US
) noexcept {
    const double clamped_us = std::clamp(dur_ms * 1000.0, 1.0, max_us);
    push_clean_frame(stats, static_cast<uint32_t>(clamped_us), timestamp_qpc, swing_ratio);
}

constexpr uint8_t MAX_CONSECUTIVE_SKIPS = 10;
constexpr uint32_t MAX_CLEAN_FRAMES_BEFORE_STALE_CLEAR = 10;

// Accumulates stutter-candidate frames and promotes the candidate to the rolling
// baseline when 60+ consistent samples with σ < 1.0 ms have been observed and the
// static gate condition is satisfied.
//
// Returns CandidateOutcome: PROMOTED on promotion, in which case out_promoted_avg_ms
// holds the new baseline mean in milliseconds and stats.candidate_* have been cleared.
// Returns SEEDED, ACCUMULATED, SKIPPED, RESET, or STALLED otherwise.
// The caller must NOT modify res.baseline_* unless outcome == PROMOTED.
//
// The caller is responsible for setting res.is_stutter / res.reason; this helper
// touches only the candidate accumulator fields and (on promotion) the rolling
// baseline fields. It does NOT touch stats.last_delta_us or
// stats.alternating_cadence_count — those are cadence-detector state, reset by
// the caller on every stutter.
//
// Uses sum-of-squares variance estimator (not Welford — layout-constrained).
[[nodiscard]] inline CandidateOutcome process_cadence_candidate(
    RollingFrameStats& stats,
    double dur_ms,
    uint64_t timestamp_qpc,
    double& out_promoted_avg_ms
) noexcept {
    stats.candidate_clean_frames_since_last_match = 0;
    const uint32_t dur_us =
        static_cast<uint32_t>(std::clamp(dur_ms * 1000.0, 1.0, DEFENSIVE_DURATION_CLAMP_US));

    // Seed case: first frame of a new candidate accumulation.
    if (stats.candidate_count == 0) {
        stats.candidate_first_qpc  = timestamp_qpc;
        stats.candidate_sum_us     = dur_us;
        stats.candidate_sum_sq_us  = static_cast<uint64_t>(dur_us) * dur_us;
        stats.candidate_count      = 1;
        stats.candidate_consecutive_skips = 0;
        return CandidateOutcome::SEEDED;
    }

    // Consistency check & tolerance triage (D5):
    const double cand_mean_us = static_cast<double>(stats.candidate_sum_us) / stats.candidate_count;
    const double cand_mean_ms = cand_mean_us / 1000.0;
    const double tol_ms       = std::max(pacing_tuning::CONSISTENCY_FLOOR_MS, cand_mean_ms * pacing_tuning::CONSISTENCY_RATIO);
    const double dev_ms       = std::abs(dur_ms - cand_mean_ms);

    if (dev_ms > pacing_tuning::CONSISTENCY_REJECT_FACTOR * tol_ms) {
        clear_cadence_candidate(stats);
        return CandidateOutcome::RESET;
    }

    if (dev_ms > tol_ms) {
        if (++stats.candidate_consecutive_skips > MAX_CONSECUTIVE_SKIPS) {
            clear_cadence_candidate(stats);
            return CandidateOutcome::STALLED;
        }
        return CandidateOutcome::SKIPPED;
    }

    stats.candidate_consecutive_skips = 0;

    // Cap policy: clear and reseed when reaching 1000 without promotion (D8).
    if (stats.candidate_count >= pacing_tuning::CANDIDATE_MAX_SAMPLES) {
        clear_cadence_candidate(stats);
        stats.candidate_first_qpc  = timestamp_qpc;
        stats.candidate_sum_us     = dur_us;
        stats.candidate_sum_sq_us  = static_cast<uint64_t>(dur_us) * dur_us;
        stats.candidate_count      = 1;
        return CandidateOutcome::SEEDED;
    }

    stats.candidate_sum_us    += dur_us;
    stats.candidate_sum_sq_us += static_cast<uint64_t>(dur_us) * dur_us;
    ++stats.candidate_count;

    // Promotion threshold not yet reached.
    if (stats.candidate_count < pacing_tuning::PROMOTION_MIN_SAMPLES) {
        return CandidateOutcome::ACCUMULATED;
    }

    const double mean_us    = static_cast<double>(stats.candidate_sum_us) / stats.candidate_count;
    const double mean_sq_us = static_cast<double>(stats.candidate_sum_sq_us) / stats.candidate_count;
    const double var_us     = std::max(0.0, mean_sq_us - (mean_us * mean_us));
    const double sigma_ms   = std::sqrt(var_us) / 1000.0;

    const double sigma_threshold_ms = std::clamp(pacing_tuning::SIGMA_SCALE * (mean_us / 1000.0), pacing_tuning::SIGMA_FLOOR_MS, pacing_tuning::SIGMA_CEIL_MS);
    if (sigma_ms >= sigma_threshold_ms || (mean_us / 1000.0) >= CANDIDATE_SANITY_CEILING_MS) {
        return CandidateOutcome::ACCUMULATED;
    }

    // Promote candidate → baseline.
    const uint32_t seed_us = static_cast<uint32_t>(mean_us);
    for (size_t i = 0; i < 64; ++i) {
        stats.durations_us[i] = seed_us;
    }
    stats.sum_dur_us    = static_cast<uint64_t>(seed_us) * 64ULL;
    stats.sum_sq_dur_us = static_cast<uint64_t>(seed_us) * seed_us * 64ULL;
    stats.sample_count  = 64;
    stats.write_idx     = 0;

    clear_cadence_candidate(stats);

    out_promoted_avg_ms = mean_us / 1000.0;
    return CandidateOutcome::PROMOTED;
}

struct FramePacingResult {
    bool is_stutter{false};
    TriggerReason reason{TriggerReason::NONE};
    double baseline_avg_ms{0.0};
    double baseline_fps{0.0};
    double spike_ratio{0.0};
    double effective_mean_ms{0.0};
    uint16_t judder_alternations{0};
    uint16_t judder_max_swing_q100{0};
    uint64_t trigger_timestamp_qpc{0};
    double   duration_ms{0.0};
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
    PacingProfile profile = PacingProfile::CUSTOM,
    uint8_t judder_min_alternations = 3
) noexcept {
    judder_min_alternations = std::max<uint8_t>(1, judder_min_alternations);
    FramePacingResult res{};

    bool pause_reset_occurred = false;
    // Check 2.0s pause ceiling (loading screens / Alt-Tab)
    if (stats.last_frame_timestamp_qpc > 0 && timestamp_qpc > stats.last_frame_timestamp_qpc) {
        const uint64_t delta_qpc = timestamp_qpc - stats.last_frame_timestamp_qpc;
        const double delta_us = qpc_delta_to_us(delta_qpc, qpc_freq);
        if (delta_us >= static_cast<double>(PAUSE_CEILING_US)) { // 2.0s scene transition / pause ceiling
            reset_frame_stats(stats, timestamp_qpc);
            pause_reset_occurred = true;
        }
    }
    // Maintain last frame timestamp across all frames (only advance forward in time)
    if (timestamp_qpc > stats.last_frame_timestamp_qpc || stats.last_frame_timestamp_qpc == 0) {
        stats.last_frame_timestamp_qpc = timestamp_qpc;
    }

    if (pause_reset_occurred) {
        clear_cadence_candidate(stats);
        // Post-pause frame: seed baseline without evaluating as a stutter or polluting with pause duration
        push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
        res.effective_mean_ms = calculate_mean_ms(stats);
        return res;
    }

    // Staleness sweep: clear unpromoted candidate accumulator if elapsed QPC time > 10.0 s (D6b)
    if (stats.candidate_count > 0 && stats.candidate_count < 60 &&
        stats.candidate_first_qpc > 0 && timestamp_qpc > stats.candidate_first_qpc) {
        const uint64_t cand_delta_qpc = timestamp_qpc - stats.candidate_first_qpc;
        const double cand_delta_us = qpc_delta_to_us(cand_delta_qpc, qpc_freq);
        if (cand_delta_us > static_cast<double>(STALE_CANDIDATE_US)) {
            clear_cadence_candidate(stats);
        }
    }

    const double mean_ms = calculate_mean_ms(stats);
    res.baseline_avg_ms = mean_ms;
    res.baseline_fps = (mean_ms > 0.0) ? (1000.0 / mean_ms) : 0.0;
    res.spike_ratio = (mean_ms > 0.0) ? (dur_ms / mean_ms) : 1.0;
    res.effective_mean_ms = (stats.candidate_count >= 2) ? resolve_candidate_mean_ms(stats) : mean_ms;

    const auto params = resolve_pacing_params(profile, mean_ms, spike_multiplier, min_spike_delta_ms);
    spike_multiplier = params.spike_multiplier;
    min_spike_delta_ms = params.min_spike_delta_ms;

    // In HYBRID mode, once the baseline is established (>= 4 samples), lift the static ceiling above
    // the observed mean if and only if the threshold would otherwise fire on normal-cadence frames.
    // This corrects vblank-derived thresholds that sit below the game's actual frame time
    // (e.g. effective_static = 5.25 ms at 200 Hz vs ~8.5 ms actual for a 120 FPS title).
    // Placed before dynamic checks so both candidate promotion and static fallback share the adjusted ceiling.
    if (mode == FrameTriggerMode::HYBRID && stats.sample_count >= 4 &&
        effective_static_threshold_ms < mean_ms) {
        effective_static_threshold_ms = std::max(
            effective_static_threshold_ms,
            mean_ms + min_spike_delta_ms
        );
    }

    if (stats.sample_count < pacing_tuning::POST_WARMUP_MIN_SAMPLES) {
        if (mode == FrameTriggerMode::STATIC_ONLY) {
            // STATIC_ONLY: Evaluates static threshold immediately from frame 0
            if (dur_ms >= effective_static_threshold_ms) {
                res.is_stutter = true;
                res.reason = TriggerReason::STATIC_THRESHOLD;
                reset_cadence_state(stats);
                res.effective_mean_ms = res.baseline_avg_ms;
                return res;
            }
            push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
            res.effective_mean_ms = calculate_mean_ms(stats);
            return res;
        } else if (mode == FrameTriggerMode::HYBRID) {
            bool is_warmup_stall = false;
            bool static_trigger  = false;

            if (stats.sample_count < pacing_tuning::INITIAL_WARMUP_SAMPLES) {
                // Initial 4 frames: suppress static trigger entirely to let the baseline
                // stabilize. Only truly catastrophic frames (>= 12x base present time,
                // clamped to [50 ms, 500 ms]) are clamped and pushed so warmup can
                // complete; ordinary slow frames are pushed unclamped.
                const double base_present_ms =
                    invert_effective_static_threshold(effective_static_threshold_ms);
                const double catastrophic_cutoff_ms = std::clamp(pacing_tuning::CATASTROPHIC_FACTOR * base_present_ms, pacing_tuning::CATASTROPHIC_FLOOR_MS, pacing_tuning::CATASTROPHIC_CEIL_MS);

                if (dur_ms >= catastrophic_cutoff_ms) {
                    is_warmup_stall = true; // Catastrophic stall during early warmup: suppress trigger, advance baseline
                }
            } else if (dur_ms >= effective_static_threshold_ms) {
                // sample_count in [4, 7]: baseline has >= 4 valid samples; static
                // triggers are active. Push a clamped sample BEFORE returning so
                // sample_count advances and warmup can complete.
                is_warmup_stall = true;     // Static stall during late warmup: advance baseline AND fire trigger
                static_trigger  = true;
            }

            if (is_warmup_stall) {
                push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
                reset_cadence_state(stats);
                if (static_trigger) {
                    res.is_stutter = true;
                    res.reason     = TriggerReason::STATIC_THRESHOLD;
                }
                res.effective_mean_ms = calculate_mean_ms(stats);
                return res;
            }

            // Normal warmup frame: push with the 100 ms clamp.
            push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
            res.effective_mean_ms = calculate_mean_ms(stats);
            return res;
        } else if (mode == FrameTriggerMode::DYNAMIC_ONLY) {
            // DYNAMIC_ONLY: Suppress static trigger; push a clamped sample so warmup can complete.
            // Without pushing, a display where every frame >= threshold keeps sample_count=0 forever
            // (e.g. 200 Hz display with 5 ms threshold, 8 ms actual frames -> permanent starvation).
            if (dur_ms >= effective_static_threshold_ms) {
                push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
                reset_cadence_state(stats);
                res.effective_mean_ms = calculate_mean_ms(stats);
                return res; // Still suppress the trigger, but populate the baseline
            }
            push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio);
            res.effective_mean_ms = calculate_mean_ms(stats);
            return res;
        }
    }

    // 1. Dynamic Relative Spike Check
    if (mode == FrameTriggerMode::HYBRID || mode == FrameTriggerMode::DYNAMIC_ONLY) {
        if (res.spike_ratio >= spike_multiplier && (dur_ms - mean_ms) >= min_spike_delta_ms) {
            // Reset cadence/idle state prior to candidate processing
            reset_cadence_state(stats);
            stats.candidate_clean_frames_since_last_match = 0;

            double promoted_avg_ms = 0.0;
            const auto outcome = process_cadence_candidate(stats, dur_ms, timestamp_qpc, promoted_avg_ms);
            switch (outcome) {
                case CandidateOutcome::PROMOTED:
                    res.is_stutter      = false;
                    res.reason          = TriggerReason::NONE;
                    res.baseline_avg_ms = promoted_avg_ms;
                    res.baseline_fps    = (promoted_avg_ms > 0.0) ? (1000.0 / promoted_avg_ms) : 0.0;
                    res.spike_ratio     = 1.0;
                    break;
                case CandidateOutcome::ACCUMULATED:
                case CandidateOutcome::SKIPPED:
                    res.is_stutter      = false;
                    res.reason          = TriggerReason::NONE;
                    break;
                case CandidateOutcome::SEEDED:
                case CandidateOutcome::RESET:
                case CandidateOutcome::STALLED:
                default:
                    res.is_stutter      = true;
                    res.reason          = TriggerReason::RELATIVE_SPIKE;
                    break;
            }
            // effective_mean_ms is populated unconditionally; consumed downstream on is_stutter == false paths
            res.effective_mean_ms = (stats.candidate_count >= 2) ? resolve_candidate_mean_ms(stats) : res.baseline_avg_ms;
            return res;
        }

        // 2. Cadence Judder Block:
        if (enable_judder && stats.sample_count > 0) {
            const auto cadence = calculate_cadence_delta(
                stats, static_cast<uint32_t>(dur_ms * 1000.0), judder_swing_ratio);
            const uint64_t ts = timestamp_qpc;

            if (cadence.is_alternating) {
                if (stats.judder_episode_alternations < 0xFFFF) {
                    ++stats.judder_episode_alternations;
                }
                if (stats.judder_episode_alternations == 1) {
                    stats.judder_episode_active = 1;
                    stats.judder_episode_start_qpc = ts;
                }
                stats.judder_episode_last_alt_qpc = ts;

                if (stats.sum_dur_us > 0 && stats.sample_count > 0) {
                    const double mean_us = static_cast<double>(stats.sum_dur_us) / stats.sample_count;
                    const double swing_ratio =
                        std::abs(static_cast<double>(cadence.delta_us)) / mean_us;
                    const uint32_t swing_q100 = static_cast<uint32_t>(
                        std::min(99999.0, swing_ratio * 100.0));
                    const uint16_t clamped =
                        static_cast<uint16_t>(std::min<uint32_t>(65535u, swing_q100));
                    if (clamped > stats.judder_episode_max_swing_q100) {
                        stats.judder_episode_max_swing_q100 = clamped;
                    }
                }

                const uint64_t ep_dur_qpc =
                    (stats.judder_episode_last_alt_qpc >= stats.judder_episode_start_qpc)
                        ? (stats.judder_episode_last_alt_qpc - stats.judder_episode_start_qpc) : 0;
                const double ep_dur_ms = qpc_delta_to_ms(ep_dur_qpc, qpc_freq);

                if (ep_dur_ms >= judder_thresholds::JUDDER_EPISODE_DURATION_CAP_MS) {
                    if (stats.judder_episode_alternations >= judder_min_alternations) {
                        res.is_stutter = true;
                        res.reason = TriggerReason::CADENCE_JUDDER;
                        res.judder_alternations = stats.judder_episode_alternations;
                        res.judder_max_swing_q100 = stats.judder_episode_max_swing_q100;
                        res.duration_ms = ep_dur_ms;
                        res.trigger_timestamp_qpc = (stats.judder_episode_last_alt_qpc > 0)
                            ? stats.judder_episode_last_alt_qpc : ts;
                        res.baseline_avg_ms = calculate_mean_ms(stats);
                        res.baseline_fps = (res.baseline_avg_ms > 0.0)
                            ? 1000.0 / res.baseline_avg_ms : 0.0;
                        res.spike_ratio = 1.0;
                    }
                    stats.judder_episode_active = 0;
                    stats.judder_episode_alternations = 0;
                    stats.judder_episode_max_swing_q100 = 0;
                    stats.judder_episode_start_qpc = 0;
                    stats.judder_episode_last_alt_qpc = 0;
                    stats.last_delta_us = 0;
                    stats.alternating_cadence_count = 0;
                    if (res.is_stutter) {
                        res.effective_mean_ms = res.baseline_avg_ms;
                        return res;
                    }
                }
            } else {
                if (stats.judder_episode_active) {
                    const uint64_t gap_qpc = (ts >= stats.judder_episode_last_alt_qpc)
                        ? (ts - stats.judder_episode_last_alt_qpc) : 0;
                    const double gap_ms = qpc_delta_to_ms(gap_qpc, qpc_freq);

                    const uint64_t ep_dur_qpc =
                        (stats.judder_episode_last_alt_qpc >= stats.judder_episode_start_qpc)
                            ? (stats.judder_episode_last_alt_qpc - stats.judder_episode_start_qpc) : 0;
                    const double ep_dur_ms = qpc_delta_to_ms(ep_dur_qpc, qpc_freq);

                    const bool close_by_gap = (gap_ms >= judder_thresholds::JUDDER_EPISODE_GAP_CLOSE_MS);
                    const bool close_by_cap = (ep_dur_ms >= judder_thresholds::JUDDER_EPISODE_DURATION_CAP_MS);

                    if (close_by_gap || close_by_cap) {
                        if (stats.judder_episode_alternations >= judder_min_alternations) {
                            res.is_stutter = true;
                            res.reason = TriggerReason::CADENCE_JUDDER;
                            res.judder_alternations = stats.judder_episode_alternations;
                            res.judder_max_swing_q100 = stats.judder_episode_max_swing_q100;
                            res.duration_ms = ep_dur_ms;
                            res.trigger_timestamp_qpc = (stats.judder_episode_last_alt_qpc > 0)
                                ? stats.judder_episode_last_alt_qpc : ts;
                            res.baseline_avg_ms = calculate_mean_ms(stats);
                            res.baseline_fps = (res.baseline_avg_ms > 0.0)
                                ? 1000.0 / res.baseline_avg_ms : 0.0;
                            res.spike_ratio = 1.0;
                        }
                        stats.judder_episode_active = 0;
                        stats.judder_episode_alternations = 0;
                        stats.judder_episode_max_swing_q100 = 0;
                        stats.judder_episode_start_qpc = 0;
                        stats.judder_episode_last_alt_qpc = 0;
                        stats.last_delta_us = 0;
                        stats.alternating_cadence_count = 0;
                        if (res.is_stutter) {
                            res.effective_mean_ms = res.baseline_avg_ms;
                            return res;
                        }
                    }
                }
            }
        }
    }

    // 3. Static Threshold Ceiling Fallback (effective_static_threshold_ms already adjusted above)
    if (mode == FrameTriggerMode::STATIC_ONLY) {
        if (dur_ms >= effective_static_threshold_ms) {
            res.is_stutter = true;
            res.reason = TriggerReason::STATIC_THRESHOLD;
            reset_cadence_state(stats);
            clear_cadence_candidate(stats);
            res.effective_mean_ms = res.baseline_avg_ms;
            return res;
        }
    } else if (mode == FrameTriggerMode::HYBRID) {
        if (dur_ms >= effective_static_threshold_ms) {
            reset_cadence_state(stats);
            stats.candidate_clean_frames_since_last_match = 0;

            double promoted_avg_ms = 0.0;
            const auto outcome = process_cadence_candidate(stats, dur_ms, timestamp_qpc, promoted_avg_ms);
            switch (outcome) {
                case CandidateOutcome::PROMOTED:
                    res.is_stutter      = false;
                    res.reason          = TriggerReason::NONE;
                    res.baseline_avg_ms = promoted_avg_ms;
                    res.baseline_fps    = (promoted_avg_ms > 0.0) ? (1000.0 / promoted_avg_ms) : 0.0;
                    res.spike_ratio     = 1.0;
                    break;
                case CandidateOutcome::ACCUMULATED:
                case CandidateOutcome::SKIPPED:
                    res.is_stutter      = false;
                    res.reason          = TriggerReason::NONE;
                    break;
                case CandidateOutcome::SEEDED:
                case CandidateOutcome::RESET:
                case CandidateOutcome::STALLED:
                default:
                    res.is_stutter      = true;
                    res.reason          = TriggerReason::STATIC_THRESHOLD;
                    break;
            }
            res.effective_mean_ms = (stats.candidate_count >= 2) ? resolve_candidate_mean_ms(stats) : res.baseline_avg_ms;
            return res;
        }
    }

    // Clean frame: push into rolling statistics while preserving candidate (D6, D6a)
    if (stats.candidate_count > 0) {
        if (++stats.candidate_clean_frames_since_last_match > MAX_CLEAN_FRAMES_BEFORE_STALE_CLEAR) {
            clear_cadence_candidate(stats);
        }
    }
    push_clamped_clean_frame(stats, dur_ms, timestamp_qpc, judder_swing_ratio, DEFENSIVE_DURATION_CLAMP_US);
    res.effective_mean_ms = (stats.candidate_count >= 2) ? resolve_candidate_mean_ms(stats) : calculate_mean_ms(stats);
    return res;
}

} // namespace stuttometer
