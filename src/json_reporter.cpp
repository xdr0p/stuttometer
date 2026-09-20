#include "stuttometer/json_reporter.hpp"
#include "stuttometer/internal/redaction_utils.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cmath>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace stuttometer {

nlohmann::json JsonReporter::to_json(const DiagnosticReport& report, bool redact) const {
    nlohmann::json root;

    root["schema_version"] = report.schema_version.empty() ? "1.1" : report.schema_version;
    root["tool_version"]   = report.tool_version.empty() ? "0.4.1" : report.tool_version;
    root["timestamp_utc"]  = report.timestamp_utc;

    const std::vector<uint32_t> ids_to_redact = redact ? collect_report_ids(report) : std::vector<uint32_t>{};

    // Environment info
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 1;
    root["environment"] = {
        {"cpu_cores", cores},
        {"qpc_frequency_hz", report.qpc_frequency != 0 ? report.qpc_frequency : get_qpc_frequency()}
    };

    // Configuration snapshot
    root["configuration"] = {
        {"window_pre_ms", report.window_pre_ms},
        {"window_post_ms", report.window_post_ms},
        {"present_threshold_ms", report.present_threshold_ms},
        {"provider_tier", report.provider_tier},
        {"redacted", redact},
        {"thresholds", {
            {"dpc_threshold_us", report.thresholds.dpc_threshold_us},
            {"isr_threshold_us", report.thresholds.isr_threshold_us},
            {"disk_threshold_ms", report.thresholds.disk_threshold_ms},
            {"cswitch_preempt_ms", report.thresholds.cswitch_preempt_ms},
            {"smi_severity_threshold_ms", report.thresholds.smi_severity_threshold_ms},
            {"d3d12_pso_threshold_ms", report.thresholds.d3d12_pso_threshold_ms},
            {"vram_demoted_threshold_mb", report.thresholds.vram_demoted_threshold_mb},
            {"mem_alloc_threshold_mb", report.thresholds.mem_alloc_threshold_mb},
            {"mem_trim_threshold_mb", report.thresholds.mem_trim_threshold_mb},
            {"mem_physical_latency_us", report.thresholds.mem_physical_latency_us}
        }},
        {"active_providers", {
            {"kernel_dpc", report.provider_context.kernel_dpc_active},
            {"kernel_disk", report.provider_context.kernel_disk_active},
            {"kernel_cswitch", report.provider_context.kernel_cswitch_active},
            {"user_dxgi", report.provider_context.user_dxgi_active},
            {"user_audio", report.provider_context.user_audio_active},
            {"user_dxgkrnl", report.provider_context.user_dxgkrnl_active},
            {"user_dwm", report.provider_context.user_dwm_active},
            {"kernel_pagefault", report.provider_context.kernel_pagefault_active},
            {"user_processor_power", report.provider_context.user_processor_power_active},
            {"user_antimalware", report.provider_context.user_antimalware_active},
            {"user_d3d12", report.provider_context.user_d3d12_active},
            {"user_vram_paging", report.provider_context.user_vram_paging_active},
            {"kernel_memory", report.provider_context.kernel_memory_active}
        }}
    };

    // Trigger metadata (with deep redaction)
    nlohmann::json trig_obj = {
        {"source", trigger_source_to_string(report.trigger.source)},
        {"reason", trigger_reason_to_string(report.trigger.reason)},
        {"trigger_timestamp_qpc", report.trigger.trigger_timestamp_qpc},
        {"target_pid", redact ? 0 : report.trigger.target_pid},
        {"target_tid", redact ? 0 : report.trigger.target_tid},
        {"cpu_index", report.trigger.cpu_index},
        {"target_process", redact ? "Process_REDACTED" : report.target_process},
        {"baseline_avg_ms", report.trigger.baseline_avg_ms},
        {"baseline_fps", report.trigger.baseline_fps},
        {"spike_ratio", report.trigger.spike_ratio}
    };
    if (report.trigger.source == TriggerSource::AUDIO_GLITCH) {
        trig_obj["glitch_count"] = report.trigger.glitch_count;
        trig_obj["duration_ms"] = 0.0;
    } else {
        trig_obj["duration_ms"] = report.trigger.duration_ms;
        trig_obj["glitch_count"] = 0;
    }
    root["trigger"] = std::move(trig_obj);

    // Root-level Attribution (v1.1)
    root["attribution"] = {
        {"tag", attribution_to_std_string(report.attribution)},
        {"pid", redact ? 0 : report.attribution_pid},
        {"process", redact ? "REDACTED" : report.attribution_process}
    };

    // Diagnoses list
    nlohmann::json diag_array = nlohmann::json::array();
    for (const auto& diag : report.diagnoses) {
        nlohmann::json d_obj;
        d_obj["rank"] = diag.rank;
        d_obj["hypothesis"] = diag.hypothesis;
        d_obj["confidence"] = diag.confidence;
        d_obj["summary"] = redact ? redact_text_with_ids(diag.summary, ids_to_redact) : diag.summary;

        nlohmann::json factors_obj = {
            {"duration_severity", diag.factors.duration_severity},
            {"core_affinity_match", diag.factors.core_affinity_match},
            {"temporal_proximity", diag.factors.temporal_proximity}
        };
        d_obj["factors"] = std::move(factors_obj);

        nlohmann::json ev_array = nlohmann::json::array();
        for (const auto& ev : diag.evidence) {
            nlohmann::json ev_obj = {
                {"event_type", ev.event_type},
                {"driver_module", get_redacted_module_name(ev.driver_module, redact)},
                {"routine_address", redact ? "0xREDACTED" : ev.routine_address},
                {"duration_us", ev.duration_us},
                {"cpu_core", ev.cpu_core},
                {"offset_from_trigger_ms", ev.offset_from_trigger_ms},
                {"pid", redact ? 0 : ev.pid}
            };
            if (!ev.extra_info.empty()) {
                ev_obj["extra_info"] = redact ? redact_text_with_ids(ev.extra_info, ids_to_redact) : ev.extra_info;
            }
            ev_array.push_back(std::move(ev_obj));
        }
        d_obj["evidence"] = std::move(ev_array);
        diag_array.push_back(std::move(d_obj));
    }
    root["diagnoses"] = std::move(diag_array);

    // Frame Timeline (v1.1)
    nlohmann::json timeline_arr = nlohmann::json::array();
    for (const auto& pt : report.frame_timeline) {
        timeline_arr.push_back({
            {"frame_index", pt.frame_index},
            {"relative_index", pt.relative_index},
            {"qpc_timestamp", pt.qpc_timestamp},
            {"duration_ms", pt.duration_ms},
            {"offset_from_trigger_ms", pt.offset_from_trigger_ms},
            {"is_trigger_frame", pt.is_trigger_frame},
            {"is_pacing_stall", pt.is_pacing_stall}
        });
    }
    root["frame_timeline"] = std::move(timeline_arr);

    // Statistics and telemetry
    root["statistics"] = {
        {"total_events_in_snapshot", report.total_events},
        {"events_by_category", {
            {"DXGI", report.event_counts.dxgi},
            {"AUDIO", report.event_counts.audio},
            {"DPC", report.event_counts.dpc},
            {"ISR", report.event_counts.isr},
            {"DISK", report.event_counts.disk},
            {"CSWITCH", report.event_counts.cswitch},
            {"PROFILE", report.event_counts.profile},
            {"DXGKRNL_MMIOFLIP", report.event_counts.dxgkrnl_mmioflip},
            {"DXGKRNL_VSYNCDPC", report.event_counts.dxgkrnl_vsyncdpc},
            {"DWM_GLITCH", report.event_counts.dwm_glitch},
            {"PAGE_FAULT", report.event_counts.page_fault},
            {"THERMAL_THROTTLE", report.event_counts.thermal_throttle},
            {"ANTIMALWARE_SCAN", report.event_counts.antimalware_scan},
            {"D3D12_PSO_CREATE", report.event_counts.d3d12_pso_create},
            {"DXGKRNL_VRAM_PAGING", report.event_counts.dxgkrnl_vram_paging},
            {"MEM_VIRTUAL_ALLOC", report.event_counts.mem_virtual_alloc},
            {"MEM_WORKING_SET_TRIM", report.event_counts.mem_working_set_trim},
            {"MEM_PHYSICAL_ALLOC", report.event_counts.mem_physical_alloc}
        }},
        {"ring_buffer_dropped_events", report.dropped_events + report.producer_dropped_events},
        {"ring_buffer_extraction_drops", report.dropped_events},
        {"ring_buffer_producer_drops", report.producer_dropped_events},
        {"in_flight_unpaired_evictions", report.unpaired_evictions},
        {"in_flight_insertion_failures", report.insertion_failures},
        {"etw_events_lost_upstream", report.etw_events_lost},
        {"etw_buffers_lost_upstream", report.etw_buffers_lost}
    };

    return root;
}

