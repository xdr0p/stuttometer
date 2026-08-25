# Stuttometer

Real-time micro-stutter, frame pacing, and audio-glitch diagnosis for Windows games and media apps via Event Tracing for Windows (ETW).

The standard way to diagnose a game stutter is to record a full system trace using WPR (`xperf`) and dig through multi-gigabyte `.etl` files after the fact—which only works if you happened to be recording when the stutter occurred. Stuttometer does the opposite: it traces continuously into a rolling in-memory flight recorder (~23.4 MB RAM), and when it detects a frame spike, cadence judder, compositor glitch, or audio dropout, it freezes that capture window, correlates the surrounding events across kernel, GPU, disk, memory, and audio subsystems, and writes a structured JSON diagnosis.

Leave it running in the background; when a stutter happens, the evidence is already captured.

> **Note:** Stuttometer is a diagnostic and troubleshooting tool. It is not an overlay, frame limiter, benchmark, or permanent ETL trace recorder.

---

## How It Works

- **Zero-Allocation Flight Recorder:** Events stream into a pre-allocated lock-free ring buffer (262,144 slots × 64 bytes, one seqlock per slot). No heap allocations occur while tracing is active, preventing diagnostic overhead from inducing its own DPCs or memory pressure.
- **Active ETW Flushing:** A dedicated worker thread issues kernel flush commands (`ControlTraceW` with `EVENT_TRACE_CONTROL_FLUSH`) every 100 ms (configurable down to 25–50 ms) so events reach the flight recorder with minimal delivery latency.
- **Hybrid Trigger Engine:** Tracks a moving-median frametime baseline to catch relative spikes (`--spike-multiplier`) and detects cadence variance ("judder")—the micro-stutters that never cross a fixed millisecond threshold. Static and dynamic trigger modes are also supported.
- **Three-Tier Presentation Timing:**
  - `Microsoft-Windows-DxgKrnl` (`MMIOFlip` / `FlipEvent` / `PresentStop`) — hardware flip completion events (DirectX 12, Vulkan, DirectX 11).
  - `Microsoft-Windows-DXGI` — CPU-side Present submission latency to isolate API bottlenecks from GPU delivery stalls.
  - `Microsoft-Windows-Dwm-Core` — desktop compositor schedule glitches with kernel-side keyword filtering.
- **Audio Underrun Detection:** Subscribes to `Microsoft-Windows-Audio` (Event ID 11) to capture audio dropouts and crackling.

---

## Root-Cause Correlation

When a trigger fires, Stuttometer saves a window around the stutter (e.g. 250 ms pre-trigger + 30 ms post-trigger) and correlates the events across multiple subsystems:

| Subsystem Signal | What It Detects / Correlates |
| :--- | :--- |
| **ISR / DPC Spikes** | Long-running interrupt routines and offending drivers (`amdkmdag.sys`, `nvlddmkm.sys`, `ndis.sys`, `storport.sys`, etc.) |
| **D3D12 Pipeline State Creation** | Synchronous runtime shader compilation stalls (`D3D12CreatePipelineState`) |
| **GPU VRAM Paging & Demotion** | VidMm budget overcommit, memory thrashing, and resource demotions |
| **Synchronous Memory Stalls** | Slow `VirtualAlloc` commits, MDL physical allocations, and aggressive working set trims |
| **Synchronous Disk I/O** | File reads/writes blocking execution beyond latency thresholds |
| **Context Switch Preemption** | Thread descheduling and CPU starvation during active frame delivery |
| **Hardware / SMI Stalls** | Firmware execution pauses (System Management Interrupts) where CPU execution was suspended |

Every trigger report produces a structured JSON output with ranked probable causes, confidence scores (0.0 to 1.0), and a supporting evidence timeline.

---

## Memory Footprint

| Subsystem | Resident Footprint | Details |
| :--- | ---:| :--- |
| Ring Buffer (`FlightRecorder`) | 16.78 MB | 262,144 slots × 64 bytes (cache-line aligned seqlocks) |
| In-Flight Tracking Tables | 6.61 MB | 9 lock-free open-addressing hash tables |
| **Total Resident Footprint** | **~23.4 MB** | Pre-allocated at startup; zero runtime heap allocations |

Buffer capacity can be adjusted with `--buffer-slots` (65,536 to 1,048,576).

---

## Building

### Prerequisites
- Windows 10 or Windows 11 (x64)
- Visual Studio 2022 (MSVC C++20) with C++ Desktop workload
- CMake 3.25+

```powershell
# Configure CMake
cmake -S . -B build

# Build Release binaries
cmake --build build --config Release

# (Optional) Run unit test suite
ctest --test-dir build -C Release --output-on-failure
```

The build produces two executables:
- `build\Release\stuttometer_gui.exe` (Standalone Win32 GUI dashboard)
- `build\Release\stuttometer.exe` (Command-line diagnostic utility)

---

## Desktop GUI (`stuttometer_gui.exe`)

Stuttometer includes a standalone native Win32 GUI (~1.2 MB) built on Common Controls v6 with zero external runtime dependencies.

