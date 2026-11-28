#pragma once

#include <cstdint>

namespace stuttometer::judder_thresholds {

// Judder severity classification thresholds (classify_severity in src/correlator.cpp)
inline constexpr uint16_t JUDDER_DANGER_MIN_ALT          = 30;
inline constexpr double   JUDDER_DANGER_MIN_DUR_MS       = 500.0;

// Judder episode tracking bounds (evaluate_frame_pacing in include/stuttometer/frame_pacing_tracker.hpp)
inline constexpr double   JUDDER_EPISODE_GAP_CLOSE_MS    = 500.0;
inline constexpr double   JUDDER_EPISODE_DURATION_CAP_MS = 5000.0;

} // namespace stuttometer::judder_thresholds