std::string JsonReporter::to_json_string(const DiagnosticReport& report, bool redact, int indent) const {
    return to_json(report, redact).dump(indent);
}

bool JsonReporter::save_to_file(const DiagnosticReport& report, const std::filesystem::path& file_path, bool redact) const {
    if (file_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }
    std::ofstream out(file_path);
    if (!out.is_open()) {
        return false;
    }
    out << to_json_string(report, redact, 2);
    out.flush();
    return out.good();
}

bool JsonReporter::save_to_file(const DiagnosticReport& report, const std::string& file_path, bool redact) const {
#if defined(_WIN32)
    int num_chars = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), static_cast<int>(file_path.length()), NULL, 0);
    if (num_chars > 0) {
        std::wstring wpath(num_chars, 0);
        MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), static_cast<int>(file_path.length()), wpath.data(), num_chars);
        return save_to_file(report, std::filesystem::path(wpath), redact);
    }
#endif
    return save_to_file(report, std::filesystem::path(file_path), redact);
}

void JsonReporter::print_console_summary(const DiagnosticReport& report, std::ostream& out, bool redact) const {
    struct StreamStateRestorer {
        std::ostream& os;
        std::ios::fmtflags flags;
        std::streamsize prec;
        ~StreamStateRestorer() {
            os.flags(flags);
            os.precision(prec);
        }
    } restorer{ out, out.flags(), out.precision() };

    const std::string proc_name = redact ? "Process_REDACTED" : report.target_process;

    out << "\n================================================================================\n";
    out << " [STUTTOMETER REPORT] Stutter Anomaly Detected at " << report.timestamp_utc << "\n";
    out << "================================================================================\n";
    out << " Trigger Source : " << trigger_source_to_string(report.trigger.source)
        << " (" << trigger_reason_to_string(report.trigger.reason) << ")\n";
    if (report.trigger.baseline_avg_ms > 0.0) {
        out << " Baseline FPS   : " << std::fixed << std::setprecision(1) << report.trigger.baseline_fps 
            << " FPS (" << report.trigger.baseline_avg_ms << " ms/frame, " << report.trigger.spike_ratio << "x spike)\n";
    }
    if (redact) {
        out << " Process        : Process_REDACTED\n";
    } else {
        out << " Process        : " << proc_name << " (PID " << report.trigger.target_pid << ", TID " << report.trigger.target_tid << ")\n";
    }
    if (report.trigger.source == TriggerSource::AUDIO_GLITCH) {
        out << " Glitch Count   : " << report.trigger.glitch_count << " buffer underrun(s)\n";
    } else {
        out << " Duration       : " << std::fixed << std::setprecision(2) << report.trigger.duration_ms << " ms\n";
    }
    out << " Captured Events: " << report.total_events << " events (Drops: " << report.dropped_events 
        << ", Upstream ETW Loss: " << report.etw_events_lost << ")\n";
    out << " Attribution    : " << attribution_to_string(report.attribution);
    if (redact || report.attribution_redacted) {
        out << " (REDACTED)\n";
    } else if (report.attribution_pid != 0) {
        out << " (" << report.attribution_process << " PID " << report.attribution_pid << ")\n";
    } else {
        out << " (" << report.attribution_process << ")\n";
    }
    out << "--------------------------------------------------------------------------------\n";
    out << " RANKED ROOT CAUSE HYPOTHESES:\n";

    const std::vector<uint32_t> ids_to_redact = redact ? collect_report_ids(report) : std::vector<uint32_t>{};

    for (const auto& diag : report.diagnoses) {
        const std::string summary_str = redact ? redact_text_with_ids(diag.summary, ids_to_redact) : diag.summary;

        out << "  #" << diag.rank << " [" << std::fixed << std::setprecision(0) << std::round(diag.confidence * 100.0) << "% Conf] "
            << diag.hypothesis << "\n";
        out << "     Summary: " << summary_str << "\n";
        if (!diag.evidence.empty()) {
            out << "     Top Evidence:\n";
            for (size_t i = 0; i < std::min<size_t>(3, diag.evidence.size()); ++i) {
                const auto& ev = diag.evidence[i];
                const std::string mod_str = get_redacted_module_name(ev.driver_module, redact);
                const std::string addr_str = redact ? "0xREDACTED" : ev.routine_address;
                out << "       - " << ev.event_type << ": " << mod_str 
                    << " (" << addr_str << ") duration=" << std::fixed << std::setprecision(2) << (ev.duration_us / 1000.0) 
                    << "ms on Core " << static_cast<int>(ev.cpu_core)
                    << " (offset: " << std::fixed << std::setprecision(1) << ev.offset_from_trigger_ms << "ms)\n";
            }
        }
    }
    out << "================================================================================\n\n";
}

} // namespace stuttometer
