#include "stuttometer/correlator.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>

namespace stuttometer {

static std::string format_hex_address(uint64_t addr) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::hex << addr;
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
    uint64_t dropped_events,
    uint64_t unpaired_evictions
) const {
    DiagnosticReport report;
    report.timestamp_utc = get_current_utc_timestamp();
    report.trigger = trigger;
    report.total_events = snapshot.size();
    report.dropped_events = dropped_events;
    report.unpaired_evictions = unpaired_evictions;

    // 1. Accumulate category counts & search for anomaly candidates
    struct DpcCandidate {
        EtwEventRecord record;
        double offset_ms{0.0};
        std::string driver_name;
    };
    std::vector<DpcCandidate> dpc_candidates;

    struct DiskCandidate {
        EtwEventRecord record;
        double offset_ms{0.0};
    };
    std::vector<DiskCandidate> disk_candidates;

    struct CSwitchCandidate {
        EtwEventRecord record;
        double offset_ms{0.0};
    };
    std::vector<CSwitchCandidate> cswitch_candidates;

    uint32_t profile_ticks = 0;
    uint64_t total_dpc_us = 0;
    uint64_t total_cswitch_preempt_us = 0;

    for (const auto& rec : snapshot) {
        const double offset_ms = (rec.qpc_timestamp >= trigger.trigger_timestamp_qpc)
            ? qpc_delta_to_ms(rec.qpc_timestamp - trigger.trigger_timestamp_qpc, qpc_freq)
            : -qpc_delta_to_ms(trigger.trigger_timestamp_qpc - rec.qpc_timestamp, qpc_freq);

        switch (static_cast<EventCategory>(rec.category)) {
            case EventCategory::DXGI:
                ++report.event_counts.dxgi;
                break;
            case EventCategory::DPC:
                ++report.event_counts.dpc;
                total_dpc_us += rec.duration_us;
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
                total_dpc_us += rec.duration_us;
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
                if (rec.tid == trigger.target_tid && !(rec.flags & EventFlags::CSWITCH_VOLUNTARY)) {
                    total_cswitch_preempt_us += rec.duration_us;
                    if (rec.duration_us >= (thresholds_.cswitch_preempt_ms * 1000)) {
                        CSwitchCandidate cand;
                        cand.record = rec;
                        cand.offset_ms = offset_ms;
                        cswitch_candidates.push_back(std::move(cand));
                    }
                }
                break;
            case EventCategory::PROFILE:
                ++report.event_counts.profile;
                ++profile_ticks;
                break;
            default:
                break;
        }
    }

    std::vector<Diagnosis> hypotheses;

    // 2. Evaluate DPC / ISR Anomaly Hypothesis
    if (!dpc_candidates.empty()) {
        std::sort(dpc_candidates.begin(), dpc_candidates.end(), [](const DpcCandidate& a, const DpcCandidate& b) {
            return a.record.duration_us > b.record.duration_us;
        });

        const auto& worst = dpc_candidates.front();
        const double duration_severity = std::min(1.0, static_cast<double>(worst.record.duration_us) / 3000.0);
        const double temporal_proximity = std::max(0.0, 1.0 - (std::abs(worst.offset_ms) / 150.0));
        const double core_match = 1.0;

        const double confidence = std::min(0.98, 0.45 + (0.35 * duration_severity) + (0.18 * temporal_proximity));

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

        for (const auto& cand : dpc_candidates) {
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
        const double is_target_proc = (worst.record.pid == trigger.target_pid) ? 1.0 : 0.0;
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

        for (const auto& cand : disk_candidates) {
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

        const double confidence = std::min(0.85, 0.40 + (0.35 * duration_severity) + (0.10 * temporal_proximity));

        Diagnosis diag;
        diag.hypothesis = "context_switch_interference";
        diag.confidence = confidence;
        diag.factors = { duration_severity, 1.0, temporal_proximity };

        std::stringstream ss;
        ss << "Critical thread " << trigger.target_tid << " was involuntarily preempted for "
           << std::fixed << std::setprecision(1) << preempt_ms << "ms (switched out for TID "
           << worst.record.payload.cswitch.prev_tid << ").";
        diag.summary = ss.str();

        for (const auto& cand : cswitch_candidates) {
            EvidenceItem ev;
            ev.event_type = "CSWITCH";
            ev.driver_module = "ntoskrnl.exe";
            ev.routine_address = format_hex_address(cand.record.payload.cswitch.prev_tid);
            ev.duration_us = cand.record.duration_us;
            ev.cpu_core = cand.record.cpu_index;
            ev.offset_from_trigger_ms = cand.offset_ms;
            ev.extra_info = "Descheduled for " + std::to_string(cand.record.duration_us / 1000) + "ms";
            diag.evidence.push_back(std::move(ev));
        }

        hypotheses.push_back(std::move(diag));
    }

    // 5. Constrained SMI / Unprofiled Hardware Gap Check
    if (hypotheses.empty() && trigger.duration_ms >= 30.0 && total_dpc_us < 1000 && total_cswitch_preempt_us < 2000) {
        Diagnosis diag;
        diag.hypothesis = "unprofiled_hardware_or_smi_stall";
        diag.confidence = 0.35; // Strictly capped <= 0.35
        diag.factors = { 0.35, 0.0, 1.0 };
        diag.summary = "Severe frame delay occurred without corresponding software DPC/ISR or context-switch stalls. "
                       "Unprofiled hardware interrupt, BIOS SMI, or GPU pipeline wait is suspected.";
        hypotheses.push_back(std::move(diag));
    }

    // 6. Insufficient Evidence Fallback
    if (hypotheses.empty()) {
        Diagnosis diag;
        diag.hypothesis = "insufficient_evidence";
        diag.confidence = 0.15;
        diag.factors = { 0.0, 0.0, 0.0 };
        diag.summary = "No decisive kernel-level driver DPC, disk stall, or thread preemption anomalies were detected in this window.";
        hypotheses.push_back(std::move(diag));
    }

    // Sort hypotheses descending by confidence and assign ranks
    std::sort(hypotheses.begin(), hypotheses.end(), [](const Diagnosis& a, const Diagnosis& b) {
        return a.confidence > b.confidence;
    });

    for (size_t i = 0; i < hypotheses.size(); ++i) {
        hypotheses[i].rank = static_cast<uint32_t>(i + 1);
    }

    report.diagnoses = std::move(hypotheses);
    return report;
}

} // namespace stuttometer
