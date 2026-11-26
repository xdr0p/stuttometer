#pragma once

#include <cstdint>
#include <cstddef>

namespace stuttometer::transition_thresholds {

// Eligibility gate (full decision ladder)
inline constexpr double MIN_DURATION_MS = 100.0;
inline constexpr double MIN_BASELINE_MULT = 8.0;

// Probe-tier eligibility gate (sub-100 ms menu hitches & adaptive probe window)
inline constexpr double PROBE_MIN_DURATION_MS = 15.0;
inline constexpr double PROBE_MIN_BASELINE_MULT = 3.0;
static_assert(PROBE_MIN_DURATION_MS <= MIN_DURATION_MS, "PROBE_MIN_DURATION_MS must not exceed MIN_DURATION_MS");
static_assert(PROBE_MIN_BASELINE_MULT <= MIN_BASELINE_MULT, "PROBE_MIN_BASELINE_MULT must not exceed MIN_BASELINE_MULT");

// Adaptive probe window (precomputed and static-asserted cap for Phase B)
inline constexpr double PROBE_WINDOW_MS = 350.0;
inline constexpr double PROBE_WINDOW_MAX_MS = 500.0;
static_assert(PROBE_WINDOW_MS <= PROBE_WINDOW_MAX_MS, "PROBE_WINDOW_MS must not exceed PROBE_WINDOW_MAX_MS");

// S1a Impulse confirmation
inline constexpr size_t IMPULSE_MIN_POST_SAMPLES = 4;
inline constexpr double IMPULSE_ABS_TOLERANCE_MS = 1.5;
inline constexpr double IMPULSE_REL_TOLERANCE = 0.20; // 20%

// S1b Regime step
inline constexpr size_t REGIME_MIN_POST_SAMPLES = 6;
inline constexpr double REGIME_STEP_RATIO = 1.5;
inline constexpr double REGIME_MAX_DISPERSION = 0.25; // MAD / median
inline constexpr double REGIME_SANITY_CEILING_MS = 200.0;

// S2 I/O burst
inline constexpr size_t DISK_BURST_MIN_EVENTS = 40;
inline constexpr uint64_t DISK_BURST_MIN_BYTES = 8ULL * 1024ULL * 1024ULL; // 8 MB
// Fraction of disk events required to occur after trigger timestamp (sustained into post-window)
inline constexpr double DISK_BURST_POST_FRACTION = 0.25;
inline constexpr uint64_t DISK_BURST_HEAVY_BYTES = 16ULL * 1024ULL * 1024ULL; // 16 MB for bonus confidence

// S3 Voluntary dominance
inline constexpr double VOLUNTARY_DOMINANCE_FRACTION = 0.80; // 80%

// Confidence scoring
inline constexpr double CONFIDENCE_REGIME_BASE = 0.50;
inline constexpr size_t REGIME_HIGH_SAMPLES_THRESHOLD = 10;
inline constexpr double CONFIDENCE_REGIME_HIGH_SAMPLES_BONUS = 0.10;
inline constexpr double REGIME_LOW_DISPERSION_THRESHOLD = 0.15;
inline constexpr double CONFIDENCE_REGIME_LOW_DISPERSION_BONUS = 0.10;
inline constexpr double CONFIDENCE_REGIME_S4_SYNC_BONUS = 0.15;

inline constexpr double CONFIDENCE_LOADING_BASE = 0.45;
inline constexpr double CONFIDENCE_LOADING_HEAVY_IO_BONUS = 0.15;
inline constexpr double CONFIDENCE_LOADING_VOLUNTARY_BONUS = 0.10;
inline constexpr double CONFIDENCE_LOADING_ELEVATED_POST_BONUS = 0.10;
inline constexpr double CONFIDENCE_LOADING_S4_BONUS = 0.10;

inline constexpr double CONFIDENCE_MAX = 0.95;
inline constexpr double POLICY_CONFIDENCE_GATE = 0.60;

} // namespace stuttometer::transition_thresholds
