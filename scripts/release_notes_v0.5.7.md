## Stuttometer v0.5.7 — Cadence Adaptation, VRAM Attribution Tightening & Scene Transition Resilience

### 1. Frame Pacing Cadence Adaptation & Dynamic Promotion

#### Resilient Rolling Baseline Adaptation
- **60-Frame Candidate Accumulation:** Extended `RollingFrameStats` with dedicated candidate tracking fields (`candidate_count`, `candidate_first_qpc`, `candidate_sum_us`, `candidate_sum_sq_us`) to detect sustained framerate shifts (e.g., Dynamic Resolution Scaling shifts, high-load area transitions, or in-engine FPS caps changing from 173 FPS to 100 FPS).
- **Variance-Gated Promotion:** When 60 consecutive frames exhibit tight cadence consistency ($\sigma < 1.0\text{ms}$) and satisfy static gating, the rolling baseline is promoted directly to the new cadence without triggering persistent false-positive stutter cascades.
- **Strict Struct Alignment Invariant:** Maintained exact 320-byte alignment (5 cache lines) for `RollingFrameStats` via explicit byte padding, preserving lock-free cache-line separation and zero runtime heap allocations.
- **DRS Flapping & Outlier Resistance:** Inconsistent frames immediately reset candidate accumulation, guarding against baseline corruption during erratic pacing or transient spikes.
- **Hybrid Threshold Realignment:** In `HYBRID` mode, once the baseline is established ($\ge 8$ samples), dynamically lifts the static ceiling above the observed mean if vblank-derived thresholds sit below actual game frame times.

### 2. VRAM Demotion Attribution Tightening

#### Flag Filtering & Fallback Unblocking
- **Explicit Demotion Flag Requirement:** In `src/correlator.cpp`, filtered `DXGKRNL_VRAM_PAGING` candidates to require `rec.flags & EventFlags::VRAM_DEMOTED_COMMITMENT`. Informational VRAM budget notifications (`VRAM_USAGE_OVER_BUDGET`) without actual memory eviction no longer trigger false `vram_exhaustion_paging_stall` diagnoses.
- **Refined Confidence Scoring:**
  - Base confidence calibrated to 0.15 (down from 0.45), requiring meaningful target attribution or severe demotion to meet the $\ge 0.30$ diagnostic threshold.
  - Duration severity normalized against 64.0 MB (previously 50.0 MB).
  - Target process weighting penalized to 0.0 for non-target processes (previously 0.2).
  - Temporal proximity window tightened from 200 ms to 100 ms.
- **Diagnostic Fallback Unblocked:** When VRAM paging events lack actual demotion flags, fallback hypotheses such as `gpu_pipeline_stall` are unblocked rather than suppressed.

### 3. Diagnostic Card Visual Polish

#### Optical Centering & Dynamic Pill Width Floor
- **Optical Text Centering:** Adjusted the vertical offset of the confidence pill text from `+1.5f * s` to `+0.5f * s` in `src/gui/card_renderer.cpp`. Because GDI+ centers the font em-box (including font descent space), all-caps labels without descenders were pushed bottom-heavy by 1–2px. The refined `+0.5f * s` offset achieves exact 9px/9px balanced ink padding.
- **Dynamic Width Floor:** Reduced the confidence badge minimum width floor from 130px to 104px (`std::max(104.0f * s, text_bounds.Width + 20.0f * s)`), eliminating excessive horizontal padding around standard `"92% CONFIDENCE"` labels while preserving balanced 10px lateral margins.
- **`UNCONFIRMED` Diagnostic State:** When a report contains no diagnoses or top confidence is $\le 0.0$, the card now renders `UNCONFIRMED` in muted slate (`#94a3b8`, `br_muted`) instead of `0% CONFIDENCE`. This preserves culprit text wrapping geometry and baseline alignment while clearly communicating diagnostic uncertainty.

### 4. Unified 2.0s Scene Transition & Loading Ceiling

#### Inter-Frame Presentation Ceiling Harmonization
- **Centralized Constants:** Defined `PAUSE_CEILING_MS = 2000.0` and derived `PAUSE_CEILING_US = 2000000ULL` in `include/stuttometer/constants.hpp`.
- **Subsystem Harmonization:** Unified inter-frame pause ceilings across:
  - DXGI present duration tracking (`src/etw_handlers_dxgi.cpp:39`, `include/stuttometer/etw_session.hpp:203`).
  - Kernel MMIO flip delivery tracking (`src/etw_handlers_dxgkrnl.cpp:42-45`).
  - Frame pacing tracker baseline reset logic (`include/stuttometer/frame_pacing_tracker.hpp:294`).
  - Session benchmark frame ingestion and dropped pause frame counting (`src/session_benchmark.cpp`).
- **Loading Screen & Alt-Tab Resilience:** Inter-frame pauses $\ge 2.0\text{s}$ (loading screens, cutscenes, Alt-Tab window switches, and menu-to-gameplay transitions) now cleanly re-seed the rolling baseline and candidate accumulation buffers without firing false stutter triggers or skewing benchmark percentiles.
- **Kernel Flip Handler Cleanup:** Harmonized the legacy 30s flip ceiling to `PAUSE_CEILING_US` and removed the dead `std::min` clamp (`rec.duration_us = static_cast<uint32_t>(delta_us);`).

#### Intentional Divergence: Single-Event Kernel Caps Unchanged
Single-event execution duration caps in kernel event handlers remain unchanged:
- **Disk I/O:** Capped at 3.0s (`src/etw_handlers_kernel_mof.cpp`).
- **DPCs, ISRs, Context Switches, Page Faults:** Capped at 10.0s (`src/etw_handlers_kernel_mof.cpp`).
- **Working Set Trims & Physical Allocations:** Capped at 10.0s (`src/etw_handlers_memory.cpp`).
- **Antimalware Scans:** Capped at 10.0s (`src/etw_handlers_user_providers.cpp`).

*Architectural Rationale:* These caps measure individual kernel/driver execution durations (e.g., a hardware DPC routine or disk read taking 1–3s is a genuine device stall). In contrast, the 2.0s ceiling strictly bounds inter-frame presentation pacing and scene transitions.

### 5. Test Suite & Verification

- **Full Suite Green:** All 16 unit test suites pass (`ctest --output-on-failure`).
- **Attribution Suite:** Added `test_vram_attribution_flag_filtering` verifying flag filtering, fallback unblocking, and attribution confidence thresholds.
- **Card Renderer Suite:** Expanded Test 10 subsection 6 to assert the 104px floor, valid culprit layout under `UNCONFIRMED`, and automated pixel scanning verifying `#94a3b8` muted slate ink count $\ge 5$.
- **Frame Pacing Suite (28 Tests):** Added 9 unit tests covering candidate adaptation (173 FPS $\to$ 100 FPS), DRS flapping resistance, struct alignment, static gate re-entry, multi-stream isolation, and 2.0s scene transition boundaries.
- **Session Benchmark Suite (19 Tests):** Added `test_pause_ceiling_2s_boundary` verifying 2500ms frames are dropped into `dropped_pause_frames` while 1900ms frames are accepted into the ring buffer.
