#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <initguid.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <iostream>
#include <chrono>

namespace stuttometer {

static constexpr const wchar_t* USER_SESSION_NAME = L"StuttometerUserSession";
static constexpr const wchar_t* KERNEL_SESSION_NAME = KERNEL_LOGGER_NAMEW;

static EtwSessionManager* g_active_manager = nullptr;

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
    g_active_manager = this;
}

EtwSessionManager::~EtwSessionManager() {
    stop();
    if (g_active_manager == this) {
        g_active_manager = nullptr;
    }
}

void EtwSessionManager::cleanup_stale_sessions() {
    // Attempt to stop existing sessions if they were left open by a previous crash
    const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    std::vector<uint8_t> buffer(prop_size, 0);
    auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    p_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);

    ControlTraceW(0, USER_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_STOP);
    ControlTraceW(0, KERNEL_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_STOP);
}

bool EtwSessionManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    cleanup_stale_sessions();

    const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;

    // 1. Configure User-Mode Trace Session (DXGI & Audio)
    if (config_.enable_dxgi || config_.enable_audio) {
        std::vector<uint8_t> user_props_buf(prop_size, 0);
        auto p_user_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(user_props_buf.data());
        p_user_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_user_props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p_user_props->Wnode.ClientContext = 1; // QPC Clock
        p_user_props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p_user_props->FlushTimer = 1;          // 1 second (active worker flushes every 30ms)
        p_user_props->BufferSize = 8;          // 8 KB buffers
        p_user_props->MinimumBuffers = 16;
        p_user_props->MaximumBuffers = 64;
        p_user_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        ULONG status = StartTraceW(&user_session_handle_, USER_SESSION_NAME, p_user_props);
        if (status != ERROR_SUCCESS) {
            std::cerr << "[ETW] Warning: Failed to start User Trace Session (Error " << status << ")\n";
        } else {
            if (config_.enable_dxgi) {
                EnableTraceEx2(user_session_handle_, &DXGI_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                               TRACE_LEVEL_INFORMATION, 0xFFFFFFFFFFFFFFFF, 0, 0, nullptr);
            }
            if (config_.enable_audio) {
                EnableTraceEx2(user_session_handle_, &AUDIO_PROVIDER_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                               TRACE_LEVEL_INFORMATION, 0xFFFFFFFFFFFFFFFF, 0, 0, nullptr);
            }
        }
    }

    // 2. Configure Kernel Trace Session (DPC, ISR, Disk, CSwitch)
    if (config_.enable_kernel_dpc || config_.enable_kernel_disk || config_.enable_kernel_cswitch) {
        std::vector<uint8_t> kernel_props_buf(prop_size, 0);
        auto p_kernel_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(kernel_props_buf.data());
        p_kernel_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);
        p_kernel_props->Wnode.Guid = SYSTEM_TRACE_CONTROL_GUID;
        p_kernel_props->Wnode.ClientContext = 1; // QPC Clock
        p_kernel_props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p_kernel_props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p_kernel_props->FlushTimer = 1;
        p_kernel_props->BufferSize = 8;
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
        p_kernel_props->EnableFlags = flags;

        ULONG status = StartTraceW(&kernel_session_handle_, KERNEL_SESSION_NAME, p_kernel_props);
        if (status != ERROR_SUCCESS) {
            std::cerr << "[ETW] Warning: Failed to start Kernel Trace Session (Error " << status 
                      << "). Note: Kernel tracing requires Administrator elevation.\n";
        }
    }

    running_.store(true, std::memory_order_release);

    // 3. Launch background active flush worker
    flush_worker_thread_ = std::thread(&EtwSessionManager::active_flush_worker_loop, this);

    // 4. Launch consumer threads
    if (user_session_handle_) {
        user_consumer_thread_ = std::thread(&EtwSessionManager::user_trace_consumer_loop, this);
    }
    if (kernel_session_handle_) {
        kernel_consumer_thread_ = std::thread(&EtwSessionManager::kernel_trace_consumer_loop, this);
    }

    return true;
}

