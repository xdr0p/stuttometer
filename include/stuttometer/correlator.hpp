#pragma once

#include "event_types.hpp"
#include "trigger_engine.hpp"
#include "privilege_utils.hpp"
#include <string>
#include <vector>

namespace stuttometer {

struct EvidenceItem {
    std::string event_type;
    std::string driver_module;
    std::string routine_address;
    uint32_t duration_us{0};
    uint8_t cpu_core{0};
    double offset_from_trigger_ms{0.0};
    std::string extra_info;
};

struct ConfidenceFactors {
    double duration_severity{0.0};
    double core_affinity_match{0.0};
    double temporal_proximity{0.0};
};

struct Diagnosis {
    uint32_t rank{1};
    std::string hypothesis;
    double confidence{0.0};
    std::string summary;
    ConfidenceFactors factors;
    std::vector<EvidenceItem> evidence;
};

struct EventCategoryCounts {
    uint64_t dxgi{0};
    uint64_t dpc{0};
    uint64_t isr{0};
    uint64_t disk{0};
    uint64_t cswitch{0};
    uint64_t profile{0};
};

struct DiagnosticReport {
    std::string schema_version{"1.0"};
    std::string tool_version{"0.1.0"};
    std::string timestamp_utc;
    TriggerInfo trigger;
    std::vector<Diagnosis> diagnoses;
    EventCategoryCounts event_counts;
    uint64_t total_events{0};
    uint64_t dropped_events{0};
    uint64_t unpaired_evictions{0};
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{25.0};
    std::string provider_tier{"full"};
    bool redacted{false};
};

struct CorrelatorThresholds {
    uint32_t dpc_threshold_us{1000};    // Single DPC > 1.0ms is anomalous
    uint32_t isr_threshold_us{500};     // Single ISR > 0.5ms is anomalous
    uint32_t disk_threshold_ms{20};     // Disk I/O > 20ms is anomalous
    uint32_t cswitch_preempt_ms{5};     // Involuntary preemption > 5ms is anomalous
};

class CorrelationEngine {
public:
    explicit CorrelationEngine(
        const DriverSymbolResolver& driver_resolver,
        const CorrelatorThresholds& thresholds = CorrelatorThresholds{}
    );
    ~CorrelationEngine() = default;

    // Analyzes a chronologically sorted snapshot window and produces ranked diagnostic hypotheses
    DiagnosticReport correlate(
        const std::vector<EtwEventRecord>& snapshot,
        const TriggerInfo& trigger,
        uint64_t qpc_freq,
        uint64_t dropped_events = 0,
        uint64_t unpaired_evictions = 0
    ) const;

private:
    const DriverSymbolResolver& driver_resolver_;
    CorrelatorThresholds thresholds_;
};

} // namespace stuttometer
