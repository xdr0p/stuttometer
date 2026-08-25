# Stuttometer

**Stuttometer** is a high-performance, lightweight, real-time Windows ETW (Event Tracing for Windows) diagnostic utility designed to detect, capture, and diagnose micro-stutters, frame pacing anomalies, and audio glitches in games and real-time media applications without recording massive multi-gigabyte `.etl` trace files.

---

## ⚡ Key Features

- **Zero-Allocation In-Memory Flight Recorder**: Rolling ~250–500ms circular memory buffer using lock-free seqlocks (~23.5 MB total RAM footprint: 16.78 MB ring buffer + 6.61 MB pre-allocated in-flight tracking tables).
- **Intelligent Frame Pacing & Display Triggers**:
  - **Hybrid Frame Pacing Engine**: Moving-median baseline tracking with dynamic relative spike detection (`--spike-multiplier`) and micro-judder cadence variance detection (`--judder-detection`).
  - **GPU Hardware Frame Delivery**: Subscribes to `Microsoft-Windows-DxgKrnl` (`MMIOFlip` / `FlipEvent` / `PresentStop`) to capture ground-truth hardware display flip latency in DirectX 12, Vulkan, and DirectX 11.
  - **CPU-Side Present Latency**: Monitors `Microsoft-Windows-DXGI` for CPU/API submission bottlenecks.
  - **DWM Compositor Glitches**: Subscribes to `Microsoft-Windows-Dwm-Core` with kernel-side keyword filtering to catch desktop compositor glitches without buffer flooding.
  - **Audio Underruns**: Automatically captures audio dropouts (`Microsoft-Windows-Audio` Event ID 11).
- **Active Flush Worker**: Background thread issuing real-time ETW flush commands (`ControlTraceW` with `EVENT_TRACE_CONTROL_FLUSH`) every 100ms (configurable down to 25–50ms) to ensure low-latency event delivery.
- **Automated Multi-Subsystem Root-Cause Correlation**:
  - **Driver ISR & DPC Spikes**: Identifies culprit drivers (e.g. `amdkmdag.sys`, `nvlddmkm.sys`, `ndis.sys`, `storport.sys`).
  - **DirectX 12 Shader Compilation**: Tracks synchronous `D3D12CreatePipelineState` runtime compilation stalls.
  - **GPU VRAM Overcommit & Paging**: Detects VidMm resource demotions and VRAM budget thrashing.
  - **Synchronous Memory Stalls**: Identifies `VirtualAlloc` commit stalls and heavy working set trimming.
  - **Synchronous Disk I/O**: Correlates file read/write operations exceeding latency thresholds.
  - **Context Switch Preemption**: Quantifies CPU thread starvation and descheduling durations.
  - **Hardware / SMI Stalls**: Identifies firmware SMI gaps where CPU execution was suspended.
- **Ranked Probabilistic Diagnostics**: Emits structured JSON reports with confidence scores (0.0 to 1.0) and supporting evidence timelines.
- **Privacy Controls**: Built-in `--redact` flag to sanitize process names, file paths, and user identifiers.

---

## 💾 Memory Architecture & Overhead

| Subsystem | Allocation | Footprint | Details |
| :--- | :--- | :--- | :--- |
| **Ring Buffer (`FlightRecorder`)** | Pre-allocated at startup | **16.78 MB** | 262,144 slots × 64 bytes (cache-line aligned seqlocks) |
| **In-Flight Tracking Tables** | Pre-allocated at startup | **6.61 MB** | 9 open-addressing lock-free hash tables with zero heap allocations |
| **Total Resident RAM Overhead** | **~23.4 MB** | **~23.5 MB total** | Zero runtime memory allocations during active tracing |

---

