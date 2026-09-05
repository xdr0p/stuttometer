#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <windows.h>
#include <cguid.h>
#include <evntrace.h>
#include <evntcons.h>

#include "event_types.hpp"
#include "flight_recorder.hpp"
#include "trigger_engine.hpp"
#include "fixed_table.hpp"
#include "privilege_utils.hpp"

namespace stuttometer {

inline constexpr GUID DXGI_PROVIDER_GUID = {
    0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 }
};

inline constexpr GUID AUDIO_PROVIDER_GUID = {
    0xAE4BD3BE, 0xF36F, 0x45B6, { 0x8D, 0x21, 0xBD, 0xD6, 0xFB, 0x83, 0x28, 0x53 }
};

inline constexpr GUID DXGKRNL_PROVIDER_GUID = {
    0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D }
};

inline constexpr GUID DWM_CORE_PROVIDER_GUID = {
    0x9E9BBA3C, 0x2E38, 0x42CB, { 0xA2, 0x68, 0x96, 0xF9, 0xBB, 0x8D, 0xDB, 0x8C }
};

inline constexpr GUID SYSTEM_TRACE_CONTROL_GUID = {
    0x9E814069, 0x11A2, 0x47AE, { 0x99, 0xCC, 0x61, 0x04, 0x4B, 0x1E, 0x3E, 0x7C }
};

inline constexpr GUID PERFINFO_GUID = {
    0xCE1DB73A, 0x284E, 0x417B, { 0xBE, 0x4D, 0x7A, 0x13, 0x41, 0x79, 0x64, 0x44 }
};

inline constexpr GUID THREAD_GUID = {
    0x3D6FA8D1, 0xFE05, 0x11D0, { 0x9D, 0x06, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x40 }
};

inline constexpr GUID DISK_IO_GUID = {
    0x3D6FA8D4, 0xFE05, 0x11D0, { 0x9D, 0x06, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x40 }
};

inline constexpr GUID PAGE_FAULT_GUID = {
    0x3D6FA8D2, 0xFE05, 0x11D0, { 0x9D, 0x06, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x40 }
};

inline constexpr GUID KERNEL_PROCESSOR_POWER_GUID = {
    0x0F67E49F, 0xFE51, 0x4E9F, { 0xB4, 0x90, 0x6F, 0x29, 0x48, 0xCC, 0x60, 0x27 }
};

inline constexpr GUID ANTIMALWARE_ENGINE_GUID = {
    0x0A002690, 0x3839, 0x4E3A, { 0xB3, 0xB6, 0x96, 0xD8, 0xDF, 0x86, 0x8D, 0x99 }
};

inline constexpr GUID DIRECT3D12_PROVIDER_GUID = {
    0x5D8087DD, 0x3A9B, 0x4F56, { 0x90, 0xDF, 0x49, 0x19, 0x6C, 0xDC, 0x4F, 0x11 }
};

inline constexpr GUID KERNEL_MEMORY_PROVIDER_GUID = {
    0xD1D93EF7, 0xE1F2, 0x4F45, { 0x99, 0x43, 0x03, 0xD2, 0x45, 0xFE, 0x6C, 0x00 }
};

inline constexpr GUID KERNEL_PROCESS_PROVIDER_GUID = {
    0x22FB2CD6, 0x0E7B, 0x422B, { 0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16 }
};

enum class SessionStartResult {
    SUCCESS              = 0,
    DEGRADED_USER_ONLY   = 1,
    DEGRADED_KERNEL_ONLY = 2,
    FAILED               = 3
};

struct EtwSessionConfig {
    bool enable_dxgi{true};
    bool enable_audio{true};
    bool enable_kernel_dpc{true};
    bool enable_kernel_disk{true};
    bool enable_kernel_cswitch{true};
    bool enable_kernel_profile{false};
    bool enable_dxgkrnl{true};
    bool enable_dwm_core{true};
    bool enable_kernel_pagefault{true};
    bool enable_processor_power{true};
    bool enable_antimalware{true};
    bool enable_d3d12{true};
    bool enable_kernel_memory{true};
    bool enable_kernel_process_events{true};
    uint32_t flush_interval_ms{100};
};

struct PresentInFlight {
    uint64_t start_qpc{0};
    uint32_t pid{0};
    uint32_t tid{0};
};