void EtwSessionManager::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Close trace processing handles to unblock ProcessTrace loops
    if (user_trace_handle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(user_trace_handle_);
        user_trace_handle_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (kernel_trace_handle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(kernel_trace_handle_);
        kernel_trace_handle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    // Wait for consumer threads to finish
    if (flush_worker_thread_.joinable()) {
        flush_worker_thread_.join();
    }
    if (user_consumer_thread_.joinable()) {
        user_consumer_thread_.join();
    }
    if (kernel_consumer_thread_.joinable()) {
        kernel_consumer_thread_.join();
    }

    // Stop ETW trace sessions
    const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    std::vector<uint8_t> buffer(prop_size, 0);
    auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    p_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);

    if (user_session_handle_) {
        ControlTraceW(user_session_handle_, USER_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_STOP);
        user_session_handle_ = 0;
    }
    if (kernel_session_handle_) {
        ControlTraceW(kernel_session_handle_, KERNEL_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_STOP);
        kernel_session_handle_ = 0;
    }
}

void EtwSessionManager::active_flush_worker_loop() {
    const size_t prop_size = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
    std::vector<uint8_t> buffer(prop_size, 0);
    auto p_props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buffer.data());
    p_props->Wnode.BufferSize = static_cast<ULONG>(prop_size);

    const auto interval = std::chrono::milliseconds(config_.flush_interval_ms);

    while (running_.load(std::memory_order_relaxed)) {
        if (user_session_handle_) {
            ControlTraceW(user_session_handle_, USER_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_FLUSH);
        }
        if (kernel_session_handle_) {
            ControlTraceW(kernel_session_handle_, KERNEL_SESSION_NAME, p_props, EVENT_TRACE_CONTROL_FLUSH);
        }
        std::this_thread::sleep_for(interval);
    }
}

void EtwSessionManager::user_trace_consumer_loop() {
    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = const_cast<LPWSTR>(USER_SESSION_NAME);
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log_file.EventRecordCallback = &EtwSessionManager::on_event_record;

    user_trace_handle_ = OpenTraceW(&log_file);
    if (user_trace_handle_ != INVALID_PROCESSTRACE_HANDLE) {
        ProcessTrace(&user_trace_handle_, 1, nullptr, nullptr);
    }
}

void EtwSessionManager::kernel_trace_consumer_loop() {
    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = const_cast<LPWSTR>(KERNEL_SESSION_NAME);
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log_file.EventRecordCallback = &EtwSessionManager::on_event_record;

    kernel_trace_handle_ = OpenTraceW(&log_file);
    if (kernel_trace_handle_ != INVALID_PROCESSTRACE_HANDLE) {
        ProcessTrace(&kernel_trace_handle_, 1, nullptr, nullptr);
    }
}

