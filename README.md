# Stuttometer

**Stuttometer** is a lightweight, real-time Windows ETW (Event Tracing for Windows) performance diagnostic utility designed to catch and diagnose micro-stutters and audio glitches in games and media applications without recording massive multi-gigabyte `.etl` trace files.

---

## Key Features

- **In-Memory Flight Recorder**: Rolling ~250ms circular memory buffer using lock-free seqlocks (< 17 MB RAM footprint).
- **Instant Stutter Triggers**: Automatically detects DXGI Present latency spikes (`Microsoft-Windows-DXGI`) and Audio glitches (`Microsoft-Windows-Audio` Event ID 11).
- **Active Flush Worker**: Background thread calling `FlushTraceW` every 25–50ms to ensure low-latency event delivery.
- **Automated Root-Cause Correlation**:
  - Driver ISR/DPC execution spikes (e.g. `nvlddmkm.sys`, `ndis.sys`, `storport.sys`).
  - Synchronous Disk I/O stalls (`EVENT_TRACE_FLAG_DISK_IO` & `DISK_IO_INIT`).
  - Context switch preemption / CPU starvation.
  - Hardware/SMI stall gap detection.
- **Ranked Probabilistic Diagnostics**: Emits structured JSON reports with confidence scores (0.0 to 1.0) and supporting evidence timelines.
- **Privacy Controls**: Built-in `--redact` flag to sanitize process names, file paths, and user identifiers.

---

## Building Stuttometer

### Prerequisites
- Windows 10 / 11 (x64)
- CMake 3.25+ (bundled with Visual Studio Build Tools or standalone)
- Visual Studio 2022 / 2026 with C++ workload (MSVC C++20)

### Build Commands

```powershell
# Configure CMake
cmake -S . -B build

# Build in Release mode
cmake --build build --config Release
```

### Running Tests (Unprivileged / Standard User)

```powershell
# Run the complete automated synthetic test suite
ctest --test-dir build -C Release --output-on-failure
```

---

## Running Live Capture (Administrator Required)

Kernel ETW trace sessions require elevated permissions (`SeSystemprofilePrivilege`):

```powershell
# In an elevated PowerShell / Terminal window:
.\build\Release\stuttometer.exe --window-ms 250 --present-threshold-ms 25.0 --output stutto_report.json
```

### CLI Options

```text
Options:
  --window-ms INT               Pre-trigger window duration in ms (default: 250, range: 50-1000)
  --post-trigger-ms INT         Post-trigger capture duration in ms (default: 30, range: 0-100)
  --present-threshold-ms FLOAT  DXGI Present stutter threshold in ms (default: 25.0)
  --audio-trigger / --no-audio  Enable/disable AudioGlitch Event ID 11 trigger (default: enabled)
  --cooldown-ms INT             Minimum cooldown between reports in ms (default: 1000)
  --dpc-threshold-us INT        DPC anomaly threshold in microseconds (default: 1000)
  --disk-threshold-ms INT       Disk latency anomaly threshold in ms (default: 20)
  --target-pid INT              Explicit target Process ID to monitor (default: auto-detect)
  --target-process TEXT         Explicit target process name (e.g. Game.exe)
  --output PATH                 Output file path for JSON reports
  --output-dir PATH             Directory to save individual trigger reports
  --max-reports INT             Maximum number of reports before exiting (default: 0 = continuous)
  --tier TEXT                   Provider tier: minimal, standard, full [default: full]
  --redact                      Redact process names, file paths, and user identifiers
  --verbose                     Print event stream metrics to console
  --mock-test                   Run internal synthetic trace simulation suite
  --version                     Print version information and exit
```