struct DiskInFlight {
    uint64_t start_qpc{0};
    uint32_t pid{0};
    uint32_t tid{0};
    bool is_write{false};
};

struct AntimalwareScanInFlight {
    uint64_t start_qpc{0};
    uint32_t pid{0};
    uint32_t tid{0};
};

struct PsoInFlight {
    uint64_t start_qpc{0};
    uint32_t pid{0};
    uint32_t tid{0};
    uint64_t pso_ptr{0};
    uint16_t flags{0};
    uint8_t  _pad[6]{0};
};
static_assert(sizeof(PsoInFlight) == 32, "PsoInFlight must be 32 bytes");
static_assert(std::is_trivially_copyable_v<PsoInFlight>, "PsoInFlight must be trivially copyable");

struct WorkingSetTrimInFlight {
    uint64_t start_qpc{0};
    uint32_t pid{0};
    uint32_t pad{0};
};
static_assert(sizeof(WorkingSetTrimInFlight) == 16, "WorkingSetTrimInFlight must be 16 bytes");
static_assert(std::is_trivially_copyable_v<WorkingSetTrimInFlight>, "WorkingSetTrimInFlight must be trivially copyable");

inline constexpr uint8_t KERNEL_OPCODE_VIRTUAL_ALLOC = 98;

struct ThreadSwitchOut {
    uint64_t qpc{0};
    uint32_t pid{0};
    uint8_t wait_state{0};
};

struct TidPidEntry {
    uint32_t pid{0};
    uint64_t last_seen_qpc{0};
};

struct LastPresentEntry {
    uint64_t last_present_qpc{0};
    uint32_t pid{0};
    uint32_t tid{0};
};

struct LastFlipEntry {
    uint64_t last_flip_qpc{0};
    uint32_t swapchain_hash{0};
    uint32_t pid{0};
    uint32_t tid{0};
};

static_assert(sizeof(PresentInFlight) == 16, "PresentInFlight must be 16 bytes");
static_assert(std::is_trivially_copyable_v<PresentInFlight>, "PresentInFlight must be trivially copyable");
static_assert(sizeof(DiskInFlight) == 24, "DiskInFlight must be 24 bytes");
static_assert(std::is_trivially_copyable_v<DiskInFlight>, "DiskInFlight must be trivially copyable");
static_assert(sizeof(AntimalwareScanInFlight) == 16, "AntimalwareScanInFlight must be 16 bytes");
static_assert(std::is_trivially_copyable_v<AntimalwareScanInFlight>, "AntimalwareScanInFlight must be trivially copyable");
static_assert(sizeof(ThreadSwitchOut) == 16, "ThreadSwitchOut must be 16 bytes");
static_assert(std::is_trivially_copyable_v<ThreadSwitchOut>, "ThreadSwitchOut must be trivially copyable");
static_assert(sizeof(TidPidEntry) == 16, "TidPidEntry must be 16 bytes");
static_assert(std::is_trivially_copyable_v<TidPidEntry>, "TidPidEntry must be trivially copyable");
static_assert(sizeof(LastPresentEntry) == 16, "LastPresentEntry must be 16 bytes");
static_assert(std::is_trivially_copyable_v<LastPresentEntry>, "LastPresentEntry must be trivially copyable");
static_assert(sizeof(LastFlipEntry) == 24, "LastFlipEntry must be 24 bytes");
static_assert(std::is_trivially_copyable_v<LastFlipEntry>, "LastFlipEntry must be trivially copyable");

struct PresentDeltaResult {
    uint64_t effective_dur_us{0};
    bool is_baseline_reset{false};
};