void WINAPI EtwSessionManager::on_event_record(PEVENT_RECORD p_event) {
    if (!p_event || !g_active_manager) return;

    const uint64_t timestamp = static_cast<uint64_t>(p_event->EventHeader.TimeStamp.QuadPart);
    const uint32_t pid = p_event->EventHeader.ProcessId;
    const uint32_t tid = p_event->EventHeader.ThreadId;
    const uint8_t cpu = static_cast<uint8_t>(p_event->BufferContext.ProcessorNumber);
    const uint16_t event_id = p_event->EventHeader.EventDescriptor.Id;
    const uint8_t opcode = p_event->EventHeader.EventDescriptor.Opcode;

    EtwEventRecord rec{};
    rec.qpc_timestamp = timestamp;
    rec.pid = pid;
    rec.tid = tid;
    rec.cpu_index = cpu;
    rec.event_id = event_id;

    // Check Provider GUID
    if (IsEqualGUID(p_event->EventHeader.ProviderId, DXGI_PROVIDER_GUID)) {
        rec.category = static_cast<uint16_t>(EventCategory::DXGI);
        // Event ID 42 (Present Start), 43 (Present Stop)
        if (event_id == 42 || opcode == 1) { // Start
            std::lock_guard<std::mutex> lock(g_active_manager->present_mutex_);
            g_active_manager->in_flight_present_[tid] = { timestamp, pid, tid };
        } else if (event_id == 43 || opcode == 2) { // Stop
            uint64_t start_qpc = 0;
            {
                std::lock_guard<std::mutex> lock(g_active_manager->present_mutex_);
                auto it = g_active_manager->in_flight_present_.find(tid);
                if (it != g_active_manager->in_flight_present_.end()) {
                    start_qpc = it->second.start_qpc;
                    g_active_manager->in_flight_present_.erase(it);
                }
            }
            if (start_qpc > 0 && timestamp >= start_qpc) {
                const double dur_ms = qpc_delta_to_ms(timestamp - start_qpc, g_active_manager->qpc_freq_);
                rec.duration_us = static_cast<uint32_t>(dur_ms * 1000.0);
                g_active_manager->trigger_engine_.on_dxgi_present(pid, tid, dur_ms, timestamp);
            }
        }
        g_active_manager->flight_recorder_.push(rec);
    }
    else if (IsEqualGUID(p_event->EventHeader.ProviderId, AUDIO_PROVIDER_GUID)) {
        rec.category = static_cast<uint16_t>(EventCategory::AUDIO);
        if (event_id == 11) { // AudioGlitch
            rec.flags |= EventFlags::AUDIO_BUFFER_UNDERRUN;
            rec.payload.audio.glitch_count = 1;
            g_active_manager->trigger_engine_.on_audio_glitch(pid, tid, 1, timestamp);
        }
        g_active_manager->flight_recorder_.push(rec);
    }
    else {
        // Kernel Provider Events (HookId / Opcode parsing)
        // DPC completion
        if (opcode == 69 || (p_event->EventHeader.EventDescriptor.Task == 1 && opcode == 2)) {
            rec.category = static_cast<uint16_t>(EventCategory::DPC);
            if (p_event->UserDataLength >= 16) {
                // Layout: InitialTime (uint64), Routine (uint64)
                const uint64_t initial_time = *reinterpret_cast<const uint64_t*>(p_event->UserData);
                const uint64_t routine = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 8);
                rec.payload.routine_addr = routine;
                if (timestamp >= initial_time) {
                    rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - initial_time, g_active_manager->qpc_freq_));
                }
            }
            g_active_manager->flight_recorder_.push(rec);
        }
        // ISR completion
        else if (opcode == 67 || (p_event->EventHeader.EventDescriptor.Task == 2 && opcode == 2)) {
            rec.category = static_cast<uint16_t>(EventCategory::ISR);
            if (p_event->UserDataLength >= 16) {
                const uint64_t initial_time = *reinterpret_cast<const uint64_t*>(p_event->UserData);
                const uint64_t routine = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 8);
                rec.payload.routine_addr = routine;
                if (timestamp >= initial_time) {
                    rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - initial_time, g_active_manager->qpc_freq_));
                }
            }
            g_active_manager->flight_recorder_.push(rec);
        }
        // Context Switch (Event ID 36 / Opcode 36)
        else if (event_id == 36 || opcode == 36) {
            rec.category = static_cast<uint16_t>(EventCategory::CSWITCH);
            if (p_event->UserDataLength >= 24) {
                const uint32_t new_tid = *reinterpret_cast<const uint32_t*>(p_event->UserData);
                const uint32_t old_tid = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 4);
                rec.payload.cswitch.prev_tid = old_tid;
                rec.tid = new_tid;
            }
            g_active_manager->flight_recorder_.push(rec);
        }
        // Disk I/O (Read=10, Write=11, ReadInit=12, WriteInit=13)
        else if (opcode >= 10 && opcode <= 14) {
            rec.category = static_cast<uint16_t>(EventCategory::DISK);
            if (opcode == 11 || opcode == 13) {
                rec.flags |= EventFlags::DISK_IS_WRITE;
            }
            if (p_event->UserDataLength >= 24) {
                const uint64_t file_obj = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 8);
                const uint32_t size_bytes = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 16);
                rec.payload.file_key = file_obj;
                rec.auxiliary_data = size_bytes;
                if (p_event->UserDataLength >= 28) {
                    rec.duration_us = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 20);
                }
            }
            g_active_manager->flight_recorder_.push(rec);
        }
    }
}

} // namespace stuttometer
