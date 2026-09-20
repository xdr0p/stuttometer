#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <tdh.h>

namespace stuttometer {

static std::wstring get_user_session_name() {
    return L"StuttometerUserSession_" + std::to_wstring(GetCurrentProcessId());
}
static constexpr const wchar_t* KERNEL_SESSION_NAME = KERNEL_LOGGER_NAMEW;

constexpr uint8_t KERNEL_OPCODE_DPC_CLASSIC       = 66;
constexpr uint8_t KERNEL_OPCODE_ISR_CLASSIC       = 67;
constexpr uint8_t KERNEL_OPCODE_DPC               = 68;
constexpr uint8_t KERNEL_OPCODE_TIMER             = 69;
constexpr uint8_t KERNEL_OPCODE_CSWITCH           = 36;
constexpr uint8_t KERNEL_OPCODE_DISK_READ_INIT    = 12;
constexpr uint8_t KERNEL_OPCODE_DISK_WRITE_INIT   = 13;
constexpr uint8_t KERNEL_OPCODE_DISK_READ         = 10;
constexpr uint8_t KERNEL_OPCODE_DISK_WRITE        = 11;
constexpr uint8_t KERNEL_OPCODE_HARDFAULT         = 32;

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


// Converts a 128-bit ActivityId GUID into a non-zero 64-bit key with SplitMix64/Murmur3 finalizer
[[nodiscard]] static inline uint64_t activity_id_to_key(const GUID& guid) noexcept {
    uint64_t low = 0, high = 0;
    std::memcpy(&low, &guid, sizeof(uint64_t));
    std::memcpy(&high, reinterpret_cast<const uint8_t*>(&guid) + sizeof(uint64_t), sizeof(uint64_t));
    uint64_t k = low ^ (high * 0x9E3779B97F4A7C15ULL);
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return (k == 0) ? 1ULL : k;
}

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

void EtwSessionManager::handle_dxgi_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGI);

    uint64_t thread_key = make_thread_key(ctx.pid, ctx.tid);

    if (ctx.event_id == 42 || ctx.event_id == 55) { // Present Start / PresentMultiplaneOverlay Start
        uint64_t swapchain_ptr = 0;
        if (p_event->UserDataLength >= 8 && p_event->UserData) {
            std::memcpy(&swapchain_ptr, p_event->UserData, sizeof(uint64_t));
        }
        rec.auxiliary_data = swapchain_ptr;
        in_flight_present_.insert(thread_key, { ctx.timestamp, swapchain_ptr, ctx.pid, ctx.tid });
    } else if (ctx.event_id == 43 || ctx.event_id == 56) { // Present Stop / PresentMultiplaneOverlay Stop
        rec.auxiliary_data = 0; // Preserve NDJSON schema v1 contract for Present Stop

        PresentInFlight present_data{};
        bool has_in_flight = in_flight_present_.find_and_erase(thread_key, present_data);

        // Always push Event 43 to flight recorder and ndjson writer (N-2)
        // If orphaned (no matching Event 42), fallback skips inter-frame calculation to prevent baseline pollution
        if (has_in_flight && present_data.pid == ctx.pid) {
            uint64_t start_qpc = present_data.start_qpc;
            uint64_t swapchain_ptr = present_data.swapchain_ptr;
            uint64_t swapchain_key = make_swapchain_key(ctx.pid, swapchain_ptr);

            LastPresentEntry last_entry{};
            bool has_prev = last_present_table_.lookup(swapchain_key, last_entry);
            uint64_t prev_qpc = has_prev ? last_entry.last_present_qpc : 0;

            PresentDeltaResult delta_res = calculate_effective_present_duration(
                ctx.timestamp, prev_qpc, start_qpc, qpc_freq_, 10000000ULL
            );

            last_present_table_.insert(swapchain_key, { ctx.timestamp, ctx.pid, ctx.tid });

            uint32_t clamped_dur_us = static_cast<uint32_t>(std::min(delta_res.effective_dur_us, 10000000ULL));
            rec.duration_us = clamped_dur_us;

            flight_recorder_.push(rec);

            if (!delta_res.is_baseline_reset && delta_res.effective_dur_us > 0) {
                double dur_ms = delta_res.effective_dur_us / 1000.0;
                trigger_engine_.on_dxgi_present(ctx.pid, ctx.tid, dur_ms, ctx.timestamp, swapchain_key, ctx.cpu);
            }
        } else {
            // Orphaned Event 43 (session started mid-frame or Event 42 dropped by ETW)
            flight_recorder_.push(rec);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_audio_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 11) { // AudioGlitch Event ID 11
        rec.category = static_cast<uint16_t>(EventCategory::AUDIO);
        rec.flags |= EventFlags::AUDIO_BUFFER_UNDERRUN;
        uint32_t glitch_count = 1;
        int32_t error_code = 0;
        if (p_event->UserDataLength >= sizeof(uint32_t) && p_event->UserData) {
            std::memcpy(&glitch_count, p_event->UserData, sizeof(uint32_t));
            glitch_count = std::clamp(glitch_count, 1U, 1'000'000U);
        }
        if (p_event->UserDataLength >= (sizeof(uint32_t) + sizeof(int32_t)) && p_event->UserData) {
            std::memcpy(&error_code, static_cast<const uint8_t*>(p_event->UserData) + sizeof(uint32_t), sizeof(int32_t));
        }
        rec.payload.audio.glitch_count = glitch_count;
        rec.payload.audio.error_code = error_code;
        rec.auxiliary_data = glitch_count;

        flight_recorder_.push(rec);

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);

        trigger_engine_.on_audio_glitch(ctx.pid, ctx.tid, glitch_count, ctx.timestamp, ctx.cpu);
    }
}

