#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>
#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "etw_kernel_opcodes.hpp"
#include <tdh.h>

namespace stuttometer {

static std::wstring get_user_session_name() {
    return L"StuttometerUserSession_" + std::to_wstring(GetCurrentProcessId());
}
static constexpr const wchar_t* KERNEL_SESSION_NAME = KERNEL_LOGGER_NAMEW;

// Named ETW Provider Keywords
constexpr uint64_t ETW_KEYWORD_DXGI_DEFAULT             = 0x0000000000000003ULL;
constexpr uint64_t ETW_KEYWORD_AUDIO_GLITCH             = 0x4000000000000000ULL;
constexpr uint64_t ETW_KEYWORD_DXGKRNL_DEFAULT          = 0x00000000000004A7ULL | 0x0000000008000000ULL; // 0x080004A7: DriverEvents | Memory | Cdd | References | Profiler | Base | Present (bit 27)
constexpr uint64_t ETW_KEYWORD_DWM_CORE_GLITCH          = 0x0000000000000001ULL;
constexpr uint64_t ETW_KEYWORD_KERNEL_PROCESSOR_POWER   = 0x0000000000000085ULL;
constexpr uint64_t ETW_KEYWORD_ANTIMALWARE_ALL          = 0x00000000FFFFFFFFULL;
constexpr uint64_t ETW_KEYWORD_D3D12_DEFAULT            = 0x0000000000000C80ULL; // ObjectLifetime (0x80) | APIs (0x400) | SODB (0x800)
constexpr uint64_t ETW_KEYWORD_KERNEL_MEMORY_DEFAULT    = 0x0000000000000280ULL; // WS_SWAP (0x80) | PHYSICAL_ALLOC (0x200)
constexpr uint64_t ETW_KEYWORD_KERNEL_PROCESS_DEFAULT   = 0x0000000000000010ULL; // WINEVENT_KEYWORD_PROCESS (0x10)

EtwSessionManager::EtwSessionManager(
    FlightRecorder& flight_recorder,
    TriggerEngine& trigger_engine,
    const EtwSessionConfig& config
)
    : flight_recorder_(flight_recorder)
    , trigger_engine_(trigger_engine)
    , config_(config)
    , qpc_freq_(get_qpc_frequency())
{
    for (size_t i = 0; i < 16; ++i) {
        recent_dwm_glitches_qpc_[i].store(0, std::memory_order_relaxed);
    }
}

EtwSessionManager::~EtwSessionManager() {
    stop();
}

SessionStartResult EtwSessionManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return SessionStartResult::SUCCESS;
    }

    if (!is_supported_windows_build()) {
        std::cerr << "[ETW] Warning: Windows build is older than 19041 or unrecognized. Kernel MOF parsing may be degraded.\n";
    }

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    sync_time_utc_.store(uli.QuadPart, std::memory_order_relaxed);
    sync_time_qpc_.store(get_current_qpc(), std::memory_order_relaxed);
    cached_dwm_pid_.store(resolve_process_name_to_pid("dwm.exe"), std::memory_order_relaxed);

    const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    bool user_started = false;
    bool kernel_started = false;
    const std::wstring user_session_name = get_user_session_name();

    // 1. Configure User-Mode Trace Session (DXGI, Audio, DxgKrnl, DWM-Core, Power, Antimalware)
    const bool user_requested = config_.enable_dxgi || config_.enable_audio || 
                                config_.enable_dxgkrnl || config_.enable_dwm_core ||
                                config_.enable_processor_power || config_.enable_antimalware ||
                                config_.enable_d3d12 || config_.enable_kernel_memory;
    if (user_requested) {
        std::vector<uint8_t> user_props_buf(prop_size, 0);
        auto p_user_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(user_props_buf.data());
        p_user_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_user_props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p_user_props->Wnode.ClientContext = 1; // QPC Clock
        p_user_props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p_user_props->FlushTimer = 1;
        p_user_props->BufferSize = 128;
        p_user_props->MinimumBuffers = 16;
        p_user_props->MaximumBuffers = 64;
        p_user_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ControlTraceW(0, user_session_name.c_str(), p_user_props, EVENT_TRACE_CONTROL_STOP);

        // Re-initialize buffer cleanly before StartTraceW
        std::fill(user_props_buf.begin(), user_props_buf.end(), uint8_t{0});
        p_user_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_user_props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p_user_props->Wnode.ClientContext = 1;
        p_user_props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p_user_props->FlushTimer = 1;
        p_user_props->BufferSize = 128;
        p_user_props->MinimumBuffers = 16;
        p_user_props->MaximumBuffers = 64;
        p_user_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        TRACEHANDLE local_user_handle = 0;
        ULONG status = StartTraceW(&local_user_handle, user_session_name.data(), p_user_props);
        if (status != ERROR_SUCCESS) {
            std::cerr << "[ETW] Warning: Failed to start User Trace Session (Error " << status << ")\n";
            user_session_handle_.store(0, std::memory_order_release);
        } else {
            bool any_user_provider_enabled = false;
            if (config_.enable_dxgi) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &DXGI_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_VERBOSE, ETW_KEYWORD_DXGI_DEFAULT, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable DXGI provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_audio) {
                // Enable audio glitch channel
                ULONG en_status = EnableTraceEx2(local_user_handle, &AUDIO_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_AUDIO_GLITCH, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Audio provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_dxgkrnl) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &DXGKRNL_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_DXGKRNL_DEFAULT, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable DxgKrnl provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_dwm_core) {
                // Strict keyword masking: DWM Schedule/Glitch only with TRACE_LEVEL_INFORMATION
                ULONG en_status = EnableTraceEx2(local_user_handle, &DWM_CORE_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_DWM_CORE_GLITCH, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable DWM-Core provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_processor_power) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &KERNEL_PROCESSOR_POWER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_KERNEL_PROCESSOR_POWER, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Kernel-Processor-Power provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_antimalware) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &ANTIMALWARE_ENGINE_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_ANTIMALWARE_ALL, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Antimalware-Engine provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_d3d12) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &DIRECT3D12_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_D3D12_DEFAULT, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Direct3D12 provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_kernel_memory) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &KERNEL_MEMORY_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_KERNEL_MEMORY_DEFAULT, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Microsoft-Windows-Kernel-Memory provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }
            if (config_.enable_kernel_process_events) {
                ULONG en_status = EnableTraceEx2(local_user_handle, &KERNEL_PROCESS_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                                 TRACE_LEVEL_INFORMATION, ETW_KEYWORD_KERNEL_PROCESS_DEFAULT, 0, 0, nullptr);
                if (en_status != ERROR_SUCCESS) {
                    std::cerr << "[ETW] Warning: Failed to enable Microsoft-Windows-Kernel-Process provider (Error " << en_status << ")\n";
                } else {
                    any_user_provider_enabled = true;
                }
            }

            if (any_user_provider_enabled) {
                user_session_handle_.store(local_user_handle, std::memory_order_release);
                user_started = true;
            } else {
                std::cerr << "[ETW] Warning: User session started but no requested providers could be enabled.\n";
                ControlTraceW(local_user_handle, nullptr, p_user_props, EVENT_TRACE_CONTROL_STOP);
                user_session_handle_.store(0, std::memory_order_release);
                user_started = false;
            }
        }
    }

    // 2. Configure Kernel Trace Session (DPC, ISR, Disk, CSwitch, PageFault, VirtualAlloc, Profile)
    const bool kernel_requested = config_.enable_kernel_dpc || config_.enable_kernel_disk || 
                                  config_.enable_kernel_cswitch || config_.enable_kernel_profile ||
                                  config_.enable_kernel_pagefault;
    if (kernel_requested) {
        std::vector<uint8_t> kernel_props_buf(prop_size, 0);
        auto p_kernel_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(kernel_props_buf.data());
        p_kernel_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_kernel_props->Wnode.Guid = SYSTEM_TRACE_CONTROL_GUID;
        p_kernel_props->Wnode.ClientContext = 1; // QPC Clock
        p_kernel_props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p_kernel_props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p_kernel_props->FlushTimer = 1;
        p_kernel_props->BufferSize = 128;
        p_kernel_props->MinimumBuffers = 16;
        p_kernel_props->MaximumBuffers = 64;
        p_kernel_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ULONG flags = 0;
        if (config_.enable_kernel_dpc) {
            flags |= (EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT);
        }
        if (config_.enable_kernel_disk) {
            flags |= (EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_IO_INIT);
        }
        if (config_.enable_kernel_cswitch) {
            flags |= EVENT_TRACE_FLAG_CSWITCH;
        }
        if (config_.enable_kernel_profile) {
            flags |= EVENT_TRACE_FLAG_PROFILE;
        }
        if (config_.enable_kernel_pagefault) {
            flags |= (EVENT_TRACE_FLAG_MEMORY_HARD_FAULTS | EVENT_TRACE_FLAG_VIRTUAL_ALLOC);
        }
        p_kernel_props->EnableFlags = flags;

        // Query existing NT Kernel Logger status using an isolated buffer so p_kernel_props is preserved
        std::vector<uint8_t> query_buf(prop_size, 0);
        auto p_query_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(query_buf.data());
        p_query_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_query_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ULONG query_status = ControlTraceW(0, KERNEL_SESSION_NAME, p_query_props, EVENT_TRACE_CONTROL_QUERY);
        if (query_status == ERROR_SUCCESS) {
            std::cerr << "[ETW] Notice: NT Kernel Logger is currently active on the system.\n";
        }

        TRACEHANDLE local_kernel_handle = 0;
        ULONG status = StartTraceW(&local_kernel_handle, KERNEL_SESSION_NAME, p_kernel_props);
        if (status == ERROR_ALREADY_EXISTS) {
            std::cerr << "[ETW] Warning: NT Kernel Logger session already owned by another tool (e.g. WPA, xperf, or Antivirus). Please close active performance recorders to enable kernel-level DPC/ISR/CSwitch tracking.\n";
            kernel_session_handle_.store(0, std::memory_order_release);
        } else if (status != ERROR_SUCCESS) {
            std::cerr << "[ETW] Warning: Failed to start Kernel Trace Session (Error " << status << ").\n";
            kernel_session_handle_.store(0, std::memory_order_release);
        } else {
            kernel_session_handle_.store(local_kernel_handle, std::memory_order_release);
            kernel_started = true;
        }
    }

    if (!user_started && !kernel_started) {
        return SessionStartResult::FAILED;
    }

    running_.store(true, std::memory_order_release);

    flush_worker_thread_ = std::thread(&EtwSessionManager::active_flush_worker_loop, this);
    SetThreadPriority(flush_worker_thread_.native_handle(), THREAD_PRIORITY_NORMAL);

    if (user_session_handle_.load(std::memory_order_acquire)) {
        user_consumer_thread_ = std::thread(&EtwSessionManager::user_trace_consumer_loop, this);
        SetThreadPriority(user_consumer_thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
    if (kernel_session_handle_.load(std::memory_order_acquire)) {
        kernel_consumer_thread_ = std::thread(&EtwSessionManager::kernel_trace_consumer_loop, this);
        SetThreadPriority(kernel_consumer_thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }

    if (user_requested && !user_started) {
        return SessionStartResult::DEGRADED_KERNEL_ONLY;
    }
    if (kernel_requested && !kernel_started) {
        return SessionStartResult::DEGRADED_USER_ONLY;
    }

    return SessionStartResult::SUCCESS;
}

void EtwSessionManager::stop_user_session() noexcept {
    TRACEHANDLE u_trace = user_trace_handle_.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
    if (u_trace != 0 && u_trace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(u_trace);
    }

    TRACEHANDLE user_sess = user_session_handle_.exchange(0, std::memory_order_acq_rel);
    if (user_sess != 0 && user_sess != INVALID_PROCESSTRACE_HANDLE) {
        constexpr size_t PROP_SIZE = sizeof(EVENT_TRACE_PROPERTIES) + (2 * 512 * sizeof(wchar_t));
        alignas(EVENT_TRACE_PROPERTIES) uint8_t props_buf[PROP_SIZE]{};
        auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(props_buf);
        p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
        p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(user_sess, nullptr, p_props, EVENT_TRACE_CONTROL_STOP);
    }
}

void EtwSessionManager::stop_kernel_session() noexcept {
    TRACEHANDLE k_trace = kernel_trace_handle_.exchange(INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel);
    if (k_trace != 0 && k_trace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(k_trace);
    }

    TRACEHANDLE kernel_sess = kernel_session_handle_.exchange(0, std::memory_order_acq_rel);
    if (kernel_sess != 0 && kernel_sess != INVALID_PROCESSTRACE_HANDLE) {
        constexpr size_t PROP_SIZE = sizeof(EVENT_TRACE_PROPERTIES) + (2 * 512 * sizeof(wchar_t));
        alignas(EVENT_TRACE_PROPERTIES) uint8_t props_buf[PROP_SIZE]{};
        auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(props_buf);
        p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
        p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(kernel_sess, nullptr, p_props, EVENT_TRACE_CONTROL_STOP);
    }
}

void EtwSessionManager::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    // 1. Join flush worker thread FIRST (guarantees no concurrent flushes remain in-flight while stopping sessions)
    try {
        if (flush_worker_thread_.joinable()) {
            flush_worker_thread_.join();
        }
    } catch (...) {}

    // 2. ABORT Consumers by closing trace handles (Unblocks ProcessTrace instantly) and stopping sessions
    stop_user_session();
    stop_kernel_session();

    // 3. Join consumer worker threads safely (exits in <1ms)
    try {
        if (user_consumer_thread_.joinable()) {
            user_consumer_thread_.join();
        }
    } catch (...) {}
    try {
        if (kernel_consumer_thread_.joinable()) {
            kernel_consumer_thread_.join();
        }
    } catch (...) {}
}

void EtwSessionManager::flush_buffers() noexcept {
    if (!running_.load(std::memory_order_acquire)) return;

    constexpr size_t PROP_SIZE = sizeof(EVENT_TRACE_PROPERTIES) + (2 * 512 * sizeof(wchar_t));
    alignas(EVENT_TRACE_PROPERTIES) uint8_t props_buf[PROP_SIZE]{};
    auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(props_buf);

    TRACEHANDLE user_sess = user_session_handle_.load(std::memory_order_acquire);
    if (user_sess != 0 && user_sess != INVALID_PROCESSTRACE_HANDLE) {
        p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
        p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        if (ControlTraceW(user_sess, nullptr, p_props, EVENT_TRACE_CONTROL_FLUSH) == ERROR_SUCCESS) {
            user_events_lost_.store(p_props->EventsLost, std::memory_order_relaxed);
            user_buffers_lost_.store(p_props->LogBuffersLost, std::memory_order_relaxed);
        }
    }
    TRACEHANDLE kernel_sess = kernel_session_handle_.load(std::memory_order_acquire);
    if (kernel_sess != 0 && kernel_sess != INVALID_PROCESSTRACE_HANDLE) {
        std::memset(props_buf, 0, PROP_SIZE);
        p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
        p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        if (ControlTraceW(kernel_sess, nullptr, p_props, EVENT_TRACE_CONTROL_FLUSH) == ERROR_SUCCESS) {
            kernel_events_lost_.store(p_props->EventsLost, std::memory_order_relaxed);
            kernel_buffers_lost_.store(p_props->LogBuffersLost, std::memory_order_relaxed);
        }
    }
}

void EtwSessionManager::active_flush_worker_loop() {
    constexpr size_t PROP_SIZE = sizeof(EVENT_TRACE_PROPERTIES) + (2 * 512 * sizeof(wchar_t));
    alignas(EVENT_TRACE_PROPERTIES) uint8_t props_buf[PROP_SIZE]{};
    auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(props_buf);

    const auto interval = std::chrono::milliseconds(config_.flush_interval_ms);
    const uint64_t present_max_age_qpc = ms_to_qpc_delta(5000.0, qpc_freq_);
    const uint64_t disk_max_age_qpc = ms_to_qpc_delta(3000.0, qpc_freq_);
    const uint64_t scan_max_age_qpc = ms_to_qpc_delta(12000.0, qpc_freq_);
    const uint64_t thread_max_age_qpc = ms_to_qpc_delta(5000.0, qpc_freq_);
    const uint64_t tid_pid_max_age_qpc = ms_to_qpc_delta(15000.0, qpc_freq_);
    const uint64_t last_present_max_age_qpc = ms_to_qpc_delta(30000.0, qpc_freq_);
    const uint64_t pso_max_age_qpc = ms_to_qpc_delta(12000.0, qpc_freq_);
    const uint64_t ws_trim_max_age_qpc = ms_to_qpc_delta(12000.0, qpc_freq_);
    uint64_t loop_counter = 0;
    uint64_t last_resync_qpc = sync_time_qpc_.load(std::memory_order_relaxed);

    while (running_.load(std::memory_order_relaxed)) {
        TRACEHANDLE user_sess = user_session_handle_.load(std::memory_order_acquire);
        TRACEHANDLE kernel_sess = kernel_session_handle_.load(std::memory_order_acquire);

        // Active buffer flush to force near-zero kernel buffer buffering latency
        if (!user_consumer_failed_.load(std::memory_order_relaxed) && user_sess != 0 && user_sess != INVALID_PROCESSTRACE_HANDLE) {
            std::memset(props_buf, 0, PROP_SIZE);
            p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
            p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            if (ControlTraceW(user_sess, nullptr, p_props, EVENT_TRACE_CONTROL_FLUSH) == ERROR_SUCCESS) {
                user_events_lost_.store(p_props->EventsLost, std::memory_order_relaxed);
                user_buffers_lost_.store(p_props->LogBuffersLost, std::memory_order_relaxed);
            }
        }
        if (!kernel_consumer_failed_.load(std::memory_order_relaxed) && kernel_sess != 0 && kernel_sess != INVALID_PROCESSTRACE_HANDLE) {
            std::memset(props_buf, 0, PROP_SIZE);
            p_props->Wnode.BufferSize = static_cast<ULONG>(PROP_SIZE);
            p_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            if (ControlTraceW(kernel_sess, nullptr, p_props, EVENT_TRACE_CONTROL_FLUSH) == ERROR_SUCCESS) {
                kernel_events_lost_.store(p_props->EventsLost, std::memory_order_relaxed);
                kernel_buffers_lost_.store(p_props->LogBuffersLost, std::memory_order_relaxed);
            }
        }

        // Periodic background-driven table eviction every ~1.6s (16 * config_.flush_interval_ms, zero hot-path latency)
        if (++loop_counter % 16 == 0) {
            const uint64_t current_qpc = get_current_qpc();
            in_flight_present_.evict_stale(current_qpc, present_max_age_qpc, [](const PresentInFlight& p) { return p.start_qpc; });
            in_flight_disk_.evict_stale(current_qpc, disk_max_age_qpc, [](const DiskInFlight& d) { return d.start_qpc; });
            in_flight_scans_.evict_stale(current_qpc, scan_max_age_qpc, [](const AntimalwareScanInFlight& s) { return s.start_qpc; });
            in_flight_pso_table_.evict_stale(current_qpc, pso_max_age_qpc, [](const PsoInFlight& p) { return p.start_qpc; });
            in_flight_ws_trims_.evict_stale(current_qpc, ws_trim_max_age_qpc, [](const WorkingSetTrimInFlight& w) { return w.start_qpc; });
            in_flight_threads_.evict_stale(current_qpc, thread_max_age_qpc, [](const ThreadSwitchOut& t) { return t.qpc; });
            tid_to_pid_.evict_stale(current_qpc, tid_pid_max_age_qpc, [](const TidPidEntry& e) { return e.last_seen_qpc; });
            last_present_table_.evict_stale(current_qpc, last_present_max_age_qpc, [](const LastPresentEntry& e) { return e.last_present_qpc; });
            last_flip_table_.evict_stale(current_qpc, last_present_max_age_qpc, [](const LastFlipEntry& e) { return e.last_flip_qpc; });
            trigger_engine_.evict_stale_pacing_entries(current_qpc, last_present_max_age_qpc);
        }

        // Periodic wall-clock clock resynchronization (~3 seconds regardless of flush interval)
        const uint64_t current_qpc = get_current_qpc();
        if ((current_qpc - last_resync_qpc) >= (3 * qpc_freq_)) {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER uli{};
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            const uint64_t new_utc = uli.QuadPart;
            const uint64_t new_qpc = get_current_qpc();

            const uint64_t s = sync_time_seq_.load(std::memory_order_relaxed);
            sync_time_seq_.store(s + 1, std::memory_order_release); // odd = write in progress
            sync_time_utc_.store(new_utc, std::memory_order_relaxed);
            sync_time_qpc_.store(new_qpc, std::memory_order_relaxed);
            sync_time_seq_.store(s + 2, std::memory_order_release); // even = published
            last_resync_qpc = current_qpc;
        }

        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }

        std::this_thread::sleep_for(interval);
    }
}

