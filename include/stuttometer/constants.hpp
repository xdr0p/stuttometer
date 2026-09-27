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

// Temporal deduplication threshold for duplicate present paths (e.g. standard DXGI + MPO on same frame).
// A sub-1 ms inter-frame delta is physically implausible for a real frame even at extreme refresh rates
// (240 Hz = ~4.17 ms). The MPO intra-frame artifact is ~0.3 ms. Any Stop-to-Stop delta on the same
// swapchain below this value is classified as a duplicate present path and excluded from pacing ingestion.
constexpr uint64_t DUPLICATE_PRESENT_PATH_MAX_US = 1000; // 1.0 ms

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

} // namespace stuttometer
