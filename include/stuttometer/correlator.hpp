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
    double smi_severity_threshold_ms{33.3};// 33.3 ms (~2 frames at 60Hz)
    uint32_t pagefault_threshold_ms{1};    // 1 ms
    uint32_t antimalware_threshold_ms{5};  // 5 ms
    uint32_t d3d12_pso_threshold_ms{5};    // 5 ms
    uint32_t vram_demoted_threshold_mb{8}; // 8 MB
    uint32_t mem_alloc_threshold_mb{16};   // 16 MB
    uint32_t mem_trim_threshold_mb{4};     // 4 MB
    uint32_t mem_physical_latency_us{1000};// 1000 us (1 ms)
};

struct ProviderContext {
    bool kernel_dpc_active{true};
    bool kernel_disk_active{true};
    bool kernel_cswitch_active{true};
    bool user_dxgi_active{true};
    bool user_audio_active{true};
    bool user_dxgkrnl_active{true};
    bool user_dwm_active{true};
    bool kernel_pagefault_active{true};
    bool user_processor_power_active{true};
    bool user_antimalware_active{true};
    bool user_d3d12_active{true};
    bool user_vram_paging_active{true};
    bool kernel_memory_active{true};
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
    uint32_t secondary_tid{0};
    uint32_t secondary_pid{0};
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
    uint64_t dxgkrnl_mmioflip{0};
    uint64_t dxgkrnl_vsyncdpc{0};
    uint64_t dwm_glitch{0};
    uint64_t page_fault{0};
    uint64_t thermal_throttle{0};
    uint64_t antimalware_scan{0};
    uint64_t d3d12_pso_create{0};
    uint64_t dxgkrnl_vram_paging{0};
    uint64_t mem_virtual_alloc{0};
    uint64_t mem_working_set_trim{0};
    uint64_t mem_physical_alloc{0};
};

struct DiagnosticReport {
    std::string schema_version{"1.0"};
    std::string tool_version{"0.1.0"};
    std::string timestamp_utc;
    TriggerInfo trigger;
    std::string target_process;

    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{25.0};
    std::string provider_tier{"standard"};
    bool redacted{false};
    uint64_t qpc_frequency{0};

    ProviderContext provider_context;
    CorrelatorThresholds thresholds;

    std::vector<Diagnosis> diagnoses;
    EventCategoryCounts event_counts;

    size_t total_events{0};
    uint64_t dropped_events{0};
    uint64_t producer_dropped_events{0};
    uint64_t unpaired_evictions{0};
    uint64_t insertion_failures{0};
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
        uint64_t unpaired_evictions = 0,
        uint64_t insertion_failures = 0,
        uint64_t producer_dropped_events = 0
    ) const;

    // Candidate tracking structures for correlation analysis
    struct DpcCandidate { EtwEventRecord record; double offset_ms{0.0}; std::string driver_name; };
    struct DiskCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct CSwitchCandidate { EtwEventRecord record; double offset_ms{0.0}; bool is_resumption{true}; };
    struct DwmCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct PageFaultCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct AntimalwareCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct D3D12PsoCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct VramPagingCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct MemVirtualAllocCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct MemTrimCandidate { EtwEventRecord record; double offset_ms{0.0}; };
    struct MemPhysicalAllocCandidate { EtwEventRecord record; double offset_ms{0.0}; };

private:
    const DriverSymbolResolver& driver_resolver_;
    const CorrelatorThresholds thresholds_;
};

} // namespace stuttometer
