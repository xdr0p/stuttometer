#pragma once

#include "event_types.hpp"
#include "flight_recorder.hpp"
#include "trigger_engine.hpp"
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace stuttometer {

// Standard Microsoft-Windows-DXGI Provider GUID
// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
inline constexpr GUID DXGI_PROVIDER_GUID = {
    0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 }
};

// Microsoft-Windows-Audio Provider GUID
// {AE4BD3BE-F36F-45B6-8D21-BDD6FB832853}
inline constexpr GUID AUDIO_PROVIDER_GUID = {
    0xAE4BD3BE, 0xF36F, 0x45B6, { 0x8D, 0x21, 0xBD, 0xD6, 0xFB, 0x83, 0x28, 0x53 }
};

// SystemTraceControlGuid for NT Kernel Logger
// {9E814069-11A2-47AE-99CC-61044B1E3E7C}
inline constexpr GUID SYSTEM_TRACE_CONTROL_GUID = {
    0x9E814069, 0x11A2, 0x47AE, { 0x99, 0xCC, 0x61, 0x04, 0x4B, 0x1E, 0x3E, 0x7C }
};

struct EtwSessionConfig {
    bool enable_dxgi{true};
    bool enable_audio{true};
    bool enable_kernel_dpc{true};
    bool enable_kernel_disk{true};
    bool enable_kernel_cswitch{true};
    bool enable_kernel_profile{false};
    uint32_t flush_interval_ms{30};
};

class EtwSessionManager {
public:
    EtwSessionManager(
        FlightRecorder& flight_recorder,
        TriggerEngine& trigger_engine,
        const EtwSessionConfig& config = EtwSessionConfig{}
    );
    ~EtwSessionManager();

    // Starts the ETW trace sessions and consumer threads
    bool start();

    // Stops sessions and flushes remaining buffers
    void stop();

    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    // Static callback entry point for ProcessTrace
    static void WINAPI on_event_record(PEVENT_RECORD p_event);

private:
    void active_flush_worker_loop();
    void user_trace_consumer_loop();
    void kernel_trace_consumer_loop();
    static void cleanup_stale_sessions();

    FlightRecorder& flight_recorder_;
    TriggerEngine& trigger_engine_;
    const EtwSessionConfig config_;
    const uint64_t qpc_freq_;

    std::atomic<bool> running_{false};
    TRACEHANDLE user_session_handle_{0};
    TRACEHANDLE kernel_session_handle_{0};
    TRACEHANDLE user_trace_handle_{INVALID_PROCESSTRACE_HANDLE};
    TRACEHANDLE kernel_trace_handle_{INVALID_PROCESSTRACE_HANDLE};

    std::thread flush_worker_thread_;
    std::thread user_consumer_thread_;
    std::thread kernel_consumer_thread_;

    // In-flight Disk I/O tracking table
    struct DiskInFlight {
        uint64_t start_qpc{0};
        uint32_t pid{0};
        uint32_t tid{0};
        uint32_t size{0};
        bool is_write{false};
    };
    std::unordered_map<uint64_t, DiskInFlight> in_flight_disk_;
    std::mutex disk_mutex_;

    // In-flight DXGI Present tracking table: key = (TID << 32) | SwapchainHash
    struct PresentInFlight {
        uint64_t start_qpc{0};
        uint32_t pid{0};
        uint32_t tid{0};
    };
    std::unordered_map<uint64_t, PresentInFlight> in_flight_present_;
    std::mutex present_mutex_;
};

} // namespace stuttometer
