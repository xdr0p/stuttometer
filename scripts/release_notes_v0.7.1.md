## Stuttometer v0.7.1 — Zero-Allocation Hot Path, Constant Canonicalization & Test Hardening

Stuttometer v0.7.1 is a focused invariant-enforcement and code-quality release. It eliminates all heap allocations from the 50 ms `ProcessWatcher` polling loop, canonicalizes previously scattered magic constants, and adds a targeted suite of new tests covering behavioral parity, adaptive cadence, and zero-allocation guarantees.

---

### Key Changes

#### 1. ProcessWatcher Zero-Allocation Hot Path (Invariant #3)
- **Strictly zero heap allocations** on the 50 ms polling loop — process names are pre-converted to UTF-16 once at `start()` via `prepare_target()`, eliminating repeated `std::filesystem::path`, `std::string`, `std::wstring`, and `std::vector` construction on every tick.
- **3-pass stack-only Toolhelp32 snapshot resolver** (`find_target_pid_snapshot()`):
  - Pass 1 — Exact match (`_wcsicmp`)
  - Pass 2 — Prefix match (`_wcsnicmp`; only when target has no `.exe` extension)
  - Pass 3 — Case-insensitive substring scan using pre-lowercased target (no per-tick `towlower` on target)
- Tie-breaking: shortest name wins, then lowest PID — identical to the original `resolve_process_name_to_pid` behavior.
- **Adaptive polling cadence preserved:** ~50 ms while waiting for process launch, ~2000 ms while attached.
- Fixed path extraction: `find_last_of("/\\:")` handles drive-relative paths (e.g. `E:game.exe`); exe-name buffer limit separated from filesystem path length limit.
- `set_target_for_test()` guarded against concurrent worker-thread data races via `is_running_` check.

#### 2. Constant Canonicalization
- Introduced `DEFAULT_SPIKE_MULTIPLIER` (2.0) and `DEFAULT_MIN_SPIKE_DELTA_MS` (4.0) in `namespace stuttometer` (`frame_pacing_tracker.hpp`).
- `CliConfig` struct member defaults updated to use canonical constants; local variable initialisers in `AUTO_ADAPTIVE` and `CUSTOM` profile resolution branches updated accordingly.
- `--present-threshold-ms` CLI help string now derived dynamically from `DEFAULT_60HZ_VBLANK_MS` via `snprintf %.2f` — eliminates hardcoded `"16.67"`.
- Raw `1048576` in `test_flight_recorder.cpp` capacity log assertion replaced with `stuttometer::MAX_BUFFER_SLOTS`.

#### 3. Test Constant Sweep — `test_frame_pacing.cpp`
- All 125 `evaluate_frame_pacing()` call sites swept; magic literals at argument positions 6, 7, and 9 replaced with canonical constants:
  - `DEFAULT_SPIKE_MULTIPLIER`, `DEFAULT_MIN_SPIKE_DELTA_MS`, `pacing_tuning::DEFAULT_JUDDER_SWING_RATIO`, `HIGH_REFRESH_SPIKE_MULTIPLIER`, `HIGH_REFRESH_MIN_DELTA_MS`
- Calculation assertions and frame duration arguments intentionally preserved as literals.

#### 4. New Tests — `test_cli_args.cpp`
- **Behavioral parity test:** 10 input variants (exact with/without `.exe`, backslash path, forward-slash path, prefix-only, substring-only, drive-relative, deep path, non-existent, empty) verified identical results vs `resolve_process_name_to_pid`.
- **Adaptive cadence test:** banded timing assertions confirm 50 ms waiting cadence and ~2000 ms attached cadence.
- **Zero-allocation assertion:** `g_disallow_allocations` (thread-local `operator new` guard) enforces 0 dynamic allocations across Pass 1 exact, Pass 2 prefix, Pass 3 substring, full-table non-existent, and empty-target scenarios.

#### 5. Documentation
- Added clarifying banner comment in `src/gui/osd_toast.cpp` cross-linking `MIN_WINDOW_DIMENSION_PX` (100 px OSD minimum heuristic) to the corresponding zero-size guard in `gui_controller.cpp`.

---

### Test Suite
- **16/16 test suites passing 100%** in Release mode.