void EtwSessionManager::user_trace_consumer_loop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    std::wstring user_session_name = get_user_session_name();
    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = user_session_name.data();
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log_file.EventRecordCallback = &EtwSessionManager::on_event_record;
    log_file.Context = this;

    TRACEHANDLE h = OpenTraceW(&log_file);
    if (h == INVALID_PROCESSTRACE_HANDLE) {
        std::cerr << "[ETW] Error: OpenTraceW failed for User Session (Error " << GetLastError() << ")\n";
        user_consumer_failed_.store(true, std::memory_order_release);
        return;
    }
    user_trace_handle_.store(h, std::memory_order_release);

    if (!running_.load(std::memory_order_acquire) || user_trace_handle_.load(std::memory_order_acquire) != h) {
        TRACEHANDLE expected = h;
        if (user_trace_handle_.compare_exchange_strong(expected, INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel)) {
            CloseTrace(h);
        }
        return;
    }

    ULONG status = ProcessTrace(&h, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS && running_.load(std::memory_order_relaxed)) {
        std::cerr << "[ETW] Error: ProcessTrace for User Session failed with code " << status << "\n";
        user_consumer_failed_.store(true, std::memory_order_release);
    }
}

void EtwSessionManager::kernel_trace_consumer_loop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = const_cast<LPWSTR>(KERNEL_SESSION_NAME);
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log_file.EventRecordCallback = &EtwSessionManager::on_event_record;
    log_file.Context = this;

    TRACEHANDLE h = OpenTraceW(&log_file);
    if (h == INVALID_PROCESSTRACE_HANDLE) {
        std::cerr << "[ETW] Error: OpenTraceW failed for Kernel Session (Error " << GetLastError() << ")\n";
        kernel_consumer_failed_.store(true, std::memory_order_release);
        return;
    }
    kernel_trace_handle_.store(h, std::memory_order_release);

    if (!running_.load(std::memory_order_acquire) || kernel_trace_handle_.load(std::memory_order_acquire) != h) {
        TRACEHANDLE expected = h;
        if (kernel_trace_handle_.compare_exchange_strong(expected, INVALID_PROCESSTRACE_HANDLE, std::memory_order_acq_rel)) {
            CloseTrace(h);
        }
        return;
    }

    ULONG status = ProcessTrace(&h, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS && running_.load(std::memory_order_relaxed)) {
        std::cerr << "[ETW] Error: ProcessTrace for Kernel Session failed with code " << status << "\n";
        kernel_consumer_failed_.store(true, std::memory_order_release);
    }
}

