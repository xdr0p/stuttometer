#pragma once
#include <cstddef>

namespace stuttometer::detail::severity {
    inline constexpr double DISK_IO_SCALE_MS           = 60.0;
    inline constexpr double CSWITCH_SCALE_MS          = 20.0;
    inline constexpr double DPC_ISR_SCALE_US          = 3000.0;
    inline constexpr double DWM_REF_VBLANK_MULTIPLIER = 3.0;
    inline constexpr double PAGE_FAULT_SCALE_MS       = 30.0;
    inline constexpr double THERMAL_THROTTLE_SCALE_MS = 20.0;
    inline constexpr double DEFENDER_DURATION_MS      = 50.0;
    inline constexpr double DEFENDER_SCAN_COUNT       = 5.0;
    inline constexpr double PSO_STALL_SCALE_MS        = 30.0;
    inline constexpr double VRAM_OVERCOMMIT_SCALE_MB  = 64.0;
    inline constexpr double VIRTUAL_ALLOC_SCALE_MB    = 64.0;
    inline constexpr double WORKING_SET_TRIM_SCALE_MB = 32.0;
    inline constexpr double PHYS_MEM_DURATION_US      = 10000.0;
    inline constexpr double PHYS_MEM_SIZE_MB          = 32.0;
    inline constexpr double VRAM_BASE_CONFIDENCE      = 0.15;
    inline constexpr double SMI_CAP_WITH_CSWITCH      = 0.35;
    inline constexpr double SMI_CAP_WITHOUT_CSWITCH   = 0.30;

    namespace smi {
        inline constexpr double AUTO_SCALE_FLOOR_MS    = 16.67;
    }

    namespace proximity {
        inline constexpr double DPC_ISR_WINDOW_MS     = 150.0;
        inline constexpr double CSWITCH_WINDOW_MS     = 100.0;
        inline constexpr double VRAM_WINDOW_MS        = 100.0;
        inline constexpr double GENERAL_WINDOW_MS     = 200.0;
    }

    namespace gpu_stall {
        inline constexpr double CONFIDENCE_BASE            = 0.60;
        inline constexpr double DURATION_DIVISOR_MS        = 100.0;
        inline constexpr double CONFIDENCE_BONUS_CAP       = 0.30;
        inline constexpr double DXGI_RELATIVE_PENALTY      = 0.90;
        inline constexpr double MIN_CONFIDENCE             = 0.50;
        inline constexpr double MAX_CONFIDENCE             = 0.90;
        inline constexpr double FACTOR_DURATION_DIVISOR_MS = 50.0;
    }
}
