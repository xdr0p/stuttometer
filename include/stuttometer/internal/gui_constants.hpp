#pragma once
#include <cstdint>
#include <cstddef>

namespace stuttometer::gui_constants {
    inline constexpr uint32_t BUTTON_FEEDBACK_MS            = 1500;
    inline constexpr uint32_t BENCHMARK_REFRESH_MS          = 1000;
    inline constexpr uint32_t OSD_ANIMATION_TIMER_MS        = 16;
    inline constexpr uint32_t SESSION_LOOP_SLEEP_MS         = 10;
    inline constexpr uint32_t METRICS_UPDATE_INTERVAL_LOOPS = 20;
    inline constexpr uint32_t VERBOSE_LOG_INTERVAL_LOOPS    = 500;
    inline constexpr size_t   MAX_ENGINE_LOGS               = 200;
    inline constexpr int      MIN_WINDOW_DIMENSION_PX       = 100;
}