void WINAPI EtwSessionManager::on_event_record(PEVENT_RECORD p_event) {
    if (!p_event) return;
    auto* mgr = reinterpret_cast<EtwSessionManager*>(p_event->UserContext);
    if (!mgr || !mgr->running_.load(std::memory_order_acquire)) return;

    const uint64_t timestamp = static_cast<uint64_t>(p_event->EventHeader.TimeStamp.QuadPart);
    const uint32_t pid = p_event->EventHeader.ProcessId;
    const uint32_t tid = p_event->EventHeader.ThreadId;
    const uint8_t cpu = static_cast<uint8_t>(p_event->BufferContext.ProcessorNumber);
    const uint16_t event_id = p_event->EventHeader.EventDescriptor.Id;
    const uint8_t opcode = p_event->EventHeader.EventDescriptor.Opcode;

    EventContext ctx{ timestamp, pid, tid, cpu, event_id, opcode };

    EtwEventRecord rec{};
    rec.qpc_timestamp = timestamp;
    rec.pid = pid;
    rec.tid = tid;
    rec.cpu_index = cpu;
    rec.event_id = event_id;

    const GUID& prov_guid = p_event->EventHeader.ProviderId;
    if (IsEqualGUID(prov_guid, DXGI_PROVIDER_GUID)) {
        mgr->handle_dxgi_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, AUDIO_PROVIDER_GUID)) {
        mgr->handle_audio_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, DXGKRNL_PROVIDER_GUID)) {
        const uint16_t task = p_event->EventHeader.EventDescriptor.Task;
        if (task == 17) { // Task 17 = MMIOFlip (Event 116) - WDDM authoritative flip
            mgr->handle_dxgkrnl_flip_event(p_event, rec, ctx);
        } else if (task == 4 && (opcode == 17 || opcode == 1)) {
            mgr->handle_dxgkrnl_vsync_event(p_event, rec, ctx);
        } else if (event_id == 370 || task == 222 || event_id == 367 || task == 219) {
            mgr->handle_dxgkrnl_vidmm_event(p_event, rec, ctx);
        } else if (event_id == 510 || task == 33) {
            mgr->handle_dxgkrnl_paging_event(p_event, rec, ctx);
        }
    } else if (IsEqualGUID(prov_guid, DWM_CORE_PROVIDER_GUID)) {
        if (p_event->EventHeader.EventDescriptor.Task == 132 || event_id == 15 || event_id == 16) {
            mgr->handle_dwm_event(p_event, rec, ctx);
        }
    } else if (IsEqualGUID(prov_guid, ANTIMALWARE_ENGINE_GUID)) {
        mgr->handle_antimalware_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, KERNEL_PROCESSOR_POWER_GUID)) {
        mgr->handle_power_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, DIRECT3D12_PROVIDER_GUID)) {
        mgr->handle_d3d12_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, KERNEL_MEMORY_PROVIDER_GUID)) {
        mgr->handle_kernel_memory_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, KERNEL_PROCESS_PROVIDER_GUID)) {
        mgr->handle_process_event(p_event, rec, ctx);
    } else if (IsEqualGUID(prov_guid, SYSTEM_TRACE_CONTROL_GUID) ||
               IsEqualGUID(prov_guid, PERFINFO_GUID) ||
               IsEqualGUID(prov_guid, THREAD_GUID) ||
               IsEqualGUID(prov_guid, DISK_IO_GUID) ||
               IsEqualGUID(prov_guid, PAGE_FAULT_GUID)) {
        const bool is_perfinfo  = IsEqualGUID(prov_guid, PERFINFO_GUID) || IsEqualGUID(prov_guid, SYSTEM_TRACE_CONTROL_GUID);
        const bool is_thread    = IsEqualGUID(prov_guid, THREAD_GUID) || IsEqualGUID(prov_guid, SYSTEM_TRACE_CONTROL_GUID);
        const bool is_disk      = IsEqualGUID(prov_guid, DISK_IO_GUID) || IsEqualGUID(prov_guid, SYSTEM_TRACE_CONTROL_GUID);
        const bool is_pagefault = IsEqualGUID(prov_guid, PAGE_FAULT_GUID) || IsEqualGUID(prov_guid, SYSTEM_TRACE_CONTROL_GUID);

        if (is_perfinfo && (opcode == KERNEL_OPCODE_DPC_CLASSIC || event_id == KERNEL_OPCODE_DPC_CLASSIC || 
                            opcode == KERNEL_OPCODE_DPC || event_id == KERNEL_OPCODE_DPC || 
                            opcode == KERNEL_OPCODE_TIMER || event_id == KERNEL_OPCODE_TIMER || 
                            (p_event->EventHeader.EventDescriptor.Task == 1 && opcode == 2) ||
                            opcode == KERNEL_OPCODE_ISR_CLASSIC || event_id == KERNEL_OPCODE_ISR_CLASSIC || 
                            (p_event->EventHeader.EventDescriptor.Task == 2 && opcode == 2) ||
                            opcode == 46 || (p_event->EventHeader.EventDescriptor.Task == 7 && opcode == 2))) {
            mgr->handle_nt_dpc_isr_event(p_event, rec, ctx);
        } else if (is_thread && (event_id == KERNEL_OPCODE_CSWITCH || opcode == KERNEL_OPCODE_CSWITCH)) {
            mgr->handle_nt_cswitch_event(p_event, rec, ctx);
        } else if (is_disk && (opcode >= KERNEL_OPCODE_DISK_READ && opcode <= KERNEL_OPCODE_DISK_WRITE_INIT)) {
            mgr->handle_nt_disk_event(p_event, rec, ctx);
        } else if (is_pagefault && (opcode == KERNEL_OPCODE_HARDFAULT || opcode == KERNEL_OPCODE_VIRTUAL_ALLOC)) {
            mgr->handle_nt_fault_event(p_event, rec, ctx);
        }
    }

    // Monotonically advance highest processed QPC timestamp for deterministic post-trigger draining
    // Published at the very end of on_event_record to guarantee all delivered events with timestamp <= to_qpc are published
    uint64_t cur_qpc = mgr->last_processed_qpc_.load(std::memory_order_relaxed);
    while (timestamp > cur_qpc &&
           !mgr->last_processed_qpc_.compare_exchange_weak(
               cur_qpc, timestamp, std::memory_order_release, std::memory_order_relaxed)) {}
}