static inline PresentDeltaResult calculate_effective_present_duration(
    uint64_t current_timestamp_qpc,
    uint64_t previous_timestamp_qpc,
    uint64_t present_start_qpc,
    uint64_t qpc_freq,
    uint64_t max_pause_ceiling_us = 10000000ULL // 10s ceiling for loading/Alt-Tab
) noexcept {
    PresentDeltaResult result{};
    uint64_t api_dur_us = 0;
    if (present_start_qpc > 0 && current_timestamp_qpc >= present_start_qpc) {
        api_dur_us = static_cast<uint64_t>(qpc_delta_to_us(current_timestamp_qpc - present_start_qpc, qpc_freq));
    }

    if (previous_timestamp_qpc == 0 || current_timestamp_qpc <= previous_timestamp_qpc) {
        // First frame seen on this swapchain: baseline initialization
        result.effective_dur_us = api_dur_us;
        result.is_baseline_reset = true;
        return result;
    }

    uint64_t frametime_us = static_cast<uint64_t>(qpc_delta_to_us(current_timestamp_qpc - previous_timestamp_qpc, qpc_freq));

    if (frametime_us > max_pause_ceiling_us) {
        // Long pause or Alt-Tab resume: reset baseline
        result.effective_dur_us = api_dur_us;
        result.is_baseline_reset = true;
        return result;
    }

    result.effective_dur_us = (frametime_us > api_dur_us) ? frametime_us : api_dur_us;
    result.is_baseline_reset = false;
    return result;
}

static inline uint64_t make_present_key(uint32_t tid, uint64_t swapchain_ptr) noexcept {
    // Fallback to 0x1ULL (guaranteed-unmapped 64KB null-page zone) when swapchain_ptr is null
    uint64_t ptr_val = swapchain_ptr ? swapchain_ptr : 0x1ULL;
    uint64_t k = (static_cast<uint64_t>(tid) << 32) | (tid ^ 0x9E3779B9U);
    k ^= ptr_val + 0x517cc1b727220a95ULL + (k << 6) + (k >> 2);
    k ^= (k >> 30);
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= (k >> 27);
    k *= 0x94d049bb133111ebULL;
    k ^= (k >> 31);
    return (k != 0) ? k : 0xCAFEBABEDEADBEEFULL;
}

static inline uint64_t make_flip_key(uint32_t vidpn_source_id, uint64_t swapchain_ptr) noexcept {
    // Fallback to 0x1ULL (guaranteed-unmapped 64KB null-page zone) when swapchain_ptr is null
    uint64_t ptr_val = swapchain_ptr ? swapchain_ptr : 0x1ULL;
    uint64_t k = (static_cast<uint64_t>(vidpn_source_id) << 32) | (vidpn_source_id ^ 0x9E3779B9U);
    k ^= ptr_val + 0x517cc1b727220a95ULL + (k << 6) + (k >> 2);
    k ^= (k >> 30);
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= (k >> 27);
    k *= 0x94d049bb133111ebULL;
    k ^= (k >> 31);
    return (k != 0) ? k : 0xFEEDFACECAFEBEEFULL;
}

static inline uint64_t make_pso_key(uint32_t tid, uint64_t pso_ptr) noexcept {
    uint64_t ptr_val = pso_ptr ? pso_ptr : 0x1ULL;
    uint64_t k = (static_cast<uint64_t>(tid) << 32) | (tid ^ 0x9E3779B9U);
    k ^= ptr_val + 0x517cc1b727220a95ULL + (k << 6) + (k >> 2);
    k ^= (k >> 30);
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= (k >> 27);
    k *= 0x94d049bb133111ebULL;
    k ^= (k >> 31);
    return (k != 0) ? k : 0xDEADBEEFCAFE0001ULL;
}

class EtwSessionManager {
public:
    EtwSessionManager(
        FlightRecorder& flight_recorder,
        TriggerEngine& trigger_engine,
        const EtwSessionConfig& config = EtwSessionConfig{}
    );
    ~EtwSessionManager();

    // Starts the ETW trace sessions and consumer threads
    SessionStartResult start();

    // Stops sessions and flushes remaining buffers
    void stop();

    // Granular thread-safe teardown of individual sessions on consumer failure
    void stop_user_session() noexcept;
    void stop_kernel_session() noexcept;

    // Synchronously flushes kernel and user buffers
    void flush_buffers() noexcept;

