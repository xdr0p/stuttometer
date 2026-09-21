## Stuttometer v0.5.1 - Dynamic Display Refresh Calibration, Lock-Free Seqlock Concurrency & OSD Toast Redesign

### What's New

#### 1. Dynamic Display Refresh & VBlank Cadence Calibration
* **Hardware Refresh Detection:** Automatically queries the active display's physical refresh rate (Hz) and vblank interval (ms) via Win32 display APIs (`EnumDisplaySettingsW`, `MonitorFromWindow`) for the target game process window or primary desktop monitor.
* **Cadence-Aware Present Thresholds:** Default Present stutter thresholds adapt automatically to physical vblank intervals (e.g. 4.17 ms at 240 Hz, 6.94 ms at 144 Hz, 16.67 ms at 60 Hz), with manual overrides explicitly tracked (`--present-threshold-ms`, `--smi-threshold-ms`).
* **SMI Stall Auto-Scaling:** Auto-scales unprofiled hardware and SMI stall severity thresholds to 2x display cadence when using dynamic profiles.
* **Cadence-Aware Frame Timeline Tagging:** Tagging of pacing stalls in frame timelines now references the active baseline/refresh cadence instead of fixed 16.67 ms limits.

#### 2. DWM Glitch Timing & Multi-Refresh Pipeline
* **VBlank-Calibrated Glitch Evaluation:** Evaluates DWM compositor glitches against physical vblank cadence rather than fixed thresholds.
* **Duration Synthesis:** Synthesizes accurate DWM glitch durations directly from missed vblank counts and physical vblank intervals.
* **Hardware VBlank Telemetry:** Passes `hardware_vblank_ms` through `CorrelateOptions`, `DiagnosticReport`, and JSON report exports (v1.1 schema).

#### 3. Real-Time Lock-Free Hardening & Seqlock Synchronization
* **Seqlock-Guarded Trigger State:** Eliminated mutex contention in `TriggerEngine::active_trigger_` by implementing a lock-free 64-bit seqlock (`active_trigger_seq_`) and generation counter (`claim_generation_`).
* **Stale-Writer Protection:** Guaranteed memory order semantics prevent delayed or preempted threads from overwriting newer trigger data or stealing active `CLAIMED` states.
* **Torn Read Prevention in Flight Recorder:** Reader-side memory barriers and sequence checks in `FlightRecorder::snapshot` eliminate torn reads and cleanly distinguish reader-side slot skips from producer drop counters.
* **ProcessStart Zero-Alloc Optimization:** Bypasses image path decoding when the target process is already attached, ensuring zero memory allocations during active monitoring.

#### 4. Centralized Theme Palette & Dynamic Metric Severity
* **Unified Attribution Palette:** Centralized palette across UI, Visual Stutter Card, and OSD Toast: Game Engine (`#daa142`), DWM Composition (`#9a64cd`), External Contention (`#c55656`), and Unknown (`#697382`).
* **Dynamic Metric Severity Classification:** Standardized `MetricSeverity` levels (`NORMAL`, `WARNING`, `DANGER`) with refresh-rate-aware duration fallbacks.
* **Benchmark & Card Visual Polish:** Added attribution-colored row indicator stripes to the Session Benchmark summary and updated Visual Stutter Card rendering.

#### 5. Redesigned In-Game OSD Toast & Visual Documentation
* **Structured 3-Row Layout:** Redesigned toast layout featuring Process & Dynamic Severity Callout (Row 1), Diagnostic Culprit & Summary (Row 2), and Attribution Pill & Confidence % (Row 3).
* **Documentation Assets:** Added `scripts/render_snapshots.ps1` and generated high-resolution assets (`assets/osd_toast.png`, `assets/osd_toast_ingame.png`, `assets/osd_toast_game_engine.png`, `assets/osd_toast_audio_glitch.png`, `assets/osd_toast_clean.png`).
* **Updated README:** Embedded in-game OSD toast preview and documented refresh rate auto-detection.

#### 6. Test Suite & Verification
* All 15 unit test suites pass (100%), including new test scenarios for display refresh queries, high-refresh DWM pipelines, stale-writer race condition guards, and manual CLI flag tracking.
