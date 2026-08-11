#include "stuttometer/json_reporter.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace stuttometer {

nlohmann::json JsonReporter::to_json(const DiagnosticReport& report, bool redact) const {
    nlohmann::json root;

    root["schema_version"] = report.schema_version;
    root["tool_version"]   = report.tool_version;
    root["timestamp_utc"]  = report.timestamp_utc;

    // Environment info
    root["environment"] = {
        {"cpu_cores", std::thread::hardware_concurrency()},
        {"qpc_frequency_hz", get_qpc_frequency()}
    };

    // Configuration snapshot
    root["configuration"] = {
        {"window_pre_ms", report.window_pre_ms},
        {"window_post_ms", report.window_post_ms},
        {"present_threshold_ms", report.present_threshold_ms},
        {"provider_tier", report.provider_tier},
        {"redacted", redact}
    };

    // Trigger metadata
    root["trigger"] = {
        {"source", trigger_source_to_string(report.trigger.source)},
        {"trigger_timestamp_qpc", report.trigger.trigger_timestamp_qpc},
        {"duration_ms", report.trigger.duration_ms},
        {"target_pid", report.trigger.target_pid},
        {"target_tid", report.trigger.target_tid},
        {"target_process", redact ? ("Process_" + std::to_string(report.trigger.target_pid)) : report.trigger.target_process}
    };

    // Diagnoses & Evidence
    nlohmann::json diag_array = nlohmann::json::array();
    for (const auto& diag : report.diagnoses) {
        nlohmann::json d;
        d["rank"] = diag.rank;
        d["hypothesis"] = diag.hypothesis;
        d["confidence"] = std::round(diag.confidence * 100.0) / 100.0;
        d["summary"] = diag.summary;

        d["factors"] = {
            {"duration_severity", std::round(diag.factors.duration_severity * 100.0) / 100.0},
            {"core_affinity_match", std::round(diag.factors.core_affinity_match * 100.0) / 100.0},
            {"temporal_proximity", std::round(diag.factors.temporal_proximity * 100.0) / 100.0}
        };

        nlohmann::json ev_array = nlohmann::json::array();
        for (const auto& ev : diag.evidence) {
            nlohmann::json e;
            e["event_type"] = ev.event_type;
            e["driver_module"] = ev.driver_module;
            e["routine_address"] = redact ? "0xREDACTED" : ev.routine_address;
            e["duration_us"] = ev.duration_us;
            e["cpu_core"] = ev.cpu_core;
            e["offset_from_trigger_ms"] = std::round(ev.offset_from_trigger_ms * 10.0) / 10.0;
            if (!ev.extra_info.empty()) {
                e["extra_info"] = ev.extra_info;
            }
            ev_array.push_back(std::move(e));
        }
        d["evidence"] = std::move(ev_array);
        diag_array.push_back(std::move(d));
    }
    root["diagnoses"] = std::move(diag_array);

    // Event & Loss Statistics
    root["statistics"] = {
        {"total_events_in_window", report.total_events},
        {"events_by_category", {
            {"DXGI", report.event_counts.dxgi},
            {"DPC", report.event_counts.dpc},
            {"ISR", report.event_counts.isr},
            {"DISK", report.event_counts.disk},
            {"CSWITCH", report.event_counts.cswitch},
            {"PROFILE", report.event_counts.profile}
        }},
        {"ring_buffer_dropped_events", report.dropped_events},
        {"in_flight_unpaired_evictions", report.unpaired_evictions}
    };

    return root;
}

std::string JsonReporter::to_json_string(const DiagnosticReport& report, bool redact, int indent) const {
    return to_json(report, redact).dump(indent);
}

bool JsonReporter::save_to_file(const DiagnosticReport& report, const std::string& file_path, bool redact) const {
    std::ofstream out(file_path);
    if (!out.is_open()) {
        return false;
    }
    out << to_json_string(report, redact, 2);
    return true;
}

void JsonReporter::print_console_summary(const DiagnosticReport& report, std::ostream& out, bool redact) const {
    const std::string proc_name = redact ? ("Process_" + std::to_string(report.trigger.target_pid)) : report.trigger.target_process;

    out << "\n================================================================================\n";
    out << " [STUTTOMETER REPORT] Stutter Anomaly Detected at " << report.timestamp_utc << "\n";
    out << "================================================================================\n";
    out << " Trigger Source : " << trigger_source_to_string(report.trigger.source) << "\n";
    out << " Process        : " << proc_name << " (PID " << report.trigger.target_pid << ", TID " << report.trigger.target_tid << ")\n";
    out << " Duration       : " << std::fixed << std::setprecision(2) << report.trigger.duration_ms << " ms\n";
    out << " Captured Events: " << report.total_events << " events (Drops: " << report.dropped_events << ")\n";
    out << "--------------------------------------------------------------------------------\n";
    out << " RANKED ROOT CAUSE HYPOTHESES:\n";

    for (const auto& diag : report.diagnoses) {
        out << "  #" << diag.rank << " [" << std::fixed << std::setprecision(0) << (diag.confidence * 100.0) << "% Conf] "
            << diag.hypothesis << "\n";
        out << "     Summary: " << diag.summary << "\n";
        if (!diag.evidence.empty()) {
            out << "     Top Evidence:\n";
            for (size_t i = 0; i < std::min<size_t>(3, diag.evidence.size()); ++i) {
                const auto& ev = diag.evidence[i];
                const std::string addr_str = redact ? "0xREDACTED" : ev.routine_address;
                out << "       - " << ev.event_type << ": " << ev.driver_module 
                    << " (" << addr_str << ") duration=" << (ev.duration_us / 1000.0) 
                    << "ms on Core " << static_cast<int>(ev.cpu_core)
                    << " (offset: " << std::fixed << std::setprecision(1) << ev.offset_from_trigger_ms << "ms)\n";
            }
        }
    }
    out << "================================================================================\n\n";
}

} // namespace stuttometer