    // Highest QPC timestamp processed by consumer thread(s)
    uint64_t last_processed_qpc() const noexcept {
        return last_processed_qpc_.load(std::memory_order_acquire);
    }

    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }
    bool is_user_session_active() const noexcept { 
        return user_session_handle_.load(std::memory_order_relaxed) != 0 && !user_consumer_failed(); 
    }
    bool is_kernel_session_active() const noexcept { 
        return kernel_session_handle_.load(std::memory_order_relaxed) != 0 && !kernel_consumer_failed(); 
    }
    bool user_consumer_failed() const noexcept { return user_consumer_failed_.load(std::memory_order_relaxed); }
    bool kernel_consumer_failed() const noexcept { return kernel_consumer_failed_.load(std::memory_order_relaxed); }
    bool consumer_failed() const noexcept { return user_consumer_failed() || kernel_consumer_failed(); }

    uint32_t events_lost() const noexcept { 
        return user_events_lost_.load(std::memory_order_relaxed) + 
               kernel_events_lost_.load(std::memory_order_relaxed); 
    }
    uint32_t buffers_lost() const noexcept { 
        return user_buffers_lost_.load(std::memory_order_relaxed) + 
               kernel_buffers_lost_.load(std::memory_order_relaxed); 
    }
    uint64_t unpaired_evictions() const noexcept {
        return in_flight_present_.unpaired_evictions() +
               in_flight_disk_.unpaired_evictions() +
               in_flight_scans_.unpaired_evictions() +
               in_flight_pso_table_.unpaired_evictions() +
               in_flight_ws_trims_.unpaired_evictions() +
               in_flight_threads_.unpaired_evictions() +
               last_present_table_.unpaired_evictions() +
               last_flip_table_.unpaired_evictions();
    }
    uint64_t insertion_failures() const noexcept {
        return in_flight_present_.insertion_failures() +
               in_flight_disk_.insertion_failures() +
               in_flight_scans_.insertion_failures() +
               in_flight_pso_table_.insertion_failures() +
               in_flight_ws_trims_.insertion_failures() +
               in_flight_threads_.insertion_failures() +
               tid_to_pid_.insertion_failures() +
               last_present_table_.insertion_failures() +
               last_flip_table_.insertion_failures();
    }

    // Static callback entry point for ProcessTrace
    static void WINAPI on_event_record(PEVENT_RECORD p_event);

private:
    void active_flush_worker_loop();
    void user_trace_consumer_loop();
    void kernel_trace_consumer_loop();

    FlightRecorder& flight_recorder_;
    TriggerEngine& trigger_engine_;
    const EtwSessionConfig config_;
    const uint64_t qpc_freq_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> last_processed_qpc_{0};
    std::atomic<uint64_t> recent_dwm_glitches_qpc_[16]{};
    std::atomic<uint32_t> recent_dwm_glitch_idx_{0};
    std::atomic<uint64_t> sync_time_utc_{0};
    std::atomic<uint64_t> sync_time_qpc_{0};
    std::atomic<uint32_t> user_events_lost_{0};
    std::atomic<uint32_t> kernel_events_lost_{0};
    std::atomic<uint32_t> user_buffers_lost_{0};
    std::atomic<uint32_t> kernel_buffers_lost_{0};

    std::atomic<bool> user_consumer_failed_{false};
    std::atomic<bool> kernel_consumer_failed_{false};

    std::atomic<TRACEHANDLE> user_session_handle_{0};
    std::atomic<TRACEHANDLE> kernel_session_handle_{0};
    std::atomic<TRACEHANDLE> user_trace_handle_{INVALID_PROCESSTRACE_HANDLE};
    std::atomic<TRACEHANDLE> kernel_trace_handle_{INVALID_PROCESSTRACE_HANDLE};

    std::thread flush_worker_thread_;
    std::thread user_consumer_thread_;
    std::thread kernel_consumer_thread_;

    // Lock-free in-flight tracking tables
    FixedInFlightTable<PresentInFlight, 2048> in_flight_present_;
    FixedInFlightTable<DiskInFlight, 2048> in_flight_disk_;
    FixedInFlightTable<AntimalwareScanInFlight, 512> in_flight_scans_;
    FixedInFlightTable<PsoInFlight, 1024> in_flight_pso_table_;
    FixedInFlightTable<WorkingSetTrimInFlight, 256> in_flight_ws_trims_;
    FixedInFlightTable<ThreadSwitchOut, 65536> in_flight_threads_;
    FixedInFlightTable<TidPidEntry, 32768> tid_to_pid_;
    FixedInFlightTable<LastPresentEntry, 2048> last_present_table_;
    FixedInFlightTable<LastFlipEntry, 2048> last_flip_table_;
};

} // namespace stuttometer
