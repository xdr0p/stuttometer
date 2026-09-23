## Stuttometer v0.5.6 — MPO Present Dedup & Warmup Starvation Fix

### Bug Fixes

#### Duplicate Present Path Deduplication
DX12 titles that emit both standard DXGI Present Stop events (IDs 42/43) and MPO
Present Stop events (IDs 55/56) for the same rendered frame produce sub-1ms
inter-Stop deltas on the same swapchain. These were being ingested into the
rolling pacing baseline, collapsing it toward ~0.2 ms and producing
physically implausible FPS readings in the Session Benchmark.

Duplicate Stops are now tagged with `DXGI_DUPLICATE_PRESENT_PATH` (bit 15), excluded
from pacing ingestion and the exported frame timeline, but retained in the flight
recorder and NDJSON stream for raw data fidelity.

#### Warmup Starvation on High-Refresh Displays
On displays where the physical vblank interval sits below the game's actual frame
time (e.g. 200 Hz / 5.25 ms threshold running a 120 FPS / ~8.5 ms title), the
HYBRID warmup branch could pin `sample_count` at 4 indefinitely, preventing the
post-warmup adaptive floor from ever activating. Warmup frames are now pushed as
clamped samples so the baseline can complete and the adaptive floor can engage.

### Test Suite
All 16 unit test suites pass. Latent failures in the kernel-present tracking
suite (previously masked by earlier failures stopping the suite) have been
corrected alongside the source fixes.