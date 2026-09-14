#pragma once
#include <cstdint>

namespace stuttometer::gui {

enum class OsdPosition : uint32_t {
    TOP_RIGHT = 0,
    BOTTOM_RIGHT = 1,
    TOP_LEFT = 2,
    BOTTOM_LEFT = 3
};

} // namespace stuttometer::gui