void EtwSessionManager::handle_dxgkrnl_flip_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_MMIOFLIP);

    uint32_t vidpn_source_id = 0;
    uint64_t allocation_ptr = 0;
    uint32_t flip_present_id = 0;

    // Verified offsets for Task 17 (Event 116: MMIOFlip):
    // Offset 8 (4B): VidPnSourceId, Offset 16 (8B): FlipToDriverAllocation, Offset 36 (4B): FlipPresentId
    if (p_event->UserDataLength >= 24 && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&vidpn_source_id, raw + 8, sizeof(uint32_t));
        std::memcpy(&allocation_ptr, raw + 16, sizeof(uint64_t));
        if (p_event->UserDataLength >= 40) {
            std::memcpy(&flip_present_id, raw + 36, sizeof(uint32_t));
        }
    }

    rec.auxiliary_data = allocation_ptr;
    rec.payload.dxgi.present_flags = vidpn_source_id;
    rec.payload.dxgi.frame_index = flip_present_id;

    const uint64_t flip_key = make_flip_key(vidpn_source_id, allocation_ptr);
    LastFlipEntry last_entry{};
    bool has_prev = last_flip_table_.lookup(flip_key, last_entry);
    uint64_t prev_qpc = has_prev ? last_entry.last_flip_qpc : 0;

    double delivery_ms = 0.0;
    bool is_baseline = true;

    if (has_prev && prev_qpc > 0 && ctx.timestamp > prev_qpc) {
        uint64_t delta_qpc = ctx.timestamp - prev_qpc;
        double delta_us = qpc_delta_to_us(delta_qpc, qpc_freq_);
        if (delta_us <= 30000000.0) { // 30s ceiling for loading / Alt-Tab
            delivery_ms = delta_us / 1000.0;
            is_baseline = false;
            rec.duration_us = static_cast<uint32_t>(std::min(delta_us, 10000000.0));
        }
    }

    last_flip_table_.insert(flip_key, { ctx.timestamp, static_cast<uint32_t>(allocation_ptr & 0xFFFFFFFF), ctx.pid, ctx.tid });

    flight_recorder_.push(rec);

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);

    // DWM/System Filtering (S1, N-3):
    // In DWM-composed borderless mode, flips originate from DWM/System (PID 4) and are filtered here;
    // DXGI_PRESENT_STUTTER and DWM_GLITCH handle composed presentation.
    const uint32_t dwm_pid = cached_dwm_pid_.load(std::memory_order_relaxed);
    if (ctx.pid == 4 || (dwm_pid != 0 && ctx.pid == dwm_pid)) {
        return;
    }

    if (!is_baseline && delivery_ms > 0.0) {
        trigger_engine_.on_kernel_frame_stall(ctx.pid, ctx.tid, delivery_ms, ctx.timestamp, flip_key, ctx.cpu);
    }
}

