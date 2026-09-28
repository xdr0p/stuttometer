#pragma once
#include <cstdint>

namespace stuttometer {

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