bool run_environment_self_check(std::ostream& out) {
    bool has_critical_failure = false;

    out << "================================================================\n";
    out << " STUTTOMETER ENVIRONMENT & ETW PROVIDER SELF-CHECK\n";
    out << "================================================================\n";

    // 1. Administrator Privileges
    const bool is_admin = is_running_as_admin();
    if (is_admin) {
        out << "[PASS] Administrator Privileges: Elevated (Full kernel ETW capabilities)\n";
    } else {
        out << "[WARN] Administrator Privileges: Standard user (Kernel ETW requires elevation)\n";
    }

    // 2. QPC Clock Resolution
    LARGE_INTEGER freq{};
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        const double freq_mhz = static_cast<double>(freq.QuadPart) / 1000000.0;
        const double tick_ns = 1000000000.0 / static_cast<double>(freq.QuadPart);
        char buf[128];
        snprintf(buf, sizeof(buf), "[PASS] QPC Clock Resolution: %.2f MHz (%.1f ns tick granularity)", freq_mhz, tick_ns);
        out << buf << "\n";
    } else {
        out << "[FAIL] QPC Clock Resolution: Failed to query frequency\n";
        has_critical_failure = true;
    }

    // 3. NT Kernel Logger Status
    if (is_admin) {
        const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
        std::vector<uint8_t> query_buf(prop_size, 0);
        auto p_query_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(query_buf.data());
        p_query_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_query_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ULONG query_status = ControlTraceW(0, KERNEL_LOGGER_NAMEW, p_query_props, EVENT_TRACE_CONTROL_QUERY);
        if (query_status == ERROR_WMI_INSTANCE_NOT_FOUND) {
            out << "[PASS] NT Kernel Logger: Available\n";
        } else if (query_status == ERROR_SUCCESS) {
            out << "[WARN] NT Kernel Logger: Currently in use by another session (WPA/xperf/PresentMon)\n";
        } else {
            out << "[WARN] NT Kernel Logger: Status query returned code " << query_status << "\n";
        }
    } else {
        out << "[INFO] NT Kernel Logger: Requires Administrator privileges to query status\n";
    }

    // 4. ETW Manifest Providers (TDH)
    ULONG buffer_size = 0;
    ULONG status = TdhEnumerateProviders(nullptr, &buffer_size);
    std::vector<uint8_t> buffer;
    while (status == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(buffer_size);
        auto* p_info = reinterpret_cast<PPROVIDER_ENUMERATION_INFO>(buffer.data());
        status = TdhEnumerateProviders(p_info, &buffer_size);
    }

    if (status != ERROR_SUCCESS) {
        out << "[WARN] ETW Provider Enumeration: TDH query failed (Error " << status << ")\n";
    } else if (!buffer.empty()) {
        auto* p_info = reinterpret_cast<PPROVIDER_ENUMERATION_INFO>(buffer.data());

        enum class ProviderSeverity {
            TIER_REQUIRED,
            TIER_STANDARD,
            TIER_OPTIONAL
        };

        struct ProviderCheck {
            const GUID* guid;
            const char* name;
            ProviderSeverity severity;
        };

        const ProviderCheck checks[] = {
            { &DXGI_PROVIDER_GUID, "Microsoft-Windows-DXGI", ProviderSeverity::TIER_REQUIRED },
            { &DXGKRNL_PROVIDER_GUID, "Microsoft-Windows-DxgKrnl", ProviderSeverity::TIER_REQUIRED },
            { &DWM_CORE_PROVIDER_GUID, "Microsoft-Windows-Dwm-Core", ProviderSeverity::TIER_REQUIRED },
            { &AUDIO_PROVIDER_GUID, "Microsoft-Windows-Audio", ProviderSeverity::TIER_STANDARD },
            { &DIRECT3D12_PROVIDER_GUID, "Microsoft-Windows-Direct3D12", ProviderSeverity::TIER_STANDARD },
            { &KERNEL_MEMORY_PROVIDER_GUID, "Microsoft-Windows-Kernel-Memory", ProviderSeverity::TIER_STANDARD },
            { &KERNEL_PROCESS_PROVIDER_GUID, "Microsoft-Windows-Kernel-Process", ProviderSeverity::TIER_STANDARD },
            { &KERNEL_PROCESSOR_POWER_GUID, "Microsoft-Windows-Kernel-Processor-Power", ProviderSeverity::TIER_STANDARD },
            { &ANTIMALWARE_ENGINE_GUID, "Microsoft-Antimalware-Engine", ProviderSeverity::TIER_OPTIONAL },
        };

        constexpr size_t num_checks = sizeof(checks) / sizeof(checks[0]);
        bool found[num_checks] = { false };

        for (ULONG i = 0; i < p_info->NumberOfProviders; ++i) {
            const GUID& pguid = p_info->TraceProviderInfoArray[i].ProviderGuid;
            for (size_t c = 0; c < num_checks; ++c) {
                if (!found[c] && IsEqualGUID(pguid, *checks[c].guid)) {
                    found[c] = true;
                }
            }
        }

        for (size_t c = 0; c < num_checks; ++c) {
            const auto& item = checks[c];
            if (found[c]) {
                const char* tier_label = (item.severity == ProviderSeverity::TIER_REQUIRED) ? "Required" :
                                         (item.severity == ProviderSeverity::TIER_STANDARD) ? "Standard" : "Optional";
                out << "[PASS] ETW Provider (" << tier_label << "): " << item.name << "\n";
            } else {
                if (item.severity == ProviderSeverity::TIER_REQUIRED) {
                    out << "[FAIL] ETW Provider (Required): " << item.name << " is NOT registered\n";
                    has_critical_failure = true;
                } else if (item.severity == ProviderSeverity::TIER_STANDARD) {
                    out << "[WARN] ETW Provider (Standard): " << item.name << " is NOT registered\n";
                } else {
                    out << "[INFO] ETW Provider (Optional): " << item.name << " is NOT registered\n";
                }
            }
        }
    } else {
        out << "[INFO] ETW Provider Enumeration: No registered ETW providers found on system\n";
    }

    out << "================================================================\n";
    if (has_critical_failure) {
        out << " Self-check completed with CRITICAL FAILURES.\n";
    } else {
        out << " Self-check completed successfully. All critical requirements met.\n";
    }
    out << "================================================================\n";

    return !has_critical_failure;
}

} // namespace stuttometer