void EtwSessionManager::handle_dxgkrnl_vsync_event(PEVENT_RECORD /*p_event*/, EtwEventRecord& rec, const EventContext& /*ctx*/) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VSYNCDPC);
    flight_recorder_.push(rec);

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_dxgkrnl_vidmm_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VRAM_PAGING);
    const uint16_t task = p_event->EventHeader.EventDescriptor.Task;
    if (ctx.event_id == 370 || task == 222) {
        if (p_event->UserDataLength >= 28 && p_event->UserData) {
            uint64_t commitment = 0;
            uint64_t old_commitment = 0;
            uint32_t process_id = 0;
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            std::memcpy(&commitment, raw + 0, sizeof(uint64_t));
            std::memcpy(&old_commitment, raw + 8, sizeof(uint64_t));
            std::memcpy(&process_id, raw + 24, sizeof(uint32_t));

            if (commitment > 0) {
                rec.pid = (process_id != 0) ? process_id : ctx.pid;
                rec.auxiliary_data = commitment;
                rec.flags = EventFlags::VRAM_DEMOTED_COMMITMENT;
                flight_recorder_.push(rec);
            }
        }
    } else if (ctx.event_id == 367 || task == 219) {
        if (p_event->UserDataLength >= 31 && p_event->UserData) {
            uint64_t new_usage = 0;
            uint32_t process_id = 0;
            uint8_t memory_segment_group = 0;
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            std::memcpy(&new_usage, raw + 0, sizeof(uint64_t));
            std::memcpy(&process_id, raw + 24, sizeof(uint32_t));
            std::memcpy(&memory_segment_group, raw + 30, sizeof(uint8_t));

            if (memory_segment_group == 1 && new_usage > 0) {
                rec.pid = (process_id != 0) ? process_id : ctx.pid;
                rec.auxiliary_data = new_usage;
                rec.flags = EventFlags::VRAM_USAGE_OVER_BUDGET;
                flight_recorder_.push(rec);
            }
        }
    }
    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_dxgkrnl_paging_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& /*ctx*/) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VRAM_PAGING);
    if (p_event->UserDataLength >= 64 && p_event->UserData) {
        uint64_t number_of_pages = 0;
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&number_of_pages, raw + 48, sizeof(uint64_t));

        if (number_of_pages > 0) {
            rec.auxiliary_data = number_of_pages * 4096ULL;
            rec.flags = EventFlags::VRAM_PAGING_TRANSFER;
            flight_recorder_.push(rec);
        }
    }
    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_dwm_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DWM_GLITCH);

    const double vblank_ms = (trigger_engine_.vblank_interval_ms() > 0.0) 
        ? trigger_engine_.vblank_interval_ms() 
        : 16.67;
    uint32_t glitch_type = 0;
    uint32_t missed_vblanks = 0;
    if (p_event->UserDataLength >= 8 && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&glitch_type, raw + 0, sizeof(uint32_t));
        std::memcpy(&missed_vblanks, raw + 4, sizeof(uint32_t));
    }
    rec.auxiliary_data = glitch_type;
    const double dur_ms = (missed_vblanks >= 1) ? (missed_vblanks * vblank_ms) : vblank_ms;
    rec.duration_us = static_cast<uint32_t>(std::clamp(dur_ms * 1000.0, 1000.0, 10000000.0));

    bool is_dedup = false;
    const uint64_t dedup_window_qpc = ms_to_qpc_delta(50.0, qpc_freq_);
    for (size_t i = 0; i < 16; ++i) {
        uint64_t recent_ts = recent_dwm_glitches_qpc_[i].load(std::memory_order_acquire);
        if (recent_ts > 0) {
            const uint64_t delta_qpc = (ctx.timestamp >= recent_ts) ? (ctx.timestamp - recent_ts) : (recent_ts - ctx.timestamp);
            if (delta_qpc < dedup_window_qpc) {
                is_dedup = true;
                break;
            }
        }
    }

    if (is_dedup) {
        rec.flags |= EventFlags::DWM_GLITCH_DEDUPLICATED;
    } else {
        uint32_t slot = (recent_dwm_glitch_idx_.fetch_add(1, std::memory_order_relaxed)) & 15;
        recent_dwm_glitches_qpc_[slot].store(ctx.timestamp, std::memory_order_release);
    }

    flight_recorder_.push(rec);

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);

    if (!is_dedup) {
        trigger_engine_.on_dwm_glitch(ctx.pid, ctx.tid, dur_ms, ctx.timestamp, ctx.cpu);
    }
}

