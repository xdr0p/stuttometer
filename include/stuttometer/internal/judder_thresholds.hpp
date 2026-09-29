#pragma once

#include <cstdint>

namespace stuttometer::judder_thresholds {

// Judder severity classification thresholds (classify_severity in src/correlator.cpp)
inline constexpr uint16_t JUDDER_DANGER_MIN_ALT          = 30;
inline constexpr double   JUDDER_DANGER_MIN_DUR_MS       = 500.0;
inline constexpr uint16_t JUDDER_DANGER_MIN_SWING        = 60;
inline constexpr uint16_t JUDDER_DANGER_SWING_MIN_ALT    = 5;

inline constexpr uint16_t JUDDER_WARNING_MIN_ALT         = 10;
inline constexpr double   JUDDER_WARNING_MIN_DUR_MS      = 150.0;
inline constexpr uint16_t JUDDER_WARNING_MIN_SWING       = 40;
inline constexpr uint16_t JUDDER_WARNING_SWING_MIN_ALT   = 3;

// Judder episode tracking bounds (evaluate_frame_pacing in include/stuttometer/frame_pacing_tracker.hpp)
inline constexpr double   JUDDER_EPISODE_GAP_CLOSE_MS    = 500.0;
inline constexpr double   JUDDER_EPISODE_DURATION_CAP_MS = 5000.0;

} // namespace stuttometer::judder_thresholds
