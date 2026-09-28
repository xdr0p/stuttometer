# Contributing to Stuttometer

Thank you for your interest in contributing to Stuttometer! This document outlines our development guidelines, architectural rules, and code quality standards.

---

## 1. Development & Build Setup

### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 (with "Desktop development with C++")
- CMake 3.20+
- Git

### Building the Project
```powershell
cmake -B build -S .
cmake --build build --config Release
```

### Running the Test Suite
All test suites must pass 100% without assertion modifications or regressions:
```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## 2. Constants and Configuration Architecture

To maintain code clarity, prevent header pollution, and avoid magic numbers across the codebase, follow these strict placement rules for all constants and configurations:

### 1. Canonical Engine & Diagnostic Constants (`include/stuttometer/constants.hpp`)
- **What goes here:** Public constants used across core engine modules, CLI parsers, tests, or public APIs.
- **Examples:** Ring buffer dimensions (`DEFAULT_BUFFER_SLOTS`, `BUFFER_SLOT_MASK`), time conversions (`QPC_TICKS_PER_SEC`, `US_PER_SEC`), display cadence defaults (`DEFAULT_60HZ_VBLANK_MS`), default diagnostic thresholds (`DEFAULT_DPC_THRESHOLD_US`, `DEFAULT_DISK_THRESHOLD_MS`), and pacing profile multiplier constants.
- **Rule:** Header must strictly depend only on `<cstdint>` and `<cstddef>`. **Zero Windows API dependencies or `<windows.h>` includes.**

### 2. Internal Subsystem Constants (`include/stuttometer/internal/*.hpp`)
When constants are shared across multiple translation units within a specific subsystem but are not part of the public engine contract:
- `include/stuttometer/internal/severity_scales.hpp`: Threshold scales, baseline bounds, and severity levels for diagnostic metrics.
- `include/stuttometer/internal/dxgkrnl_layout.hpp`: Event property offsets and struct layout contracts for kernel ETW parsing.
- `include/stuttometer/internal/gui_constants.hpp`: Shared GUI timings, animation refresh loops, log limits, and window heuristics.
- `include/stuttometer/internal/process_watcher.hpp`: Process watcher polling intervals and lifecycle management constants.
- **Rule:** These headers must remain pure C++ headers free of `<windows.h>` where possible to prevent macro collisions.

### 3. File-Local & Subsystem Tuning Constants
- Tuning parameters local to a single `.cpp` file (e.g., sound cue synthesis frequencies in `src/gui/sound_cues.cpp`, ETW privilege retry constants in `src/privilege_utils.cpp`) should reside in an anonymous namespace or internal tuning namespace (e.g. `namespace sound_tuning`, `namespace etw_privilege_tuning`) at the top of that file.

### 4. CLI Validation Table (`include/stuttometer/cli_parser.hpp`)
- Numerical range validation for CLI options is centrally defined in `CLI_RANGES[]`.
- All CLI options with bounded ranges must use `validate_option_range<T>()` and be registered in `CLI_RANGES[]`.

---

## 3. Strict Architectural Invariants

### Zero-Allocation Hot Paths
- **Tracing Hot Path:** The ETW callback handler and flight recorder event processing path must perform **zero runtime heap allocations**.
- **Process Watcher Hot Path:** The 50 ms process watcher polling loop (`ProcessWatcher`) must execute without allocating memory (no `std::string` allocations, no heap copies; use `std::string_view` referencing the pinned target process string).
- Diagnostic overhead must never induce page faults, working set pressure, or GC pauses that distort frame pacing.

### Core & Engine Header Independence
- Core headers (`constants.hpp`, `correlator.hpp`, `flight_recorder.hpp`, `severity_scales.hpp`, `gui_constants.hpp`, etc.) must **never** include `<windows.h>`.
- Any required Win32 APIs, types, or GDI handles must be restricted to `.cpp` implementation files or GUI-specific headers (`theme.hpp`, `osd_toast.hpp`).

### Compile-Time Invariant Encoding
- Use `static_assert` to validate structural contracts, ring buffer power-of-two constraints (`(DEFAULT_BUFFER_SLOTS & (DEFAULT_BUFFER_SLOTS - 1)) == 0`), and byte alignment.

---

## 4. Git & Commit Guidelines

We follow Conventional Commits formatting:
- `feat(...)`: New user-facing or architectural feature
- `refactor(...)`: Code refactoring without behavioral drift
- `clean(...)`: Code hygiene, comment cleanup, string standardization
- `test(...)`: Adding or updating test suites
- `docs(...)`: Documentation updates

Ensure that every commit builds cleanly and passes all 16 test suites prior to pushing.