void EtwSessionManager::handle_power_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 37) {
        rec.category = static_cast<uint16_t>(EventCategory::THERMAL_THROTTLE);

        if (p_event->UserDataLength >= 24 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t core_number = 0;
            uint32_t cap_duration_sec = 0;
            std::memcpy(&core_number, raw + 4, sizeof(uint32_t));
            std::memcpy(&cap_duration_sec, raw + 8, sizeof(uint32_t));
            if (core_number <= 1024 && cap_duration_sec <= 86400) {
                rec.cpu_index = static_cast<uint8_t>(std::min(core_number, 255U));
                rec.auxiliary_data = cap_duration_sec;
                rec.duration_us = 0;
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_antimalware_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::ANTIMALWARE_SCAN);

    uint64_t scan_key = 0;
    const auto& act = p_event->EventHeader.ActivityId;
    if (!IsEqualGUID(act, GUID_NULL)) {
        scan_key = activity_id_to_key(act);
    }
    if (scan_key == 0) {
        scan_key = (static_cast<uint64_t>(ctx.pid) << 32) | (ctx.tid != 0 ? ctx.tid : 1);
    }

    if (ctx.opcode == 1) { // win:Start
        in_flight_scans_.insert(scan_key, { ctx.timestamp, ctx.pid, ctx.tid });
    } else if (ctx.opcode == 2) { // win:Stop
        AntimalwareScanInFlight scan_data{};
        if (in_flight_scans_.find_and_erase(scan_key, scan_data)) {
            if (ctx.timestamp >= scan_data.start_qpc) {
                const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - scan_data.start_qpc, qpc_freq_));
                if (delta_us <= 10000000ULL) {
                    rec.duration_us = static_cast<uint32_t>(delta_us);
                }
            }
        }
        if (rec.duration_us > 0) {
            flight_recorder_.push(rec);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_d3d12_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    const auto& desc = p_event->EventHeader.EventDescriptor;
    const uint16_t task = desc.Task;
    const uint8_t op = desc.Opcode;

    // Note: Non-PSO D3D12 events (draw calls, resource bindings, etc.) are intentionally filtered to preserve high signal-to-noise ratio in the categorized NDJSON stream.
    const bool is_pso_task = (task == 29 || task == 66 || task == 67 || ctx.event_id == 63 || ctx.event_id == 64 || ctx.event_id == 155 || ctx.event_id == 156 || ctx.event_id == 157 || ctx.event_id == 158);

    if (is_pso_task) {
        rec.category = static_cast<uint16_t>(EventCategory::D3D12_PSO_CREATE);

        uint64_t pso_ptr = 0;
        if (p_event->UserDataLength >= sizeof(uint64_t) && p_event->UserData) {
            if (p_event->UserDataLength >= 16) {
                std::memcpy(&pso_ptr, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            } else {
                std::memcpy(&pso_ptr, p_event->UserData, sizeof(uint64_t));
            }
        }

        uint64_t pso_key = 0;
        const auto& act = p_event->EventHeader.ActivityId;
        if (!IsEqualGUID(act, GUID_NULL)) {
            pso_key = activity_id_to_key(act);
        }
        if (pso_key == 0 || pso_key == 1ULL) {
            pso_key = make_pso_key(ctx.tid, pso_ptr);
        }

        uint16_t flags = EventFlags::NONE;
        if (task == 29 || ctx.event_id == 63 || ctx.event_id == 64) {
            flags |= EventFlags::D3D12_GRAPHICS_PSO;
        } else if (task == 67 || ctx.event_id == 157 || ctx.event_id == 158) {
            flags |= EventFlags::D3D12_COMPUTE_PSO;
        }

        if (op == 1 || ctx.event_id == 63 || ctx.event_id == 155 || ctx.event_id == 157) { // win:Start
            in_flight_pso_table_.insert(pso_key, { ctx.timestamp, ctx.pid, ctx.tid, pso_ptr, flags });
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
        } else if (op == 2 || ctx.event_id == 64 || ctx.event_id == 156 || ctx.event_id == 158) { // win:Stop
            PsoInFlight pso_data{};
            if (in_flight_pso_table_.find_and_erase(pso_key, pso_data)) {
                if (ctx.timestamp >= pso_data.start_qpc) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - pso_data.start_qpc, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                if (flags == EventFlags::NONE) {
                    flags = pso_data.flags;
                }
                if (pso_ptr == 0) {
                    pso_ptr = pso_data.pso_ptr;
                }
            }
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
            if (rec.duration_us > 0) {
                flight_recorder_.push(rec);
            }
        } else {
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_kernel_memory_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 4) { // WorkingSetOutSwap Start
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);

        if (p_event->UserDataLength >= 4 && p_event->UserData) {
            uint32_t target_proc = 0;
            std::memcpy(&target_proc, p_event->UserData, sizeof(uint32_t));
            if (target_proc != 0 && target_proc != 4) {
                WorkingSetTrimInFlight trim_entry{};
                trim_entry.start_qpc = ctx.timestamp;
                trim_entry.pid = target_proc;
                const uint64_t trim_key = (static_cast<uint64_t>(target_proc) << 32) | (ctx.tid != 0 ? ctx.tid : 1);
                in_flight_ws_trims_.insert(trim_key, trim_entry);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    } else if (ctx.event_id == 5) { // WorkingSetOutSwap Stop
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);

        if (p_event->UserDataLength >= 16 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t target_proc = 0;
            uint64_t pages_processed = 0;
            std::memcpy(&target_proc, raw + 0, sizeof(uint32_t));
            std::memcpy(&pages_processed, raw + 8, sizeof(uint64_t));

            rec.pid = target_proc;
            rec.auxiliary_data = pages_processed * 4096ULL;
            rec.flags = EventFlags::MEM_WS_TRIM_OUTSWAP;

            WorkingSetTrimInFlight start_entry{};
            const uint64_t trim_key = (static_cast<uint64_t>(target_proc) << 32) | (ctx.tid != 0 ? ctx.tid : 1);
            if (target_proc != 0 && target_proc != 4 && in_flight_ws_trims_.find_and_erase(trim_key, start_entry)) {
                if (ctx.timestamp >= start_entry.start_qpc) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - start_entry.start_qpc, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
            }
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    } else if (ctx.event_id == 10 || ctx.event_id == 11) { // MdlAllocation (10) or ContAllocation (11)
        rec.category = static_cast<uint16_t>(EventCategory::MEM_PHYSICAL_ALLOC);

        if (p_event->UserDataLength >= 16 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint64_t dur_us = 0;
            uint64_t total_bytes = 0;
            std::memcpy(&dur_us, raw + 0, sizeof(uint64_t));
            std::memcpy(&total_bytes, raw + 8, sizeof(uint64_t));

            if (dur_us <= 10000000ULL) {
                rec.duration_us = static_cast<uint32_t>(dur_us);
            }
            rec.auxiliary_data = total_bytes;
            rec.flags = EventFlags::MEM_PHYSICAL_CONTIGUOUS;
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_process_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::PROCESS);

    if (ctx.event_id == 1 && p_event->UserDataLength >= 4 && p_event->UserData) {
        uint32_t target_pid = 0;
        std::memcpy(&target_pid, p_event->UserData, sizeof(uint32_t));
        if (target_pid != 0) {
            rec.pid = target_pid;
            std::string proc_name;
            if (p_event->UserDataLength >= 26) {
                const auto* raw_bytes = static_cast<const uint8_t*>(p_event->UserData) + 24;
                const auto* p_ws = reinterpret_cast<const wchar_t*>(raw_bytes);
                const size_t max_wchars = (p_event->UserDataLength - 24) / sizeof(wchar_t);
                size_t wlen = 0;
                bool null_terminated = false;
                bool valid_chars = true;
                while (wlen < max_wchars && wlen < MAX_PATH) {
                    wchar_t wc = p_ws[wlen];
                    if (wc == L'\0') {
                        null_terminated = true;
                        break;
                    }
                    if ((wc < 0x20 && wc != L'\t') || wc == 0x7F) {
                        valid_chars = false;
                        break;
                    }
                    ++wlen;
                }
                if (null_terminated && valid_chars && wlen > 0) {
                    std::string full_path = utf16_to_utf8(std::wstring_view(p_ws, wlen));
                    const size_t slash = full_path.find_last_of("\\/");
                    proc_name = (slash != std::string::npos) ? full_path.substr(slash + 1) : full_path;
                }
            }
            if (proc_name.empty()) {
                proc_name = get_process_name_by_pid(target_pid);
            }
            if (!proc_name.empty()) {
                trigger_engine_.on_process_launched(target_pid, proc_name);
            }
        }
    } else if (ctx.event_id == 2 && p_event->UserDataLength >= 4 && p_event->UserData) {
        uint32_t target_pid = 0;
        std::memcpy(&target_pid, p_event->UserData, sizeof(uint32_t));
        if (target_pid != 0) {
            rec.pid = target_pid;
            trigger_engine_.on_process_terminated(target_pid);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_nt_dpc_isr_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    // DPC Completion
    if (ctx.opcode == KERNEL_OPCODE_DPC_CLASSIC || ctx.event_id == KERNEL_OPCODE_DPC_CLASSIC || 
        ctx.opcode == KERNEL_OPCODE_DPC || ctx.event_id == KERNEL_OPCODE_DPC || 
        ctx.opcode == KERNEL_OPCODE_TIMER || ctx.event_id == KERNEL_OPCODE_TIMER || 
        (p_event->EventHeader.EventDescriptor.Task == 1 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::DPC);

        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t initial_time = 0;
            uint64_t routine = 0;
            if (p_event->UserDataLength == 12) {
                uint32_t initial_time_32 = 0;
                std::memcpy(&initial_time_32, p_event->UserData, sizeof(uint32_t));
                initial_time = initial_time_32;
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 4, sizeof(uint64_t));
            } else if (p_event->UserDataLength >= 16) {
                std::memcpy(&initial_time, p_event->UserData, sizeof(uint64_t));
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            }
            if (initial_time > 0 || routine > 0) {
                rec.payload.routine_addr = routine;
                rec.auxiliary_data = routine;
                if (ctx.timestamp >= initial_time) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - initial_time, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // ISR Completion
    else if (ctx.opcode == KERNEL_OPCODE_ISR_CLASSIC || ctx.event_id == KERNEL_OPCODE_ISR_CLASSIC || (p_event->EventHeader.EventDescriptor.Task == 2 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::ISR);

        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t initial_time = 0;
            uint64_t routine = 0;
            if (p_event->UserDataLength == 12) {
                uint32_t initial_time_32 = 0;
                std::memcpy(&initial_time_32, p_event->UserData, sizeof(uint32_t));
                initial_time = initial_time_32;
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 4, sizeof(uint64_t));
            } else if (p_event->UserDataLength >= 16) {
                std::memcpy(&initial_time, p_event->UserData, sizeof(uint64_t));
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            }
            if (initial_time > 0 || routine > 0) {
                rec.payload.routine_addr = routine;
                rec.auxiliary_data = routine;
                if (ctx.timestamp >= initial_time) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - initial_time, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // Kernel Profile / Sampled Profile (Opcode 46 / PerfInfo Sample)
    else if (ctx.opcode == 46 || (p_event->EventHeader.EventDescriptor.Task == 7 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::PROFILE);
        if (p_event->UserDataLength >= sizeof(uint64_t) && p_event->UserData) {
            uint64_t ip = 0;
            std::memcpy(&ip, p_event->UserData, sizeof(uint64_t));
            rec.payload.routine_addr = ip;
            rec.auxiliary_data = ip;
        }
        flight_recorder_.push(rec);

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_nt_cswitch_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::CSWITCH);

    if (p_event->UserDataLength >= 15 && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        uint32_t new_tid = 0;
        uint32_t old_tid = 0;
        uint8_t old_state = 0;

        std::memcpy(&new_tid, raw + 0, sizeof(uint32_t));
        std::memcpy(&old_tid, raw + 4, sizeof(uint32_t));
        std::memcpy(&old_state, raw + 14, sizeof(uint8_t));

        tid_to_pid_.insert(new_tid, { ctx.pid, ctx.timestamp });

        uint32_t old_pid = 0;
        TidPidEntry entry{};
        if (tid_to_pid_.lookup(old_tid, entry)) {
            old_pid = entry.pid;
        }

        if (old_tid != 0) {
            in_flight_threads_.insert(old_tid, { ctx.timestamp, old_pid, old_state });
        }

        ThreadSwitchOut so{};
        if (in_flight_threads_.find_and_erase(new_tid, so)) {
            if (so.pid == 0 || so.pid == ctx.pid) {
                if (ctx.timestamp >= so.qpc) {
                    const uint64_t dur_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - so.qpc, qpc_freq_));
                    rec.duration_us = (dur_us <= 10000000ULL) ? static_cast<uint32_t>(dur_us) : 10000000U;
                }
                if (so.wait_state == 5 || so.wait_state == 4) {
                    rec.flags |= EventFlags::CSWITCH_VOLUNTARY;
                }
            }
        }

        if (old_state == 5 || old_state == 4) {
            rec.flags |= EventFlags::CSWITCH_OUT_VOLUNTARY;
        }

        rec.tid = new_tid;
        rec.payload.cswitch.prev_tid = old_tid;
        rec.payload.cswitch.prev_pid = old_pid;

        flight_recorder_.push(rec);
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_nt_disk_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DISK);

    if (ctx.opcode == KERNEL_OPCODE_DISK_READ_INIT || ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) {
        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t irp = 0;
            uint32_t issuing_tid = ctx.tid;
            std::memcpy(&irp, p_event->UserData, sizeof(uint64_t));
            std::memcpy(&issuing_tid, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint32_t));
            if (issuing_tid == 0) issuing_tid = ctx.tid;
            in_flight_disk_.insert(irp, { ctx.timestamp, ctx.pid, issuing_tid, (ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) });
            rec.payload.file_key = irp;
            rec.auxiliary_data = 0;
            if (ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) {
                rec.flags |= EventFlags::DISK_IS_WRITE;
            }
        }
    } else if (ctx.opcode == KERNEL_OPCODE_DISK_READ || ctx.opcode == KERNEL_OPCODE_DISK_WRITE) {
        if (ctx.opcode == KERNEL_OPCODE_DISK_WRITE) {
            rec.flags |= EventFlags::DISK_IS_WRITE;
        }
        if (p_event->UserDataLength >= 40 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t size_bytes = 0;
            uint64_t irp = 0;

            std::memcpy(&size_bytes, raw + 8, sizeof(uint32_t));
            std::memcpy(&irp, raw + 32, sizeof(uint64_t));

            rec.payload.file_key = irp;
            rec.auxiliary_data = size_bytes;

            DiskInFlight disk_data{};
            if (in_flight_disk_.find_and_erase(irp, disk_data)) {
                if (disk_data.pid != 0) {
                    rec.pid = disk_data.pid;
                    rec.tid = disk_data.tid;
                    if (ctx.timestamp >= disk_data.start_qpc) {
                        const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - disk_data.start_qpc, qpc_freq_));
                        if (delta_us <= 3000000ULL) {
                            rec.duration_us = static_cast<uint32_t>(delta_us);
                        }
                    }
                } else {
                    rec.duration_us = 0;
                }
                if (disk_data.is_write) {
                    rec.flags |= EventFlags::DISK_IS_WRITE;
                }
            } else {
                rec.duration_us = 0;
            }
            if (rec.duration_us > 0 || rec.pid != 0) {
                flight_recorder_.push(rec);
            }
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_nt_fault_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    // Hard Page Fault (Opcode 32 / HardFault)
    if (ctx.opcode == KERNEL_OPCODE_HARDFAULT) {
        rec.category = static_cast<uint16_t>(EventCategory::PAGE_FAULT);

        if (p_event->UserDataLength >= 40 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            int64_t initial_time = 0;
            uint64_t file_object = 0;
            uint32_t byte_count = 0;
            std::memcpy(&initial_time, raw + 0, sizeof(int64_t));
            std::memcpy(&file_object, raw + 24, sizeof(uint64_t));
            std::memcpy(&byte_count, raw + 36, sizeof(uint32_t));
            rec.payload.file_key = file_object;
            rec.auxiliary_data = byte_count;

            uint64_t initial_time_ft = static_cast<uint64_t>(initial_time);
            constexpr uint64_t MIN_VALID_FILETIME = 125911584000000000ULL; // Jan 1, 2000 00:00:00 UTC
            uint64_t sync_utc = 0, sync_qpc = 0;
            read_sync_time(sync_utc, sync_qpc);

            if (initial_time_ft >= MIN_VALID_FILETIME && sync_utc > 0 && sync_qpc > 0 && ctx.timestamp >= sync_qpc) {
                const uint64_t delta_qpc = ctx.timestamp - sync_qpc;
                const uint64_t q = delta_qpc / qpc_freq_;
                const uint64_t r = delta_qpc % qpc_freq_;
                const uint64_t delta_100ns = (q * 10000000ULL) + ((r * 10000000ULL) / qpc_freq_);
                const uint64_t end_ft = sync_utc + delta_100ns;
                if (end_ft >= initial_time_ft) {
                    uint64_t dur_100ns = end_ft - initial_time_ft;
                    uint64_t dur_us = dur_100ns / 10;
                    if (dur_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(dur_us);
                    }
                }
            } else {
                rec.duration_us = 0;
            }
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // VirtualAlloc (Opcode 98 under PAGE_FAULT_GUID)
    else if (ctx.opcode == KERNEL_OPCODE_VIRTUAL_ALLOC) {
        rec.category = static_cast<uint16_t>(EventCategory::MEM_VIRTUAL_ALLOC);

        if (p_event->UserDataLength >= 24 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint64_t base_addr = 0;
            uint64_t region_size = 0;
            uint32_t alloc_pid = 0;
            uint32_t alloc_flags = 0;
            std::memcpy(&base_addr, raw + 0, sizeof(uint64_t));
            std::memcpy(&region_size, raw + 8, sizeof(uint64_t));
            std::memcpy(&alloc_pid, raw + 16, sizeof(uint32_t));
            std::memcpy(&alloc_flags, raw + 20, sizeof(uint32_t));

            rec.pid = alloc_pid;
            rec.payload.routine_addr = base_addr;
            rec.auxiliary_data = region_size;
            rec.flags = (alloc_flags & MEM_COMMIT) ? EventFlags::MEM_ALLOC_COMMIT : EventFlags::NONE;

            if (region_size >= (4 * 1024 * 1024ULL) && (alloc_flags & (MEM_COMMIT | MEM_RESET | MEM_LARGE_PAGES)) != 0) {
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
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
