#pragma once

#include "event_types.hpp"
#include "trigger_engine.hpp"
#include "privilege_utils.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace stuttometer {

struct CorrelatorThresholds {
    uint32_t dpc_threshold_us{1000};       // 1.0 ms
    uint32_t isr_threshold_us{500};        // 0.5 ms
    uint32_t disk_threshold_ms{20};        // 20 ms
    uint32_t cswitch_preempt_ms{5};        // 5 ms
};

struct ProviderContext {
    bool kernel_dpc_active{true};
    bool kernel_disk_active{true};
    bool kernel_cswitch_active{true};
    bool user_dxgi_active{true};
    bool user_audio_active{true};
    uint32_t etw_events_lost{0};
    uint32_t etw_buffers_lost{0};
};

struct ConfidenceFactors {
    double duration_severity{0.0};
    double core_affinity_match{0.0};
    double temporal_proximity{0.0};
};

struct EvidenceItem {
    std::string event_type;
    std::string driver_module;
    std::string routine_address;
    uint32_t duration_us{0};
    uint8_t cpu_core{0};
    double offset_from_trigger_ms{0.0};
    std::string extra_info;
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
    uint64_t audio{0};
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

    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{25.0};
    std::string provider_tier{"full"};
    bool redacted{false};

    std::vector<Diagnosis> diagnoses;
    EventCategoryCounts event_counts;

    size_t total_events{0};
    uint64_t dropped_events{0};
    uint64_t unpaired_evictions{0};
    uint32_t etw_events_lost{0};
    uint32_t etw_buffers_lost{0};
};

class CorrelationEngine {
public:
    explicit CorrelationEngine(
        const DriverSymbolResolver& driver_resolver,
        const CorrelatorThresholds& thresholds = CorrelatorThresholds{}
    );
    ~CorrelationEngine() = default;

    // Evaluates a flight recorder snapshot against the trigger event with provider context
    DiagnosticReport correlate(
        const std::vector<EtwEventRecord>& snapshot,
        const TriggerInfo& trigger,
        uint64_t qpc_freq,
        const ProviderContext& provider_ctx = ProviderContext{},
        uint64_t dropped_events = 0,
        uint64_t unpaired_evictions = 0
    ) const;

private:
    const DriverSymbolResolver& driver_resolver_;
    const CorrelatorThresholds thresholds_;
};

} // namespace stuttometer
