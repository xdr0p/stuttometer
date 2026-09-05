#include "stuttometer/correlator.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <ctime>
#include <array>

namespace stuttometer {

static std::string format_hex_address(uint64_t addr) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << addr;
    return ss.str();
}

static std::string get_current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_s(&tm_buf, &tt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return ss.str();
}

CorrelationEngine::CorrelationEngine(
    const DriverSymbolResolver& driver_resolver,
    const CorrelatorThresholds& thresholds
)
    : driver_resolver_(driver_resolver)
    , thresholds_(thresholds)
{
}

DiagnosticReport CorrelationEngine::correlate(
    const std::vector<EtwEventRecord>& snapshot,
    const TriggerInfo& trigger,
    uint64_t qpc_freq,
    const ProviderContext& provider_ctx,
    uint64_t dropped_events,
    uint64_t unpaired_evictions,
    uint64_t insertion_failures,
    uint64_t producer_dropped_events
) const {
    DiagnosticReport report;
    report.timestamp_utc = get_current_utc_timestamp();
    report.trigger = trigger;
    report.total_events = snapshot.size();
    report.dropped_events = dropped_events;
    report.producer_dropped_events = producer_dropped_events;
    report.unpaired_evictions = unpaired_evictions;
    report.insertion_failures = insertion_failures;
    report.etw_events_lost = provider_ctx.etw_events_lost;
    report.etw_buffers_lost = provider_ctx.etw_buffers_lost;
    report.qpc_frequency = qpc_freq;
    report.provider_context = provider_ctx;
    report.thresholds = thresholds_;

    // 1. Accumulate category counts & search for anomaly candidates (local thread-safe candidate buffers)
    std::vector<DpcCandidate> dpc_candidates; dpc_candidates.reserve(128);
    std::vector<DiskCandidate> disk_candidates; disk_candidates.reserve(128);
    std::vector<CSwitchCandidate> cswitch_candidates; cswitch_candidates.reserve(256);
    std::vector<DwmCandidate> dwm_candidates; dwm_candidates.reserve(64);
    std::vector<PageFaultCandidate> pagefault_candidates; pagefault_candidates.reserve(64);
    std::vector<AntimalwareCandidate> antimalware_candidates; antimalware_candidates.reserve(64);
    std::vector<D3D12PsoCandidate> d3d12_candidates; d3d12_candidates.reserve(64);
    std::vector<VramPagingCandidate> vram_candidates; vram_candidates.reserve(64);
    std::vector<MemVirtualAllocCandidate> mem_alloc_candidates; mem_alloc_candidates.reserve(64);
    std::vector<MemTrimCandidate> mem_trim_candidates; mem_trim_candidates.reserve(64);
    std::vector<MemPhysicalAllocCandidate> mem_physical_candidates; mem_physical_candidates.reserve(64);

    bool thermal_throttle_detected = false;
    uint32_t throttle_core = 0;
    uint32_t throttle_cap_seconds = 0;

    std::array<uint64_t, 256> core_cswitch_preempt_us{};
    uint64_t target_thread_preempt_us = 0;

    for (const auto& rec : snapshot) {
        const double offset_ms = (rec.qpc_timestamp >= trigger.trigger_timestamp_qpc)
            ? qpc_delta_to_ms(rec.qpc_timestamp - trigger.trigger_timestamp_qpc, qpc_freq)
            : -qpc_delta_to_ms(trigger.trigger_timestamp_qpc - rec.qpc_timestamp, qpc_freq);

        switch (static_cast<EventCategory>(rec.category)) {
            case EventCategory::DXGI:
                ++report.event_counts.dxgi;
                break;
            case EventCategory::AUDIO:
                ++report.event_counts.audio;
                break;
            case EventCategory::DXGKRNL_MMIOFLIP:
                ++report.event_counts.dxgkrnl_mmioflip;
                break;
            case EventCategory::DXGKRNL_VSYNCDPC:
                ++report.event_counts.dxgkrnl_vsyncdpc;
                break;
            case EventCategory::DWM_GLITCH:
                ++report.event_counts.dwm_glitch;
                dwm_candidates.push_back({ rec, offset_ms });
                break;
            case EventCategory::DPC:
                ++report.event_counts.dpc;
                if (rec.duration_us >= thresholds_.dpc_threshold_us) {
                    DpcCandidate cand;
                    cand.record = rec;
                    cand.offset_ms = offset_ms;
                    cand.driver_name = driver_resolver_.resolve_driver_name(rec.payload.routine_addr);
                    dpc_candidates.push_back(std::move(cand));
                }
                break;
            case EventCategory::ISR:
                ++report.event_counts.isr;
                if (rec.duration_us >= thresholds_.isr_threshold_us) {
                    DpcCandidate cand;
                    cand.record = rec;
                    cand.offset_ms = offset_ms;
                    cand.driver_name = driver_resolver_.resolve_driver_name(rec.payload.routine_addr);
                    dpc_candidates.push_back(std::move(cand));
                }
                break;
            case EventCategory::DISK:
                ++report.event_counts.disk;
                if (rec.duration_us >= (thresholds_.disk_threshold_ms * 1000)) {
                    DiskCandidate cand;
                    cand.record = rec;
                    cand.offset_ms = offset_ms;
                    disk_candidates.push_back(std::move(cand));
                }
                break;
            case EventCategory::CSWITCH:
                ++report.event_counts.cswitch;
                if (!(rec.flags & EventFlags::CSWITCH_VOLUNTARY)) {
                    core_cswitch_preempt_us[rec.cpu_index] += rec.duration_us;
                }
                if (trigger.target_tid != 0) {
                    if (rec.tid == trigger.target_tid && !(rec.flags & EventFlags::CSWITCH_VOLUNTARY)) {
                        target_thread_preempt_us += rec.duration_us;
                        if (rec.duration_us >= (thresholds_.cswitch_preempt_ms * 1000)) {
                            cswitch_candidates.push_back({ rec, offset_ms, true });
                        }
                    } else if (rec.payload.cswitch.prev_tid == trigger.target_tid) {
                        // Switch-out event marks preemption initiation ONLY if outgoing departure was involuntary
                        if (!(rec.flags & EventFlags::CSWITCH_OUT_VOLUNTARY)) {
                            EtwEventRecord switch_out_rec = rec;
                            switch_out_rec.duration_us = 0;
                            cswitch_candidates.push_back({ switch_out_rec, offset_ms, false });
                        }
                    }
                } else {
                    // Auto-detect mode: collect severe involuntary preemptions across any thread
                    if (rec.duration_us >= (thresholds_.cswitch_preempt_ms * 1000) && !(rec.flags & EventFlags::CSWITCH_VOLUNTARY)) {
                        cswitch_candidates.push_back({ rec, offset_ms, true });
                    }
                }
                break;
            case EventCategory::PROFILE:
                ++report.event_counts.profile;
                break;
            case EventCategory::PAGE_FAULT:
                ++report.event_counts.page_fault;
                if (rec.duration_us >= (thresholds_.pagefault_threshold_ms * 1000)) {
                    pagefault_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::THERMAL_THROTTLE:
                ++report.event_counts.thermal_throttle;
                thermal_throttle_detected = true;
                throttle_core = rec.cpu_index;
                throttle_cap_seconds = static_cast<uint32_t>(rec.auxiliary_data);
                break;
            case EventCategory::ANTIMALWARE_SCAN:
                ++report.event_counts.antimalware_scan;
                if (rec.duration_us >= (thresholds_.antimalware_threshold_ms * 1000)) {
                    antimalware_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::D3D12_PSO_CREATE:
                ++report.event_counts.d3d12_pso_create;
                if (rec.duration_us >= (thresholds_.d3d12_pso_threshold_ms * 1000)) {
                    d3d12_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::DXGKRNL_VRAM_PAGING:
                ++report.event_counts.dxgkrnl_vram_paging;
                if (rec.auxiliary_data >= (thresholds_.vram_demoted_threshold_mb * 1024ULL * 1024ULL)) {
                    vram_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::MEM_VIRTUAL_ALLOC:
                ++report.event_counts.mem_virtual_alloc;
                if (rec.auxiliary_data >= (thresholds_.mem_alloc_threshold_mb * 1024ULL * 1024ULL)) {
                    mem_alloc_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::MEM_WORKING_SET_TRIM:
                ++report.event_counts.mem_working_set_trim;
                if (rec.auxiliary_data >= (thresholds_.mem_trim_threshold_mb * 1024ULL * 1024ULL)) {
                    mem_trim_candidates.push_back({ rec, offset_ms });
                }
                break;
            case EventCategory::MEM_PHYSICAL_ALLOC:
                ++report.event_counts.mem_physical_alloc;
                if (rec.duration_us >= thresholds_.mem_physical_latency_us) {
                    mem_physical_candidates.push_back({ rec, offset_ms });
                }
                break;
            default:
                break;
        }
    }

    std::vector<Diagnosis> hypotheses;
    constexpr size_t MAX_EVIDENCE_ITEMS = 10;

    // 2. Evaluate DPC / ISR Anomaly Hypothesis
    if (!dpc_candidates.empty()) {
        std::sort(dpc_candidates.begin(), dpc_candidates.end(), [](const DpcCandidate& a, const DpcCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        const auto& worst = dpc_candidates.front();
        const double duration_severity = std::min(1.0, static_cast<double>(worst.record.duration_us) / 3000.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 150.0));
        const double core_match = (worst.record.cpu_index == trigger.cpu_index) ? 1.0 : 0.5;

        // Weights sum to 0.98 max (0.40 base + 0.35 duration + 0.15 temporal + 0.08 core)
        const double confidence = 0.40 + (0.35 * duration_severity) + (0.15 * temporal_proximity) + (0.08 * core_match);

        Diagnosis diag;
        diag.hypothesis = "dpc_isr_spike";
        diag.confidence = confidence;
        diag.factors = { duration_severity, core_match, temporal_proximity };

        std::stringstream ss;
        ss << "Driver " << worst.driver_name << " executed a single " 
           << category_to_string(static_cast<EventCategory>(worst.record.category))
           << " routine for " << std::fixed << std::setprecision(2) << (worst.record.duration_us / 1000.0)
           << "ms on Core " << static_cast<int>(worst.record.cpu_index)
           << ", stalling thread execution during the performance window.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(dpc_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = dpc_candidates[i];
            EvidenceItem ev;
            ev.event_type = std::string(category_to_string(static_cast<EventCategory>(cand.record.category)));
            ev.driver_module = cand.driver_name;
            ev.routine_address = format_hex_address(cand.record.payload.routine_addr);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            diag.evidence.push_back(std::move(ev));
        }

        hypotheses.push_back(std::move(diag));
    }

    // 3. Evaluate Disk I/O Stall Hypothesis
    if (!disk_candidates.empty()) {
        std::sort(disk_candidates.begin(), disk_candidates.end(), [](const DiskCandidate& a, const DiskCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        const auto& worst = disk_candidates.front();
        const double lat_ms = worst.record.duration_us / 1000.0;
        const double duration_severity = std::min(1.0, lat_ms / 60.0);
        const double is_target_proc = (trigger.target_pid != 0 && worst.record.pid == trigger.target_pid) ? 1.0 
                                    : (trigger.target_pid == 0 ? 0.5 : 0.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.40 + (0.35 * duration_severity) + (0.15 * is_target_proc) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "disk_io_stall";
        diag.confidence = confidence;
        diag.factors = { duration_severity, is_target_proc, temporal_proximity };

        std::stringstream ss;
        ss << "Synchronous disk I/O operation stalled for " << std::fixed << std::setprecision(1)
           << lat_ms << "ms (" << ((worst.record.flags & EventFlags::DISK_IS_WRITE) ? "Write" : "Read")
           << " of " << worst.record.auxiliary_data << " bytes) during the trigger window.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(disk_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = disk_candidates[i];
            EvidenceItem ev;
            ev.event_type = "DISK";
            ev.driver_module = "storport.sys / disk.sys";
            ev.routine_address = format_hex_address(cand.record.payload.file_key);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = std::to_string(cand.record.auxiliary_data) + " bytes";
            diag.evidence.push_back(std::move(ev));
        }

        hypotheses.push_back(std::move(diag));
    }

    // 4. Evaluate Context Switch Preemption Hypothesis
    if (!cswitch_candidates.empty()) {
        std::sort(cswitch_candidates.begin(), cswitch_candidates.end(), [](const CSwitchCandidate& a, const CSwitchCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        const auto& worst = cswitch_candidates.front();
        const double preempt_ms = worst.record.duration_us / 1000.0;
        const double duration_severity = std::min(1.0, preempt_ms / 20.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 100.0));
        
        double confidence = 0.0;
        double core_match = 0.0;
        if (trigger.target_tid != 0) {
            core_match = (worst.record.cpu_index == trigger.cpu_index) ? 1.0 : 0.5;
            confidence = 0.40 + (0.30 * duration_severity) + (0.10 * temporal_proximity) + (0.05 * core_match);
        } else {
            core_match = (worst.record.cpu_index == trigger.cpu_index) ? 1.0 : 0.4;
            confidence = 0.35 + (0.30 * duration_severity) + (0.10 * temporal_proximity) + (0.05 * core_match);
        }

        Diagnosis diag;
        diag.hypothesis = "context_switch_interference";
        diag.confidence = confidence;
        diag.factors = { duration_severity, core_match, temporal_proximity };

        std::stringstream ss;
        if (trigger.target_tid != 0) {
            if (preempt_ms > 0.0) {
                ss << "Critical thread " << trigger.target_tid << " was involuntarily preempted for "
                   << std::fixed << std::setprecision(1) << preempt_ms << "ms (switched for TID "
                   << (worst.is_resumption ? worst.record.payload.cswitch.prev_tid : worst.record.tid) << ").";
            } else {
                ss << "Critical thread " << trigger.target_tid << " preemption was initiated near the performance anomaly "
                   << "(resumption occurred outside analysis window, switched for TID " << worst.record.tid << ").";
            }
        } else {
            if (preempt_ms > 0.0) {
                ss << "Involuntary thread preemption occurred for "
                   << std::fixed << std::setprecision(1) << preempt_ms << "ms on TID "
                   << worst.record.tid << " (switched from TID " << worst.record.payload.cswitch.prev_tid << ").";
            } else {
                ss << "Involuntary thread preemption initiated near trigger on TID "
                   << worst.record.tid << " (switched from TID " << worst.record.payload.cswitch.prev_tid << ").";
            }
        }
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(cswitch_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = cswitch_candidates[i];
            EvidenceItem ev;
            ev.event_type = "CSWITCH";
            ev.driver_module = "ntoskrnl.exe";
            ev.routine_address = "0x0000000000000000";
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.secondary_tid = cand.record.payload.cswitch.prev_tid;
            ev.secondary_pid = cand.record.payload.cswitch.prev_pid;
            if (cand.is_resumption) {
                std::stringstream ev_ss;
                ev_ss << "Resumed TID " << cand.record.tid << " after " << std::fixed << std::setprecision(1) 
                      << (cand.record.duration_us / 1000.0) << "ms (prev TID " << cand.record.payload.cswitch.prev_tid;
                if (cand.record.payload.cswitch.prev_pid != 0) {
                    ev_ss << ", PID " << cand.record.payload.cswitch.prev_pid;
                }
                ev_ss << ")";
                ev.extra_info = ev_ss.str();
            } else {
                std::stringstream ev_ss;
                ev_ss << "Preempted TID " << cand.record.payload.cswitch.prev_tid << " for TID " << cand.record.tid;
                if (cand.record.payload.cswitch.prev_pid != 0) {
                    ev_ss << " (prev PID " << cand.record.payload.cswitch.prev_pid << ")";
                }
                ev.extra_info = ev_ss.str();
            }
            diag.evidence.push_back(std::move(ev));
        }

        hypotheses.push_back(std::move(diag));
    }

    // 5. GPU-Side Pipeline Stall Hypothesis (when triggered via KERNEL_FRAME_STALL and no software DPC/Disk/VRAM/Memory/PSO/AV anomalies)
    if (trigger.source == TriggerSource::KERNEL_FRAME_STALL && 
        dpc_candidates.empty() && 
        disk_candidates.empty() && 
        cswitch_candidates.empty() &&
        vram_candidates.empty() && 
        mem_alloc_candidates.empty() && 
        mem_trim_candidates.empty() && 
        mem_physical_candidates.empty() && 
        d3d12_candidates.empty() && 
        antimalware_candidates.empty() && 
        pagefault_candidates.empty()) {
        Diagnosis diag;
        diag.hypothesis = "gpu_pipeline_stall";
        diag.confidence = std::clamp(0.60 + std::min(trigger.duration_ms / 100.0, 0.30), 0.60, 0.90);
        diag.factors = { std::min(trigger.duration_ms / 50.0, 1.0), 0.5, 1.0 };
        std::stringstream ss;
        ss << "Display frame delivery stall detected via kernel DxgKrnl Flip delta ("
           << std::fixed << std::setprecision(1) << trigger.duration_ms << "ms";
        if (trigger.baseline_fps > 0.0) {
            ss << ", " << std::setprecision(1) << trigger.spike_ratio << "x spike from "
               << trigger.baseline_fps << " FPS / " << trigger.baseline_avg_ms << "ms baseline";
        }
        ss << "). "
           << "No software driver DPC/ISR or disk I/O freezes were observed in the kernel window. "
           << "Suspected GPU-side pipeline stall (shader compilation, rasterization overload, or VRAM pressure).";
        diag.summary = ss.str();
        hypotheses.push_back(std::move(diag));
    }

    // 6. DWM Compositor Stall Hypothesis (when triggered via DWM_GLITCH or when DWM glitch events present)
    if (trigger.source == TriggerSource::DWM_GLITCH || !dwm_candidates.empty()) {
        std::sort(dwm_candidates.begin(), dwm_candidates.end(), [](const DwmCandidate& a, const DwmCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        double effective_dur_ms = trigger.duration_ms;
        if (trigger.source != TriggerSource::DWM_GLITCH && !dwm_candidates.empty()) {
            effective_dur_ms = dwm_candidates.front().record.duration_us / 1000.0;
        }

        const double duration_severity = std::min(1.0, effective_dur_ms / 50.0);
        const double confidence = std::clamp(0.50 + (0.25 * duration_severity), 0.50, 0.75);

        Diagnosis diag;
        diag.hypothesis = "dwm_compositor_stall";
        diag.confidence = confidence;
        diag.factors = { duration_severity, 0.0, 0.0 };

        std::stringstream ss;
        ss << "Desktop Window Manager (DWM) compositor frame drop detected ("
           << std::fixed << std::setprecision(1) << effective_dur_ms << "ms). ";
        if (trigger.source == TriggerSource::DWM_GLITCH && trigger.glitch_count > 0) {
            ss << "Compositor missed " << trigger.glitch_count << " vertical sync interval(s).";
        } else if (!dwm_candidates.empty()) {
            ss << "Compositor recorded " << dwm_candidates.size() << " composition glitch event(s) during the analysis window.";
        } else {
            ss << "Composition scheduling hitch occurred on the desktop window manager.";
        }
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(dwm_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = dwm_candidates[i];
            EvidenceItem ev;
            ev.event_type = "DWM_GLITCH";
            ev.driver_module = "dwm.exe / dwmcore.dll";
            ev.routine_address = "0x0000000000000000";
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = "GlitchType: " + std::to_string(cand.record.auxiliary_data);
            diag.evidence.push_back(std::move(ev));
        }

        hypotheses.push_back(std::move(diag));
    }

    // 7. Page Fault Stall Hypothesis
    if (!pagefault_candidates.empty()) {
        std::sort(pagefault_candidates.begin(), pagefault_candidates.end(),
            [](const PageFaultCandidate& a, const PageFaultCandidate& b) {
                return a.record.duration_us > b.record.duration_us;
            });

        const auto& worst = pagefault_candidates.front();
        const double lat_ms = worst.record.duration_us / 1000.0;
        const double duration_severity = std::min(1.0, lat_ms / 30.0);
        const double is_target_proc = (trigger.target_pid != 0 && worst.record.pid == trigger.target_pid) ? 1.0
                                    : (trigger.target_pid == 0 ? 0.5 : 0.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.90, 0.40 + (0.30 * duration_severity) + (0.20 * is_target_proc) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "page_fault_stall";
        diag.confidence = confidence;
        diag.factors = { duration_severity, is_target_proc, temporal_proximity };

        std::stringstream ss;
        ss << "Hard page fault stalled thread " << worst.record.tid << " for "
           << std::fixed << std::setprecision(1) << lat_ms << "ms reading "
           << worst.record.auxiliary_data << " bytes from disk. "
           << "Working set pressure or memory-mapped file I/O forced a synchronous disk read.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(pagefault_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = pagefault_candidates[i];
            EvidenceItem ev;
            ev.event_type = "PAGE_FAULT";
            ev.driver_module = "ntoskrnl.exe / mm";
            ev.routine_address = format_hex_address(cand.record.payload.file_key);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = std::to_string(cand.record.auxiliary_data) + " bytes (hard fault)";
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 8. Thermal Throttle Hypothesis
    if (thermal_throttle_detected) {
        const double cap_severity = std::min(1.0, static_cast<double>(throttle_cap_seconds) / 20.0);
        const double confidence = std::clamp(0.40 + (0.35 * cap_severity), 0.40, 0.80);
        Diagnosis diag;
        diag.hypothesis = "thermal_throttle";
        diag.confidence = confidence;
        diag.factors = { cap_severity, 0.0, 1.0 };

        std::stringstream ss;
        ss << "CPU core " << throttle_core << " was firmware-throttled for "
           << throttle_cap_seconds << " seconds during the capture window. "
           << "Thermal or power budget constraints reduced processor speed, causing increased frame times.";
        diag.summary = ss.str();
        hypotheses.push_back(std::move(diag));
    }

    // 9. Antimalware Interference Hypothesis
    if (!antimalware_candidates.empty()) {
        std::sort(antimalware_candidates.begin(), antimalware_candidates.end(),
            [](const AntimalwareCandidate& a, const AntimalwareCandidate& b) {
                return a.record.duration_us > b.record.duration_us;
            });

        const auto& worst = antimalware_candidates.front();
        const double lat_ms = worst.record.duration_us / 1000.0;
        const double duration_severity = std::min(1.0, lat_ms / 50.0);
        const double scan_count_factor = std::min(1.0, static_cast<double>(antimalware_candidates.size()) / 5.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.85, 0.35 + (0.25 * duration_severity) + (0.15 * scan_count_factor) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "antimalware_interference";
        diag.confidence = confidence;
        diag.factors = { duration_severity, scan_count_factor, temporal_proximity };

        std::stringstream ss;
        ss << "Windows Defender real-time protection performed "
           << antimalware_candidates.size() << " scan(s) during the trigger window (longest: "
           << std::fixed << std::setprecision(1) << lat_ms << "ms). "
           << "Minifilter interception may have blocked game file I/O. "
           << "Consider adding game directory to Defender exclusions.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(antimalware_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = antimalware_candidates[i];
            EvidenceItem ev;
            ev.event_type = "ANTIMALWARE_SCAN";
            ev.driver_module = "MsMpEng.exe / WdFilter.sys";
            ev.routine_address = "0x0000000000000000";
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = "Real-time scan (" + std::to_string(cand.record.duration_us / 1000) + "ms)";
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 10. Direct3D 12 Pipeline State Object (PSO) & Shader Compilation Stall Hypothesis
    if (!d3d12_candidates.empty()) {
        std::sort(d3d12_candidates.begin(), d3d12_candidates.end(),
            [](const D3D12PsoCandidate& a, const D3D12PsoCandidate& b) {
                return a.record.duration_us > b.record.duration_us;
            });

        const auto& worst = d3d12_candidates.front();
        const double lat_ms = worst.record.duration_us / 1000.0;
        const double duration_severity = std::min(1.0, lat_ms / 30.0);
        const double is_target_thread = (trigger.target_tid != 0 && trigger.target_tid == worst.record.tid) ? 1.0
                                      : (trigger.target_pid != 0 && trigger.target_pid == worst.record.pid ? 0.8 : (trigger.target_pid == 0 ? 0.5 : 0.1));
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.40 + (0.35 * duration_severity) + (0.15 * is_target_thread) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "d3d12_shader_pso_compilation_stall";
        diag.confidence = confidence;
        diag.factors = { duration_severity, is_target_thread, temporal_proximity };

        std::stringstream ss;
        ss << "Direct3D 12 Pipeline State Object (PSO) or shader compilation stall detected taking "
           << std::fixed << std::setprecision(1) << lat_ms << "ms on TID " << worst.record.tid
           << " (offset: " << std::showpos << worst.offset_ms << "ms).";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(d3d12_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = d3d12_candidates[i];
            EvidenceItem ev;
            ev.event_type = "D3D12_PSO";
            ev.driver_module = "d3d12.dll";
            ev.routine_address = format_hex_address(cand.record.auxiliary_data);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = ((cand.record.flags & EventFlags::D3D12_COMPUTE_PSO) ? "Compute/DXR State Object (" : "Graphics PSO (") +
                            std::to_string(cand.record.duration_us / 1000) + "ms)";
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 11. Evaluate GPU VRAM Exhaustion & PCIe Paging Stall Hypothesis
    if (provider_ctx.user_vram_paging_active && !vram_candidates.empty()) {
        std::sort(vram_candidates.begin(), vram_candidates.end(), [](const VramPagingCandidate& a, const VramPagingCandidate& b) {
            return a.record.auxiliary_data > b.record.auxiliary_data;
        });

        const auto& worst = vram_candidates.front();
        const double max_demoted_mb = worst.record.auxiliary_data / (1024.0 * 1024.0);
        const double duration_severity = std::min(1.0, max_demoted_mb / 50.0);
        const double is_target_proc = (trigger.target_pid != 0 && worst.record.pid == trigger.target_pid) ? 1.0 
                                    : (trigger.target_pid == 0 ? 0.6 : 0.2);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.45 + (0.30 * duration_severity) + (0.15 * is_target_proc) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "vram_exhaustion_paging_stall";
        diag.confidence = confidence;
        diag.factors = { duration_severity, is_target_proc, temporal_proximity };

        std::stringstream ss;
        ss << "GPU VRAM exhaustion and PCIe paging thrashing detected for PID " << worst.record.pid
           << " (demoted/evicted commitment: " << std::fixed << std::setprecision(1) << max_demoted_mb << " MB). "
           << "Local video memory budget exceeded, forcing driver to page allocations over PCIe bus during frame delivery.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(vram_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = vram_candidates[i];
            EvidenceItem ev;
            ev.event_type = "VRAM_PAGING";
            ev.driver_module = "dxgkrnl.sys / dxgmms2.sys";
            ev.routine_address = format_hex_address(cand.record.auxiliary_data);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;

            const double mb = cand.record.auxiliary_data / (1024.0 * 1024.0);
            std::stringstream ev_ss;
            if (cand.record.flags & EventFlags::VRAM_DEMOTED_COMMITMENT) {
                ev_ss << "Demoted to System RAM: " << std::fixed << std::setprecision(1) << mb << " MB";
            } else if (cand.record.flags & EventFlags::VRAM_USAGE_OVER_BUDGET) {
                ev_ss << "Non-Local Aperture Usage: " << std::fixed << std::setprecision(1) << mb << " MB";
            } else {
                ev_ss << "PCIe Paging Transfer: " << std::fixed << std::setprecision(1) << mb << " MB";
            }
            ev.extra_info = ev_ss.str();
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 12. Evaluate VirtualAlloc Allocation Stall Hypothesis
    if (provider_ctx.kernel_pagefault_active && !mem_alloc_candidates.empty()) {
        std::sort(mem_alloc_candidates.begin(), mem_alloc_candidates.end(), [](const MemVirtualAllocCandidate& a, const MemVirtualAllocCandidate& b) {
            return a.record.auxiliary_data > b.record.auxiliary_data;
        });

        const auto& worst = mem_alloc_candidates.front();
        const double alloc_mb = worst.record.auxiliary_data / (1024.0 * 1024.0);
        const double size_severity = std::min(1.0, alloc_mb / 64.0);
        const double is_target_proc = (trigger.target_pid != 0 && worst.record.pid == trigger.target_pid) ? 1.0 
                                    : (trigger.target_pid == 0 ? 0.6 : 0.2);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.45 + (0.30 * size_severity) + (0.15 * is_target_proc) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "virtual_memory_allocation_stall";
        diag.confidence = confidence;
        diag.factors = { size_severity, is_target_proc, temporal_proximity };

        std::stringstream ss;
        ss << "Synchronous virtual memory allocation stall detected for PID " << worst.record.pid
           << " committing " << std::fixed << std::setprecision(1) << alloc_mb << " MB of virtual address space "
           << "near frame delivery (offset: " << std::showpos << worst.offset_ms << "ms).";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(mem_alloc_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = mem_alloc_candidates[i];
            EvidenceItem ev;
            ev.event_type = "MEM_VIRTUAL_ALLOC";
            ev.driver_module = "ntoskrnl.exe";
            ev.routine_address = format_hex_address(cand.record.payload.routine_addr);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            const double mb = cand.record.auxiliary_data / (1024.0 * 1024.0);
            ev.extra_info = "Committed " + std::to_string(static_cast<uint64_t>(mb)) + " MB (Base " + format_hex_address(cand.record.payload.routine_addr) + ")";
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 13. Evaluate OS Working Set Low-Memory Trim Stall Hypothesis
    if (provider_ctx.kernel_memory_active && !mem_trim_candidates.empty()) {
        std::sort(mem_trim_candidates.begin(), mem_trim_candidates.end(), [](const MemTrimCandidate& a, const MemTrimCandidate& b) {
            return a.record.auxiliary_data > b.record.auxiliary_data;
        });

        const auto& worst = mem_trim_candidates.front();
        const double trim_mb = worst.record.auxiliary_data / (1024.0 * 1024.0);
        const double trim_severity = std::min(1.0, trim_mb / 32.0);
        const double duration_factor = (worst.record.duration_us > 0) ? std::min(1.0, worst.record.duration_us / 10000.0) : 0.5;
        const double is_target_proc = (trigger.target_pid != 0 && worst.record.pid == trigger.target_pid) ? 1.0 
                                    : (trigger.target_pid == 0 ? 0.6 : 0.2);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.40 + (0.25 * trim_severity) + (0.15 * duration_factor) + (0.10 * is_target_proc) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "low_memory_working_set_trim_stall";
        diag.confidence = confidence;
        diag.factors = { trim_severity, is_target_proc, temporal_proximity };

        std::stringstream ss;
        ss << "OS working set trim and memory out-swap detected under system memory pressure for PID " << worst.record.pid
           << " (trimmed " << std::fixed << std::setprecision(1) << trim_mb << " MB";
        if (worst.record.duration_us > 0) {
            ss << " taking " << (worst.record.duration_us / 1000.0) << "ms";
        }
        ss << "). Subsequent page touches will cause hard page fault stalls.";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(mem_trim_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = mem_trim_candidates[i];
            EvidenceItem ev;
            ev.event_type = "MEM_WORKING_SET_TRIM";
            ev.driver_module = "ntoskrnl.exe";
            ev.routine_address = "0x0000000000000000";
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            const double mb = cand.record.auxiliary_data / (1024.0 * 1024.0);
            std::stringstream ev_ss;
            ev_ss << "Trimmed " << std::fixed << std::setprecision(1) << mb << " MB";
            if (cand.record.duration_us > 0) {
                ev_ss << " (" << (cand.record.duration_us / 1000.0) << "ms)";
            }
            ev.extra_info = ev_ss.str();
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 14. Evaluate Contiguous Physical Memory & MDL Driver Allocation Latency Hypothesis
    if (provider_ctx.kernel_memory_active && !mem_physical_candidates.empty()) {
        std::sort(mem_physical_candidates.begin(), mem_physical_candidates.end(), [](const MemPhysicalAllocCandidate& a, const MemPhysicalAllocCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        const auto& worst = mem_physical_candidates.front();
        const double lat_ms = worst.record.duration_us / 1000.0;
        const double alloc_mb = worst.record.auxiliary_data / (1024.0 * 1024.0);
        const double latency_severity = std::min(1.0, worst.record.duration_us / 10000.0);
        const double size_factor = std::min(1.0, alloc_mb / 32.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 200.0));

        const double confidence = std::min(0.95, 0.40 + (0.35 * latency_severity) + (0.15 * size_factor) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "physical_memory_allocation_latency";
        diag.confidence = confidence;
        diag.factors = { latency_severity, size_factor, temporal_proximity };

        std::stringstream ss;
        ss << "Kernel contiguous physical memory or MDL allocation stalled execution for "
           << std::fixed << std::setprecision(2) << lat_ms << "ms (allocated "
           << std::setprecision(1) << alloc_mb << " MB).";
        diag.summary = ss.str();

        for (size_t i = 0; i < std::min(mem_physical_candidates.size(), MAX_EVIDENCE_ITEMS); ++i) {
            const auto& cand = mem_physical_candidates[i];
            EvidenceItem ev;
            ev.event_type = "MEM_PHYSICAL_ALLOC";
            ev.driver_module = "ntoskrnl.exe";
            ev.routine_address = "0x0000000000000000";
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            const double mb = cand.record.auxiliary_data / (1024.0 * 1024.0);
            ev.extra_info = "Physical alloc: " + std::to_string(cand.record.duration_us / 1000) + "ms (" + std::to_string(static_cast<uint64_t>(mb)) + " MB)";
            diag.evidence.push_back(std::move(ev));
        }
        hypotheses.push_back(std::move(diag));
    }

    // 15. Evaluate Presentation Frame Pacing Judder Hypothesis
    if (trigger.source == TriggerSource::FRAME_PACING_JUDDER || trigger.reason == TriggerReason::CADENCE_JUDDER) {
        Diagnosis diag;
        diag.hypothesis = "frame_pacing_judder";
        diag.confidence = 0.80;
        diag.factors = { 0.80, 0.5, 1.0 };
        std::stringstream ss;
        ss << "Severe presentation cadence judder detected (alternating frame delivery swing >= 35% around ";
        if (trigger.baseline_fps > 0.0) {
            ss << std::fixed << std::setprecision(1) << trigger.baseline_fps << " FPS / " << trigger.baseline_avg_ms << "ms baseline). ";
        } else {
            ss << "stream baseline). ";
        }
        ss << "Presentation queue or compositor timing is de-synced with the display refresh rate.";
        diag.summary = ss.str();
        hypotheses.push_back(std::move(diag));
    }

    // 16. Constrained SMI / Unprofiled Hardware Gap Check
    const bool is_severe = (trigger.source == TriggerSource::AUDIO_GLITCH) || 
                           (trigger.source == TriggerSource::KERNEL_FRAME_STALL) || 
                           (trigger.duration_ms >= thresholds_.smi_severity_threshold_ms);
    const bool no_preemption_anomaly = (trigger.target_tid != 0) 
        ? (target_thread_preempt_us < (thresholds_.cswitch_preempt_ms * 1000)) 
        : (core_cswitch_preempt_us[trigger.cpu_index] < (thresholds_.cswitch_preempt_ms * 1000));

    if (hypotheses.empty() && 
        dwm_candidates.empty() &&
        provider_ctx.kernel_dpc_active && 
        is_severe && 
        no_preemption_anomaly) {
        Diagnosis diag;
        diag.hypothesis = "unprofiled_hardware_or_smi_stall";
        diag.confidence = provider_ctx.kernel_cswitch_active ? 0.35 : 0.30; // Strictly capped <= 0.35
        diag.factors = { 0.35, 0.0, 1.0 };
        diag.summary = (trigger.source == TriggerSource::AUDIO_GLITCH)
            ? "Audio buffer underrun occurred without corresponding software DPC/ISR or context-switch stalls. "
              "Unprofiled hardware interrupt or BIOS SMI is suspected."
            : "Severe frame delay occurred without corresponding software DPC/ISR or context-switch stalls. "
              "Unprofiled hardware interrupt, BIOS SMI, or GPU pipeline wait is suspected.";
        hypotheses.push_back(std::move(diag));
    }

    // 17. Insufficient Evidence Fallback
    if (hypotheses.empty()) {
        Diagnosis diag;
        diag.hypothesis = "insufficient_evidence";
        diag.confidence = 0.15;
        diag.factors = { 0.0, 0.0, 0.0 };
        diag.summary = "No decisive kernel-level driver DPC, disk stall, or thread preemption anomalies were detected in this window.";
        hypotheses.push_back(std::move(diag));
    }

    // Stable sort hypotheses descending by confidence and assign ranks
    std::stable_sort(hypotheses.begin(), hypotheses.end(), [](const Diagnosis& a, const Diagnosis& b) {
        return a.confidence > b.confidence;
    });

    for (size_t i = 0; i < hypotheses.size(); ++i) {
        hypotheses[i].rank = static_cast<uint32_t>(i + 1);
    }

    report.diagnoses = std::move(hypotheses);
    return report;
}

} // namespace stuttometer
