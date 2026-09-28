## Stuttometer v0.7.0 — Dynamic Cadence Adaptation, Dynamic MPO Deduplication & Robust Hybrid Frame Pacing

Stuttometer v0.7.0 brings major stability, pacing accuracy, and presentation improvements, resolving false-positive triggers during framerate transitions, dynamic refresh rate switches, and dual DXGI/MPO presentation paths.

---

### Key Features & Architectural Improvements

#### 1. Dynamic VBlank-Derived Present Deduplication (MPO Protection)
- **Bounded Dynamic Deduplication:** Replaced the fixed 1.0 ms temporal deduplication window with a dynamic vblank-derived threshold clamped between `DUPLICATE_PRESENT_PATH_FLOOR_US` (2.0 ms) and `DUPLICATE_PRESENT_PATH_CEILING_US` (3.0 ms) (50% of vblank interval).
- **High-Refresh & Low-Refresh Protection:** Ensures uncapped framerates on 60 Hz panels are never erroneously tagged as duplicate presents, while preventing MPO API overhead artifacts (~1.7 ms) from polluting pacing baseline ingestion on ultra-high-refresh displays (240 Hz / 360 Hz+).
- **ETW Ingestion Integration:** Connected real-time vblank cadence from the trigger engine directly to `calculate_effective_present_duration` in DXGI event handling.

#### 2. Candidate Cadence Adaptation & Hybrid Trigger Engine Hardening
- **Typed Candidate State Machine:** Introduced `CandidateOutcome` (`SEEDED`, `ACCUMULATED`, `SKIPPED`, `RESET`, `PROMOTED`, `STALLED`) for deterministic handling of sustained framerate transitions (e.g. 144 FPS -> 60 FPS -> 30 FPS cutscenes / gameplay shifts).
- **Grace Band & Noise Resilience:** Implemented candidate tolerance bands with variance-based promotion criteria (`sigma_threshold_ms = clamp(0.10 * mean, 0.5, 5.0)`), allowing games with slight render jitter or dynamic resolution scaling (DRS) to smoothly adapt without triggering false stutters.
- **Clean-Frame Interleaving & Periodic Hitch Protection:** Added clean-frame tolerance counters (`MAX_CLEAN_FRAMES_BEFORE_STALE_CLEAR = 10`) preserving candidate progress during intermittent stutter, while ensuring genuine periodic hitches are reliably flagged.
- **Staleness Sweep & Reseed Policy:** Unpromoted candidate accumulators are automatically swept after 10.0 seconds (`STALE_CANDIDATE_US`) and capped at 1,000 samples to prevent unbounded state stagnation.
- **Accelerated Adaptive Floor Lift:** Shifted the adaptive static floor activation from sample count 8 to 4, completely eliminating warmup false positives during high-refresh startup.
- **Unified Telemetry Feedback:** Populated `effective_mean_ms` across all pacing outcomes for smooth live session benchmark tracking.

#### 3. Data Layout & Zero-Allocation Guarantees
- **Cache-Line Aligned Structs:** Struct `RollingFrameStats` remains strictly 320 bytes (5x 64-byte cache lines) with compile-time `sizeof` and trivial copyability static assertions.
- **Zero Allocations on Ingestion Path:** All candidate tracking, staleness checks, and circular buffer rolling operations maintain zero runtime heap allocations.

---

### Test Suite & Verification

- **43 Frame Pacing Unit Tests (Expanded from 28):** Comprehensive coverage of multi-stage framerate transitions, dynamic mode conversions, noisy transition grace bands, staleness sweeps, 1000-sample reseed policies, and clean frame preservation.
- **16/16 Test Suites Passing 100%:** All test targets (`test_flight_recorder`, `test_correlator`, `test_json_schema`, `test_etw_constants`, `test_etw_session`, `test_kernel_present_tracking`, `test_frame_pacing`, `test_attribution`, `test_frame_timeline`, `test_csv_exporter`, `test_ndjson_writer`, `test_report_serialization_consistency`, `test_card_renderer`, `test_session_benchmark`, `test_cli_args`, and `test_gui_settings`) passing in Release mode.
