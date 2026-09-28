#pragma once
#include <cstddef>

namespace stuttometer::dxgkrnl_layout::task17 {
    // Binary offsets into DxgKrnl Task 17 (MMIOFlip) ETW payload
    inline constexpr size_t OFFSET_VIDPN_SOURCE_ID          = 8;
    inline constexpr size_t OFFSET_FLIP_TO_DRIVER_ALLOCATION = 16;
    inline constexpr size_t OFFSET_FLIP_PRESENT_ID          = 36;
    inline constexpr size_t MIN_PAYLOAD_LEN_BASIC           = 24;
    inline constexpr size_t MIN_PAYLOAD_LEN_EXTENDED        = 40;
    // WARNING: Validated against WDDM 2.7–3.2 layouts. Subject to Windows kernel driver shifts.
}
