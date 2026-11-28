#include "stuttometer/internal/display_strings.hpp"

namespace stuttometer::display {

std::string_view trigger_reason_display(std::string_view raw) noexcept {
    if (raw == "STATIC_THRESHOLD" || raw == "Static Threshold")           return "Static Threshold";
    if (raw == "RELATIVE_SPIKE" || raw == "Relative Spike")               return "Relative Spike";
    if (raw == "STATISTICAL_OUTLIER" || raw == "Statistical Outlier")     return "Statistical Outlier";
    if (raw == "CADENCE_JUDDER" || raw == "Cadence Judder")               return "Cadence Judder";
    if (raw == "AUDIO_BUFFER_UNDERRUN" || raw == "Audio Underrun")        return "Audio Underrun";
    if (raw == "DWM_COMPOSITOR_GLITCH" || raw == "DWM Compositor Glitch") return "DWM Compositor Glitch";
    if (raw == "NONE" || raw == "None" || raw.empty())                    return "None";
    return raw;
}

std::string_view trigger_source_display(std::string_view raw) noexcept {
    if (raw == "DXGI_PRESENT_STUTTER" || raw == "DXGI Present") return "DXGI Present";
    if (raw == "AUDIO_GLITCH" || raw == "Audio Glitch")         return "Audio Glitch";
    if (raw == "MANUAL" || raw == "Manual")                     return "Manual";
    if (raw == "KERNEL_FRAME_STALL" || raw == "Kernel Frame Stall") return "Kernel Frame Stall";
    if (raw == "DWM_GLITCH" || raw == "DWM Glitch")             return "DWM Glitch";
    if (raw == "FRAME_PACING_JUDDER" || raw == "Pacing Judder") return "Pacing Judder";
    if (raw == "NONE" || raw == "None" || raw.empty())          return "None";
    return raw;
}

std::string_view hypothesis_display(std::string_view raw) noexcept {
    if (raw == "dpc_isr_spike")                      return "DPC / ISR Execution Spike";
    if (raw == "disk_io_stall")                      return "Disk I/O Latency Stall";
    if (raw == "context_switch_interference")        return "Thread Preemption & Scheduling Contention";
    if (raw == "gpu_pipeline_stall")                 return "GPU Pipeline / Shader Execution Stall";
    if (raw == "dwm_compositor_stall")               return "Desktop Window Manager (DWM) Compositor Stall";
    if (raw == "page_fault_stall")                   return "Hard Page Fault Latency Stall";
    if (raw == "thermal_throttle")                   return "Hardware Thermal Throttling";
    if (raw == "antimalware_interference")           return "Antimalware Real-Time Scan Contention";
    if (raw == "d3d12_shader_pso_compilation_stall") return "D3D12 Shader / PSO Compilation Stall";
    if (raw == "vram_exhaustion_paging_stall")       return "VRAM Eviction & PCIe Paging Stall";
    if (raw == "virtual_memory_allocation_stall")    return "VirtualAlloc Allocation Latency Stall";
    if (raw == "low_memory_working_set_trim_stall")  return "Low-Memory Working Set Trim Stall";
    if (raw == "physical_memory_allocation_latency") return "Contiguous Physical Memory Allocation Stall";
    if (raw == "frame_pacing_judder")                return "Presentation Frame Pacing Judder";
    if (raw == "unprofiled_hardware_or_smi_stall")   return "Unprofiled Hardware or Firmware SMI Stall";
    if (raw == "insufficient_evidence")              return "Insufficient Diagnostic Evidence";
    if (raw == "none" || raw == "None" || raw.empty()) return "None";
    if (raw == "Other")                              return "Other";
    return raw;
}

} // namespace stuttometer::display
