#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "stuttometer/constants.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/csv_exporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/trigger_engine.hpp"

namespace stuttometer::session_helpers {

[[nodiscard]] inline EtwSessionConfig make_etw_config(std::string_view tier, bool enable_audio) noexcept {
    EtwSessionConfig etw_config;
    etw_config.enable_dxgi = true;
    etw_config.enable_audio = enable_audio;
    etw_config.enable_kernel_dpc = (tier != "minimal");
    etw_config.enable_kernel_disk = (tier != "minimal");
    etw_config.enable_kernel_cswitch = (tier == "full");
    etw_config.enable_dxgkrnl = (tier != "minimal");
    etw_config.enable_dwm_core = (tier != "minimal");
    etw_config.enable_kernel_pagefault = (tier != "minimal");
    etw_config.enable_processor_power = (tier != "minimal");
    etw_config.enable_antimalware = (tier != "minimal");
    etw_config.enable_d3d12 = (tier != "minimal");
    etw_config.enable_kernel_memory = (tier != "minimal");
    return etw_config;
}

template <typename ConfigT>
[[nodiscard]] inline CorrelatorThresholds make_correlator_thresholds(const ConfigT& cfg) noexcept {
    CorrelatorThresholds thresholds;
    thresholds.dpc_threshold_us = cfg.dpc_threshold_us;
    thresholds.isr_threshold_us = cfg.isr_threshold_us;
    thresholds.disk_threshold_ms = cfg.disk_threshold_ms;
    thresholds.cswitch_preempt_ms = cfg.cswitch_preempt_ms;
    thresholds.smi_severity_threshold_ms = cfg.smi_severity_threshold_ms;
    thresholds.d3d12_pso_threshold_ms = cfg.d3d12_pso_threshold_ms;
    thresholds.vram_demoted_threshold_mb = cfg.vram_demoted_threshold_mb;
    thresholds.mem_alloc_threshold_mb = cfg.mem_alloc_threshold_mb;
    thresholds.mem_trim_threshold_mb = cfg.mem_trim_threshold_mb;
    thresholds.mem_physical_latency_us = cfg.mem_physical_latency_us;
    if (cfg.smi_threshold_manual || cfg.smi_severity_threshold_ms != 33.3) {
        thresholds.auto_scale_smi = false;
    }
    return thresholds;
}

[[nodiscard]] inline ProviderContext build_provider_context(
    const EtwSessionManager& session_mgr,
    const EtwSessionConfig& etw_config) noexcept
{
    ProviderContext p_ctx;
    p_ctx.kernel_dpc_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_dpc;
    p_ctx.kernel_disk_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_disk;
    p_ctx.kernel_cswitch_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_cswitch;
    p_ctx.user_dxgi_active = session_mgr.is_user_session_active() && etw_config.enable_dxgi;
    p_ctx.user_audio_active = session_mgr.is_user_session_active() && etw_config.enable_audio;
    p_ctx.user_dxgkrnl_active = session_mgr.is_user_session_active() && etw_config.enable_dxgkrnl;
    p_ctx.user_dwm_active = session_mgr.is_user_session_active() && etw_config.enable_dwm_core;
    p_ctx.kernel_pagefault_active = session_mgr.is_kernel_session_active() && etw_config.enable_kernel_pagefault;
    p_ctx.user_processor_power_active = session_mgr.is_user_session_active() && etw_config.enable_processor_power;
    p_ctx.user_antimalware_active = session_mgr.is_user_session_active() && etw_config.enable_antimalware;
    p_ctx.user_d3d12_active = session_mgr.is_user_session_active() && etw_config.enable_d3d12;
    p_ctx.user_vram_paging_active = session_mgr.is_user_session_active() && etw_config.enable_dxgkrnl;
    // Note: Kernel-Memory provider runs on the user trace session, not the NT Kernel Logger
    p_ctx.kernel_memory_active = session_mgr.is_user_session_active() && etw_config.enable_kernel_memory;
    p_ctx.etw_events_lost = session_mgr.events_lost();
    p_ctx.etw_buffers_lost = session_mgr.buffers_lost();
    return p_ctx;
}

inline void capture_trigger_snapshot(
    EtwSessionManager& session_mgr,
    FlightRecorder& flight_recorder,
    uint64_t from_qpc,
    uint64_t to_qpc,
    std::vector<EtwEventRecord>& snapshot_buffer,
    uint64_t& out_drops)
{
    session_mgr.flush_buffers();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(POST_TRIGGER_DRAIN_BUDGET_MS);
    while (session_mgr.last_processed_qpc() < to_qpc && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POST_TRIGGER_DRAIN_STEP_MS));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(POST_TRIGGER_DRAIN_STEP_MS));

    out_drops = 0;
    flight_recorder.snapshot(from_qpc, to_qpc, snapshot_buffer, &out_drops);
}

inline void save_per_trigger_reports(
    const DiagnosticReport& report,
    const std::filesystem::path& dir,
    uint32_t report_count,
    uint64_t current_qpc,
    bool redact,
    JsonReporter& reporter,
    std::string_view log_prefix)
{
    const std::string json_name = "stutto_report_" + std::to_string(report_count + 1) + "_" + std::to_string(current_qpc) + ".json";
    if (reporter.save_to_file(report, dir / json_name, redact)) {
        rotate_directory_by_prefix(dir, "stutto_report_", ".json", 100);
    } else {
        std::cerr << log_prefix << "Failed to write report to '" << (dir / json_name).generic_string() << "'\n";
    }
    const std::string csv_name = "stutto_pacing_" + std::to_string(report_count + 1) + "_" + std::to_string(current_qpc) + ".csv";
    if (csv::export_to_file(report, dir / csv_name)) {
        rotate_directory_by_prefix(dir, "stutto_pacing_", ".csv", 100);
    }
}

inline void drain_filtered_events(TriggerEngine& engine, NdjsonWriter& writer, int max_count = -1) {
    FilteredEvent fe;
    int drained = 0;
    while ((max_count < 0 || drained < max_count) && engine.pop_filtered_event(fe)) {
        EtwEventRecord rec{};
        rec.qpc_timestamp  = fe.qpc_timestamp;
        rec.duration_us    = static_cast<uint32_t>(fe.duration_ms * 1000.0);
        rec.pid            = fe.target_pid;
        rec.tid            = fe.target_tid;
        rec.cpu_index      = fe.cpu_index;
        rec.category       = static_cast<uint16_t>(EventCategory::TRIGGER_FILTERED);
        rec.event_id       = fe.reason;
        rec.flags          = fe.source;
        rec.auxiliary_data = static_cast<uint64_t>(fe.severity) | (static_cast<uint64_t>(fe.filter_kind) << 4);
        writer.push(rec);
        ++drained;
    }
}

} // namespace stuttometer::session_helpers