## 🛠️ Building Stuttometer

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
# Run the complete automated test suite
ctest --test-dir build -C Release --output-on-failure
```

---

## 🖥️ Running the Desktop GUI (`stuttometer_gui.exe`)

Stuttometer includes a standalone native Windows GUI (`stuttometer_gui.exe`, ~1.2 MB) built directly on the Win32 API and Common Controls v6 with zero runtime dependencies.

### GUI Features:
- **Instant Stutter Inspector**: View real-time stutter captures with ranked root-cause diagnoses, confidence score meters, and structured evidence timelines.
- **Process Picker**: Asynchronously discovers running graphical games and applications (`EnumWindows`).
- **Live Metric Dashboard**: Visualizes frametimes, DPC/ISR counts, disk operations, and memory events in real time.
- **Automatic Administrator Elevation**: Embedded UAC manifest requests required administrative privileges on startup.
- **JSON Export & Clipboard**: Single-click export or copy with full PII redaction toggles.

```powershell
# Launch GUI
.\build\Release\stuttometer_gui.exe
```

---

## ⚙️ Running Live Capture (CLI)

Kernel ETW trace sessions require elevated permissions (`SeSystemprofilePrivilege`):

```powershell
# In an elevated PowerShell / Terminal window:
.\build\Release\stuttometer.exe --window-ms 250 --present-threshold-ms 16.67 --output stutto_report.json
```

### CLI Options

```text
Options:
  --window-ms FLOAT               Pre-trigger window duration in ms (50.0-1000.0, default: 250.0)
  --post-trigger-ms FLOAT         Post-trigger capture duration in ms (0.0-200.0, default: 30.0)
  --present-threshold-ms FLOAT    Static Present stutter threshold in ms (2.0-200.0, default: 16.67)
  --trigger-mode TEXT             Frame trigger mode: hybrid, dynamic, static (default: hybrid)
  --spike-multiplier FLOAT        Relative stutter spike multiplier (1.2-10.0, default: 2.0)
  --min-spike-delta-ms FLOAT      Minimum absolute spike delta in ms (1.0-50.0, default: 4.0)
  --judder-detection / --no-judder Enable/disable cadence judder detection (default: enabled)
  --judder-swing-ratio FLOAT      Judder cadence swing threshold ratio (0.1-0.9, default: 0.35)
  --audio-trigger / --no-audio    Enable/disable AudioGlitch Event ID 11 trigger (default: enabled)
  --cooldown-ms FLOAT             Minimum cooldown between reports in ms (100.0-10000.0, default: 1000.0)
  --dpc-threshold-us INT          DPC anomaly threshold in microseconds (100-50000, default: 1000)
  --isr-threshold-us INT          ISR anomaly threshold in microseconds (50-50000, default: 500)
  --disk-threshold-ms INT         Disk latency anomaly threshold in ms (1-1000, default: 20)
  --cswitch-threshold-ms INT      Context switch preemption threshold in ms (1-500, default: 5)
  --smi-threshold-ms FLOAT        Hardware SMI stall threshold in ms (10.0-100.0, default: 33.3)
  --d3d12-pso-threshold-ms INT    D3D12 PSO compilation threshold in ms (1-500, default: 5)
  --vram-threshold-mb INT         GPU VRAM demotion anomaly threshold in MB (1-1024, default: 8)
  --mem-alloc-threshold-mb INT    VirtualAlloc commit stall threshold in MB (1-1024, default: 16)
  --mem-trim-threshold-mb INT     Working set out-swap trim threshold in MB (1-1024, default: 4)
  --mem-physical-latency-us INT   Physical memory / MDL allocation latency threshold in us (50-50000, default: 1000)
  --buffer-slots INT              Ring buffer capacity in slots (65536-1048576, default: 262144)
  --target-pid INT                Target Process ID to monitor (default: 0 = monitor all)
  --target-process TEXT           Target process name substring (e.g. Game.exe)
  --output PATH                   Output file path for JSON reports
  --output-dir PATH               Directory to save individual trigger reports
  --max-reports INT               Maximum number of reports before exiting (default: 0 = continuous)
  --tier TEXT                     Provider tier: minimal, standard, full (default: standard)
  --redact                        Redact process names, file paths, and user identifiers
  --verbose                       Print detailed event stream metrics to console
  --mock-test                     Run internal synthetic trace simulation suite
  --version                       Print version information and exit
```

---

## 🏛️ Architecture & Design Decisions

- **Fixed Window Dimensions**: The GUI window is intentionally fixed-size (1020×660 @ 96 DPI, scaled automatically with system DPI). It deliberately disallows arbitrary window resizing (`ptMaxTrackSize` equals `ptMinTrackSize`) to maintain a clean, compact, pixel-perfect dashboard layout without visual clipping or alignment anomalies.
- **Mandatory Administrator Manifest**: `stuttometer_gui.exe` is configured with `/MANIFESTUAC:level='requireAdministrator'`. Because real-time kernel-level ETW tracing (DPC, ISR, Disk I/O, Context Switches) strictly requires `SeSystemprofilePrivilege`, requesting elevation directly at startup provides a seamless experience and avoids redundant in-app prompt clicks.
