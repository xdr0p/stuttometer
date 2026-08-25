# Stuttometer

**Stuttometer** is a lightweight, real-time Windows ETW (Event Tracing for Windows) performance diagnostic utility designed to catch and diagnose micro-stutters and audio glitches in games and media applications without recording massive multi-gigabyte `.etl` trace files.

---

## Key Features

- **In-Memory Flight Recorder**: Rolling ~250–500ms circular memory buffer using lock-free seqlocks (~23.5 MB RAM footprint: ~16.8 MB ring buffer + ~6.7 MB in-flight tracking tables).
- **Dual CPU & GPU Display Stutter Triggers**:
  - **GPU Hardware Frame Delivery**: Subscribes to `Microsoft-Windows-DxgKrnl` (`MMIOFlip` / `FlipEvent` / `PresentStop`) to capture ground-truth hardware display flip latency in modern DirectX 12, Vulkan, and DirectX 11 titles.
  - **CPU-Side Present Latency**: Monitors `Microsoft-Windows-DXGI` for CPU/API submission bottlenecks.
  - **DWM Schedule Glitches**: Subscribes to `Microsoft-Windows-Dwm-Core` with kernel-side keyword filtering to catch desktop compositor glitches without buffer flooding.
  - **Audio Underruns**: Automatically captures audio dropouts (`Microsoft-Windows-Audio` Event ID 11).
- **Active Flush Worker**: Background thread issuing real-time ETW flush commands (`ControlTraceW` with `EVENT_TRACE_CONTROL_FLUSH`) every 100ms (configurable down to 25–50ms) to ensure low-latency event delivery.
- **Automated Root-Cause Correlation**:
  - Driver ISR/DPC execution spikes (e.g. `amdkmdag.sys`, `nvlddmkm.sys`, `ndis.sys`, `storport.sys`).
  - GPU pipeline stalls (shader compilation, rasterization overload, VRAM pressure).
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
- Visual Studio 2022 or latest with C++ workload (MSVC C++20)

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

## Running the Desktop GUI (`stuttometer_gui.exe`)

Stuttometer includes a small, standalone native Windows GUI (`stuttometer_gui.exe`, ~290 KB) built directly on the Win32 API and Common Controls v6 with zero runtime dependencies.

### Features:
- **Instant Stutter Inspector**: View real-time stutter captures with ranked root-cause diagnoses, confidence score meters, and structured evidence timelines.
- **Process Picker**: Asynchronously discovers running graphical games and applications (`EnumWindows`).
- **Automatic Administrator Elevation**: Embedded UAC manifest requests required administrative privileges on startup with active elevation badge.
- **JSON Export & Clipboard**: Single-click export or copy with full PII redaction toggles.

```powershell
# Launch GUI
.\build\Release\stuttometer_gui.exe
```

---

## Running Live Capture (Administrator Required)

Kernel ETW trace sessions require elevated permissions (`SeSystemprofilePrivilege`):

```powershell
# In an elevated PowerShell / Terminal window:
.\build\Release\stuttometer.exe --window-ms 250 --present-threshold-ms 16.67 --output stutto_report.json
```

### CLI Options

```text
Options:
  --window-ms FLOAT             Pre-trigger window duration in ms (default: 250.0, range: 50.0-1000.0)
  --post-trigger-ms FLOAT       Post-trigger capture duration in ms (default: 30.0, range: 0.0-200.0)
  --present-threshold-ms FLOAT  DXGI Present stutter threshold in ms (default: 16.67 [60 FPS], range: 5.0-200.0)
  --audio-trigger / --no-audio  Enable/disable AudioGlitch Event ID 11 trigger (default: enabled)
  --cooldown-ms FLOAT           Minimum cooldown between reports in ms (default: 1000.0, range: 100.0-10000.0)
  --dpc-threshold-us INT        DPC anomaly threshold in microseconds (default: 1000, range: 100-50000)
  --isr-threshold-us INT        ISR anomaly threshold in microseconds (default: 500, range: 50-50000)
  --disk-threshold-ms INT       Disk latency anomaly threshold in ms (default: 20, range: 1-1000)
  --cswitch-threshold-ms INT    Context switch preemption threshold in ms (default: 5, range: 1-500)
  --smi-threshold-ms FLOAT      Hardware SMI stall threshold in ms (default: 33.3, range: 10.0-100.0)
  --buffer-slots INT            Ring buffer capacity in slots (default: 262144, range: 65536-1048576)
  --target-pid INT              Explicit target Process ID to monitor (default: 0 = monitor all)
  --target-process TEXT         Explicit target process name substring (e.g. Game.exe)
  --output PATH                 Output file path for JSON reports
  --output-dir PATH             Directory to save individual trigger reports
  --max-reports INT             Maximum number of reports before exiting (default: 0 = continuous)
  --tier TEXT                   Provider tier: minimal, standard, full [default: standard]
  --redact                      Redact process names, file paths, and user identifiers
  --verbose                     Print event stream metrics to console
  --mock-test                   Run internal synthetic trace simulation suite
  --version                     Print version information and exit
```

---

## Intentional Design Decisions (Architecture Notes)

- **Fixed Window Dimensions**: The GUI window is intentionally fixed-size (1020x660 @ 96 DPI, scaled automatically with system DPI). It deliberately disallows arbitrary window resizing (`ptMaxTrackSize` equals `ptMinTrackSize`) to maintain a clean, compact, pixel-perfect dashboard layout without visual clipping or alignment anomalies.
- **Mandatory Administrator Manifest**: `stuttometer_gui.exe` is configured with `/MANIFESTUAC:level='requireAdministrator'`. Because real-time kernel-level ETW tracing (DPC, ISR, Disk I/O, Context Switches) strictly requires `SeSystemprofilePrivilege`, requesting elevation directly at startup provides a seamless experience and avoids redundant in-app prompt clicks.
