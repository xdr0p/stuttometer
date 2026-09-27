## Stuttometer v0.6.0 — Architectural Modernization & Comprehensive Subsystem Refactoring

Stuttometer v0.6.0 represents a major architectural modernization across all 5 core subsystems: the Correlation & Diagnostic Attribution Engine, Frame Pacing & Trigger Engine, ETW Ingestion & Kernel Handlers, Session Benchmark & Ring Buffer, and Native Win32 GUI.

---

### Pack 1: Correlation & Diagnostic Attribution Engine

- **Modular Confidence Computation:** Extracted specialized confidence calculation helpers (`compute_vram_confidence`, `compute_d3d12_confidence`, `compute_cswitch_confidence`, `compute_smi_confidence`, `compute_dpc_isr_confidence`, `compute_memory_confidence`, and `compute_disk_confidence`) in `src/correlator.cpp`.
- **Arithmetic & Overflow Safety:** Hardened duration normalization, score scaling, and temporal proximity windowing against potential numeric overflow or underflow under extreme stall durations.
- **Enhanced Test Verification:** Expanded unit test suites in `tests/test_attribution.cpp` and `tests/test_correlator.cpp` to validate confidence bounds, overflow resistance, and multi-culprit ranking heuristics.

---

### Pack 2: Frame Pacing & Trigger Engine

- **Candidate Resolution & Clamping Deduplication:** Extracted cadence candidate helpers and deduplicated dynamic threshold clamping in `include/stuttometer/frame_pacing_tracker.hpp`.
- **Harmonized Pacing Parameters:** Unified pacing multiplier and delta resolution via `resolve_pacing_params` across frame pacing evaluations and hybrid trigger logic.
- **Defensive Duration Clamping:** Standardized trigger source resolution and enforced `DEFENSIVE_DURATION_CLAMP_US` bounding in `src/trigger_engine.cpp`.
- **Hot-Path Logging Cleanup:** Removed unconditional debug `fprintf` logging from `evaluate_frame_pacing_common`, eliminating I/O stalls during real-time frame pacing analysis.

---

### Pack 3: ETW Ingestion & Kernel Handlers

- **Zero Hot-Path Logging:** Removed all diagnostic `fprintf` calls from the ETW consumer dispatch loop in `src/etw_session.cpp`.
- **Named Kernel Execution Caps:** Centralized single-event duration caps (`KERNEL_DPC_ISR_CAP_US`, `KERNEL_CSWITCH_CAP_US`, `KERNEL_DISK_CAP_US`, etc.) and helper `clamped_qpc_delta_us`, replacing magic numbers across all MOF, memory, and user-provider handlers while preserving strict CSwitch saturation semantics.
- **Decoupled Emission Pipeline:** Standardized event routing through `emit_event` and `emit_ndjson_only`, completely decoupling `ndjson_writer` header dependencies from individual kernel event handlers.
- **Handler Decomposition:** Extracted `unpack_dpc_isr` and `valid_trim_target` routines in `src/etw_handlers_kernel_mof.cpp` and `src/etw_handlers_memory.cpp`.
- **Portability & Debug Invariants:** Added `_DEBUG` assertions for `UserDataLength` validation and documented ARM64 memory ordering requirements.

---

### Pack 4: Session Benchmark & Lock-Free Ring Buffer

- **Bit-Packed State Transitions:** Extracted `target_state` bit-packing and unpacking operations into dedicated `benchmark_detail` helpers in `src/session_benchmark.cpp`.
- **Zero-Allocation State-2 Fallback:** Eliminated dynamic heap allocations in State-2 fallback processing and locked the frame duration denominator invariant to prevent division-by-zero or NaN propagation.
- **Optimized Top-5 Culprit Aggregation:** Replaced $O(N \log N)$ sorting with $O(K \log N)$ `std::partial_sort` for ranking top diagnostic culprits during live benchmark summaries.
- **Test 21 Coverage:** Added dedicated unit testing in `tests/test_session_benchmark.cpp` verifying partial sort correctness, tie-breaking stability, and zero-allocation guarantees.

---

### Pack 5: Native Win32 GUI & Visual Presentation

- **Declarative Control Subsystem:** Introduced `src/gui/gui_helpers.hpp` and `src/gui/gui_helpers.cpp` providing type-safe declarative factory functions (`create_control`, `create_checkbox`, `create_dropdown`, `create_numeric_edit`, `create_owner_button`, and `read_clamped_edit`), dramatically reducing Win32 boilerplate.
- **Centralized String Utilities:** Added `src/gui/gui_string_utils.hpp` for standardized string conversions and numeric formatting across GUI dialogs.
- **Unified Button Color Palettes:** Centralized theme palette resolution via `resolve_button_palette` for consistent styling across standard buttons, dark controls, and custom-drawn elements.
- **Modernized Dialogs:** Refactored `settings_dialog.cpp` and `benchmark_view.cpp` to use declarative control specifications, RAII layout scoping, and strict bounds validation.

---

### Test Suite & Release Verification

- **16/16 Test Suites Green:** 100% test pass rate across `test_flight_recorder`, `test_correlator`, `test_json_schema`, `test_etw_constants`, `test_etw_session`, `test_kernel_present_tracking`, `test_frame_pacing`, `test_attribution`, `test_frame_timeline`, `test_csv_exporter`, `test_ndjson_writer`, `test_report_serialization_consistency`, `test_card_renderer`, `test_session_benchmark`, `test_cli_args`, and `test_gui_settings`.
- **Zero Runtime Allocations Maintained:** Strict zero-allocation invariants preserved across the flight recorder, kernel ingestion, and session benchmark ring buffers.
