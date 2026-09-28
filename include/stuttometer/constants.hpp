#pragma once
#include <cstdint>
#include <cstddef>

namespace stuttometer {

inline constexpr uint64_t PAGE_SIZE_BYTES = 4096ULL;
inline constexpr uint64_t VIRTUAL_ALLOC_MIN_SEVERE_BYTES = 4ULL * 1024 * 1024;
inline constexpr uint32_t MIN_BUFFER_SLOTS = 65536;
inline constexpr uint32_t DEFAULT_BUFFER_SLOTS = 262144;
inline constexpr uint32_t MAX_BUFFER_SLOTS = 1048576;
static_assert(MIN_BUFFER_SLOTS <= DEFAULT_BUFFER_SLOTS && DEFAULT_BUFFER_SLOTS <= MAX_BUFFER_SLOTS,
              "Buffer slot bounds invariant violated");
static_assert((MIN_BUFFER_SLOTS & (MIN_BUFFER_SLOTS - 1)) == 0,
              "MIN_BUFFER_SLOTS must be a power of 2 for bitmask indexing");
static_assert((DEFAULT_BUFFER_SLOTS & (DEFAULT_BUFFER_SLOTS - 1)) == 0,
              "DEFAULT_BUFFER_SLOTS must be a power of 2 for bitmask indexing");
static_assert((MAX_BUFFER_SLOTS & (MAX_BUFFER_SLOTS - 1)) == 0,
              "MAX_BUFFER_SLOTS must be a power of 2 for bitmask indexing");

inline constexpr double   DEFAULT_60HZ_VBLANK_MS = 16.67;
inline constexpr double   TRIGGER_WATCHDOG_MS = 5000.0;
inline constexpr double   VBLANK_WARNING_FACTOR = 2.0;
inline constexpr uint32_t POST_TRIGGER_DRAIN_BUDGET_MS = 30;
inline constexpr uint32_t POST_TRIGGER_DRAIN_STEP_MS = 1;

// ETW Buffer Configuration (uint32_t implicitly converts to ULONG in Win32 APIs without Windows.h dependency)
inline constexpr uint32_t ETW_BUFFER_SIZE_KB   = 128;
inline constexpr uint32_t ETW_MIN_BUFFERS       = 16;
inline constexpr uint32_t ETW_MAX_BUFFERS       = 64;
inline constexpr uint32_t ETW_FLUSH_TIMER_SEC   = 1;

// DWM Glitch Deduplication (size_t matches internal array capacity & bitmask indexing)
inline constexpr double DWM_MIN_GLITCH_DURATION_US = 1000.0;
inline constexpr double DWM_DEDUP_WINDOW_MS        = 50.0;
inline constexpr size_t DWM_DEDUP_BUFFER_SIZE      = 16;
static_assert((DWM_DEDUP_BUFFER_SIZE & (DWM_DEDUP_BUFFER_SIZE - 1)) == 0,
              "DWM_DEDUP_BUFFER_SIZE must be a power of 2 for bitmask indexing");

namespace eviction_age_ms {
    inline constexpr double PRESENT_TABLE          = 5000.0;
    inline constexpr double DISK_TABLE             = 3000.0;
    inline constexpr double SCAN_TABLE             = 12000.0;
    inline constexpr double THREAD_TABLE           = 5000.0;
    inline constexpr double TID_PID_TABLE          = 15000.0;
    inline constexpr double PRESENT_AND_FLIP_TABLE = 30000.0;
    inline constexpr double PSO_TABLE              = 12000.0;
    inline constexpr double WS_TRIM_TABLE          = 12000.0;
}

// 2.0s ceiling: any gap >= 2.0s is a loading screen, cutscene, Alt-Tab, or scene transition
constexpr double   PAUSE_CEILING_MS = 2000.0;
constexpr uint64_t PAUSE_CEILING_US = static_cast<uint64_t>(PAUSE_CEILING_MS * 1000.0);

// Single-event kernel duration caps. DO NOT collapse into PAUSE_CEILING_US (2.0s).
// PAUSE_CEILING bounds INTER-FRAME presentation gaps (loading screens, Alt-Tab,
// scene transitions). These bound INDIVIDUAL kernel routine / driver execution
// times, where a >2s stall IS a genuine hardware fault, not a scene transition.
constexpr uint64_t KERNEL_SINGLE_EVENT_CAP_US = 10'000'000ULL; // 10.0 s: DPC, ISR, CSwitch, Memory, HardFault
constexpr uint64_t DISK_SINGLE_EVENT_CAP_US   =  3'000'000ULL; //  3.0 s: Disk I/O

// Duplicate present path detection bounds (in microseconds).
// The MPO artifact is driver-side API overhead (typically ~1.7 ms) and does NOT scale
// with display cadence. A pure fraction of vblank fails at low refresh rates (would flag
// 120 FPS uncapped on a 60 Hz panel). A pure min() fails at high refresh rates (360 Hz+,
// where vblank * 0.5 drops below the artifact size). Clamp covers both extremes.
inline constexpr uint64_t DUPLICATE_PRESENT_PATH_FLOOR_US        = 2000;  // 2.0 ms floor (high-refresh protection)
inline constexpr uint64_t DUPLICATE_PRESENT_PATH_CEILING_US      = 3000;  // 3.0 ms ceiling (uncapped-low-refresh protection)
inline constexpr double   DUPLICATE_PRESENT_PATH_VBLANK_FRACTION = 0.5;   // 50% of vblank interval
inline constexpr uint64_t DUPLICATE_PRESENT_PATH_DEFAULT_US      = 2000;  // Fallback when vblank is unset

static_assert(DUPLICATE_PRESENT_PATH_FLOOR_US <= DUPLICATE_PRESENT_PATH_CEILING_US,
              "Floor must be <= ceiling for std::clamp");

// Warmup clamp — bounds the contribution of any single warmup frame to the baseline,
// allowing warmup to complete even when every frame exceeds the static threshold
// (prevents sample_count starvation on high-refresh displays running below refresh rate).
constexpr double WARMUP_CLAMP_US = 100000.0; // 100 ms

// Defensive ceiling for individual frame duration (dur_ms) fed to candidate accumulator
// or staged GPU upgrade. Intentionally larger than PAUSE_CEILING_US (2.0 s): PAUSE_CEILING_US
// bounds the wall-clock gap (delta_qpc) between frames to detect Alt-Tab / loading
// screens, whereas this constant bounds the caller-supplied payload duration (dur_ms)
// against malformed ETW telemetry — e.g. a GPU driver reporting a 5 s stall while
// QPC timestamps are only 50 ms apart due to driver-level timing bugs or DPC latency.
// The two quantities are equal in the current DXGI ETW path but the engine does not
// enforce that invariant; do NOT collapse this to PAUSE_CEILING_US.
constexpr double DEFENSIVE_DURATION_CLAMP_US = 10000000.0; // 10.0 s

// Absolute sanity ceiling for candidate cadence adaptation (~5 FPS floor).
// Chosen to be well below any real-time interactive target and well above the pathological-hang
// / pause regime (PAUSE_CEILING_MS = 2000.0) that the sanity ceiling exists to reject.
// Mode-independent: applies to both HYBRID and DYNAMIC_ONLY. Replaces vblank-derived static gate.
constexpr double CANDIDATE_SANITY_CEILING_MS = 200.0;

// Maximum time window an unpromoted candidate accumulator can persist before being cleared as stale.
constexpr uint64_t STALE_CANDIDATE_US = 10'000'000ULL; // 10.0 s

} // namespace stuttometer
