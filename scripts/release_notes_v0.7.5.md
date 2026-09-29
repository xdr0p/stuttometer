# Stuttometer v0.7.5 — Intelligent Judder Episodes, Severity Gating & UI Modernization

Stuttometer v0.7.5 is a major release introducing an end-to-end overhaul of frame pacing and judder detection, multi-tier severity classification and pre-claim gating, detection presets, benchmark schema 1.3, and a comprehensive modernization of the GUI input controls and layout.

---

## 🚀 Key Highlights & Architecture Changes

### 1. Judder Episode Tracker & End-Emission Architecture
- **Episode Aggregation**: Multi-frame alternating cadence judder (e.g., 3:2 pull-down, alternating 16.6ms / 33.3ms frames) is now collapsed into discrete **Judder Episodes** with start tracking and end-emission, eliminating redundant trigger floods.
- **Last-Alternation Anchor**: The correlator capture window anchors at `trigger_timestamp_qpc` (`last_alt_qpc`), guaranteeing that the pre-trigger diagnostic window captures the episode and its root cause rather than post-episode clean frames.
- **Cadence State Bridging**: `reset_cadence_state` preserves `last_delta_us` while an episode is active (`judder_episode_active == 1`), bridging across relative spikes and threshold checks.
- **Integer Representation**: Standardized `judder_max_swing_q100` to ratio × 100 integer percentage format (50% swing is `50`, 60% is `60`).

### 2. Severity Classification, Pre-Claim Gating & Filtered Event Ring
- **Decoupled Detection & Reporting**: Detection sensitivity is decoupled from reporting. The detector remains hyper-sensitive and accurate, but triggers are filtered by severity before they touch the state machine or trigger cooldown.
- **Multi-Tier Severity**: Triggers are classified into `NORMAL`, `WARNING`, and `DANGER` tiers based on frame pacing impact, dropped vblanks, and duration.
- **Zero-Allocation & Lock-Free Hot Path**: `classify_severity` runs directly on the ETW producer thread with zero heap allocations, zero blocking synchronization, and zero Win32 calls.
- **Lock-Free `FilteredEventRing`**: High-performance MPSC lock-free ring buffer tracks filtered/minor triggers without taking locks or dropping telemetry.
- **NDJSON `TRIGGER_FILTERED` Stream**: Emits filtered events in NDJSON logs for comprehensive post-mortem analysis and tooling integration.

### 3. Detection Presets & User Controls
- **Standardized Presets**: Added `DetectionPreset` profiles:
  - `Esports / Low Latency` (High sensitivity for ultra-low latency esports)
  - `Competitive (Balanced)` (Default balanced profile)
  - `Cinematic / Heavy GPU` (Tuned for 4K / ray-traced AAA games)
  - `VR / Strict Frame Rate` (Zero-tolerance for VR judder)
  - `Custom` (Unlocks full manual threshold configuration)
- **Default Severity Gate**: Default gate flipped to `WARNING` so minor micro-stutters and vblank floor drops do not spam notifications or inflate stutter counts while gaming.
- **CLI & GUI Controls**:
  - CLI flags: `--preset`, `--min-report-severity`, `--osd-min-severity`, `--judder-min-alternations`.
  - GUI: Preset dropdown in Settings, OSD gate, Severity filter dropdown on dashboard, and real-time ListView filtering.

### 4. Session Benchmark Schema 1.3
- **Diagnostic Metrics**:
  - `stutters_detected`: Counts `WARNING` and `DANGER` severity events only.
  - `minor_stutters`: Counts filtered triggers (`NORMAL` severity or vblank floor drops).
  - `total_triggers = stutters_detected + minor_stutters`: Backward-compatible continuity with pre-1.3 benchmark totals.

### 5. UI Modernization, Control Centering & Layout Polish
- **Native Single-Line Centered Edit Controls**: Migrated all numeric threshold and multiplier edit controls to `ES_CENTER | ES_AUTOHSCROLL`, eliminating Win32 multi-line caret tracking offsets on incremental typing (e.g. typing `'1'` then `'0'`).
- **Dynamic Padding (`WM_NCCALCSIZE`)**: `EditCenteredSubclassProc` dynamically computes vertical padding based on active font metrics (`(box_h - font_h) / 2`) and insets horizontally by 4px.
- **Border Protection (`WM_NCPAINT`)**: Implemented client-rect clipping in `WM_NCPAINT`, guaranteeing mouse clicks, drag-selections, and caret rendering never clip or overwrite the 1-pixel rounded border.
- **Disabled Background Consistency (`WM_CTLCOLORSTATIC`)**: Routed disabled `EDIT` controls to return `COLOR_INPUT_BG` (`#0F172A`), eliminating inner gray rectangle artifacts.
- **Inline High-FPS Target Notice**: Replaced overlapping text warning in Card 2 with clean inline unit label rendering (`10 – 500 FPS (High floor: high sensitivity)` in amber).
- **"Custom" Profile Renaming & Preset Sync**: Renamed `"Custom Calibration"` to `"Custom"` across presets, sensitivity profiles, and benchmark views with bidirectional preset unlocking.
- **Dashboard Layout Rebalance**: Relocated the Severity Filter dropdown to Row 1 (Configuration Card), leaving Row 2 dedicated to actions (`Start`/`Stop`/`Clear`) and live telemetry. Standardized column alignment for improved readability.

---

## 🧪 Verification & Test Suite
- **18/18 test suites passing 100%** under MSVC x64 in Release mode (`ctest --test-dir build -C Release --output-on-failure`).
- New test suites added:
  - `test_judder_episodes.cpp`
  - `test_report_filtering.cpp`
  - `test_session_benchmark.cpp`
  - `test_cli_args.cpp`
  - `test_gui_settings.cpp`
  - `test_ndjson_writer.cpp`
  - `test_kernel_present_tracking.cpp`
  - `test_correlator.cpp`
