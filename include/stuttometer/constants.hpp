#pragma once

namespace stuttometer {

constexpr double PAUSE_CEILING_MS = 10000.0;

// Temporal deduplication threshold for duplicate present paths (e.g. standard DXGI + MPO on same frame).
// A sub-1 ms inter-frame delta is physically implausible for a real frame even at extreme refresh rates
// (240 Hz = ~4.17 ms). The MPO intra-frame artifact is ~0.3 ms. Any Stop-to-Stop delta on the same
// swapchain below this value is classified as a duplicate present path and excluded from pacing ingestion.
constexpr uint64_t DUPLICATE_PRESENT_PATH_MAX_US = 1000; // 1.0 ms

} // namespace stuttometer