- **Stutter Inspector:** Live report feed with ranked root-cause diagnoses, confidence score meters, and evidence timelines.
- **Process Picker:** Discovers running graphical games and applications via `EnumWindows`.
- **Live Activity Feed:** Visualizes real-time frametimes, DPC/ISR spikes, disk I/O, and memory events.
- **Export & Privacy:** One-click JSON report export and clipboard copying, with a toggleable `--redact` mode to sanitize process names, file paths, and usernames.

```powershell
.\build\Release\stuttometer_gui.exe
```

---

## CLI Usage

Live capture requires an elevated terminal (`Run as Administrator`):

```powershell
# Basic capture with default hybrid triggering:
.\build\Release\stuttometer.exe --output stutto_report.json

# Target a specific game executable:
.\build\Release\stuttometer.exe --target-process Game.exe --output-dir reports

# Target a specific Process ID:
.\build\Release\stuttometer.exe --target-pid 12345 --output-dir reports
```

### CLI Options

```text
Capture Window:
  --window-ms FLOAT               Pre-trigger window duration in ms (50.0-1000.0, default: 250.0)
  --post-trigger-ms FLOAT         Post-trigger capture duration in ms (0.0-200.0, default: 30.0)
  --cooldown-ms FLOAT             Minimum cooldown between reports in ms (100.0-10000.0, default: 1000.0)
  --buffer-slots INT              Ring buffer capacity in slots (65536-1048576, default: 262144)

Trigger Configuration:
  --trigger-mode TEXT             Frame trigger mode: hybrid, dynamic, static (default: hybrid)
  --present-threshold-ms FLOAT    Static Present stutter threshold in ms (2.0-200.0, default: 16.67)
  --spike-multiplier FLOAT        Relative stutter spike multiplier (1.2-10.0, default: 2.0)
  --min-spike-delta-ms FLOAT      Minimum absolute spike delta in ms (1.0-50.0, default: 4.0)
  --judder-detection / --no-judder Enable/disable cadence judder detection (default: enabled)
  --judder-swing-ratio FLOAT      Judder cadence swing threshold ratio (0.1-0.9, default: 0.35)
  --audio-trigger / --no-audio    Enable/disable AudioGlitch Event ID 11 trigger (default: enabled)

Subsystem Anomaly Thresholds:
  --dpc-threshold-us INT          DPC anomaly threshold in microseconds (100-50000, default: 1000)
  --isr-threshold-us INT          ISR anomaly threshold in microseconds (50-50000, default: 500)
  --disk-threshold-ms INT         Disk latency anomaly threshold in ms (1-1000, default: 20)
  --cswitch-threshold-ms INT      Context switch preemption threshold in ms (1-500, default: 5)
  --smi-threshold-ms FLOAT        Hardware SMI stall threshold in ms (10.0-100.0, default: 33.3)
  --d3d12-pso-threshold-ms INT    D3D12 PSO compilation threshold in ms (1-500, default: 5)
  --vram-threshold-mb INT         GPU VRAM demotion anomaly threshold in MB (1-1024, default: 8)
  --mem-alloc-threshold-mb INT    VirtualAlloc commit stall threshold in MB (1-1024, default: 16)
  --mem-trim-threshold-mb INT     Working set out-swap trim threshold in MB (1-1024, default: 4)
  --mem-physical-latency-us INT   Physical memory allocation latency in microseconds (50-50000, default: 1000)

Targeting, Output & General:
  --target-pid INT                Target Process ID to monitor (default: 0 = monitor all)
  --target-process TEXT           Target process name substring (e.g. Game.exe)
  --output PATH                   Output file path for single JSON report
  --output-dir PATH               Directory to save individual trigger reports
  --max-reports INT               Maximum number of reports before exiting (default: 0 = continuous)
  --tier TEXT                     Provider tier: minimal, standard, full (default: standard)
  --redact                        Redact process names, file paths, and user identifiers
  --verbose                       Print detailed event stream metrics to console
  --version                       Print version information and exit
```

---

## Design Notes

- **Zero Allocation During Active Tracing:** A diagnostic utility that dynamically allocates memory mid-trace risks triggering its own page faults, DPCs, and thread preemptions, distorting the very latency it is trying to observe. All ring slots and in-flight hash tables are pre-allocated at startup.
- **Fixed-Size Dashboard Layout (1020×660 @ 96 DPI, DPI-Scaled):** The GUI dashboard is hand-tuned to present dense telemetry data without horizontal/vertical scrollbars or awkward control clipping. `ptMaxTrackSize` equals `ptMinTrackSize` to guarantee a clean layout.
- **UAC Manifest on the GUI:** Real-time kernel ETW sessions require `SeSystemProfilePrivilege`. Elevating once via application manifest on launch avoids mid-session failures and secondary restart prompts.

---

## Limitations

- **Platform & Privileges:** Windows 10/11 x64 only. Live tracing strictly requires administrator privileges.
- **Heuristic Confidence:** Root-cause rankings are probabilistic correlation heuristics designed as high-signal starting points for investigation.
- **Scope:** Stuttometer identifies root causes and isolates culpable subsystems; it does not alter driver behavior, inject into game processes, or modify system scheduler priorities.

---

## License

This project is licensed under the **GNU General Public License v3.0** (GPLv3). See the [LICENSE](LICENSE) file for the full text.

