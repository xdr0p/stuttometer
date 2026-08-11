#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <chrono>

namespace stuttometer {

static constexpr const wchar_t* USER_SESSION_NAME = L"StuttometerUserSession";
static constexpr const wchar_t* KERNEL_SESSION_NAME = KERNEL_LOGGER_NAMEW;

static std::atomic<EtwSessionManager*> g_active_manager{nullptr};

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
    g_active_manager.store(this, std::memory_order_release);
}

EtwSessionManager::~EtwSessionManager() {
    stop();
    g_active_manager.store(nullptr, std::memory_order_release);
}

bool EtwSessionManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

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
        p_user_props->BufferSize = 64;         // 64 KB buffers
        p_user_props->MinimumBuffers = 16;
        p_user_props->MaximumBuffers = 64;
        p_user_props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        // Clean up stale user session if previously left open
        ControlTraceW(0, USER_SESSION_NAME, p_user_props, EVENT_TRACE_CONTROL_STOP);

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
        p_kernel_props->BufferSize = 64;
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

        // Non-destructive check: query if NT Kernel Logger is already active
        ULONG query_status = ControlTraceW(0, KERNEL_SESSION_NAME, p_kernel_props, EVENT_TRACE_CONTROL_QUERY);
        if (query_status == ERROR_SUCCESS) {
            std::cerr << "[ETW] Notice: NT Kernel Logger is currently active on the system.\n";
        }

        ULONG status = StartTraceW(&kernel_session_handle_, KERNEL_SESSION_NAME, p_kernel_props);
        if (status == ERROR_ALREADY_EXISTS) {
            std::cerr << "[ETW] Warning: NT Kernel Logger session already owned by another tool (e.g. WPA/Antivirus).\n";
        } else if (status != ERROR_SUCCESS) {
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
    if (!p_event) return;
    EtwSessionManager* mgr = g_active_manager.load(std::memory_order_acquire);
    if (!mgr) return;

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
        
        // Extract swapchain pointer from payload if present
        uint64_t swapchain_ptr = 0;
        if (p_event->UserDataLength >= 8) {
            swapchain_ptr = *reinterpret_cast<const uint64_t*>(p_event->UserData);
        }
        const uint64_t present_key = (static_cast<uint64_t>(tid) << 32) | (swapchain_ptr & 0xFFFFFFFF);

        // Event ID 42 (Present Start), 43 (Present Stop)
        if (event_id == 42 || opcode == 1) { // Start
            mgr->in_flight_present_.insert(present_key, { timestamp, pid, tid });
        } else if (event_id == 43 || opcode == 2) { // Stop
            PresentInFlight present_data{};
            if (mgr->in_flight_present_.find_and_erase(present_key, present_data)) {
                if (timestamp >= present_data.start_qpc) {
                    const double dur_ms = qpc_delta_to_ms(timestamp - present_data.start_qpc, mgr->qpc_freq_);
                    rec.duration_us = static_cast<uint32_t>(dur_ms * 1000.0);
                    mgr->trigger_engine_.on_dxgi_present(pid, tid, dur_ms, timestamp);
                }
            }
        }
        mgr->flight_recorder_.push(rec);
    }
    else if (IsEqualGUID(p_event->EventHeader.ProviderId, AUDIO_PROVIDER_GUID)) {
        rec.category = static_cast<uint16_t>(EventCategory::AUDIO);
        if (event_id == 11) { // AudioGlitch
            rec.flags |= EventFlags::AUDIO_BUFFER_UNDERRUN;
            rec.payload.audio.glitch_count = 1;
            mgr->trigger_engine_.on_audio_glitch(pid, tid, 1, timestamp);
        }
        mgr->flight_recorder_.push(rec);
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
                    rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - initial_time, mgr->qpc_freq_));
                }
            }
            mgr->flight_recorder_.push(rec);
        }
        // ISR completion
        else if (opcode == 67 || (p_event->EventHeader.EventDescriptor.Task == 2 && opcode == 2)) {
            rec.category = static_cast<uint16_t>(EventCategory::ISR);
            if (p_event->UserDataLength >= 16) {
                const uint64_t initial_time = *reinterpret_cast<const uint64_t*>(p_event->UserData);
                const uint64_t routine = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 8);
                rec.payload.routine_addr = routine;
                if (timestamp >= initial_time) {
                    rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - initial_time, mgr->qpc_freq_));
                }
            }
            mgr->flight_recorder_.push(rec);
        }
        // Context Switch (Event ID 36 / Opcode 36)
        else if (event_id == 36 || opcode == 36) {
            rec.category = static_cast<uint16_t>(EventCategory::CSWITCH);
            if (p_event->UserDataLength >= 24) {
                const uint32_t new_tid = *reinterpret_cast<const uint32_t*>(p_event->UserData);
                const uint32_t old_tid = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 4);
                const uint8_t old_wait_mode = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 13);
                const uint8_t old_state = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 14);

                // Record switch-out timestamp for old_tid
                mgr->in_flight_threads_.insert(old_tid, timestamp);

                // Look up descheduled duration for resuming new_tid
                uint64_t switch_out_qpc = 0;
                if (mgr->in_flight_threads_.find_and_erase(new_tid, switch_out_qpc)) {
                    if (timestamp >= switch_out_qpc) {
                        rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - switch_out_qpc, mgr->qpc_freq_));
                    }
                }

                rec.tid = new_tid;
                rec.payload.cswitch.prev_tid = old_tid;

                // State 5 = Waiting, WaitMode 1 = UserMode -> Voluntary switch
                if (old_state == 5 || old_wait_mode == 1) {
                    rec.flags |= EventFlags::CSWITCH_VOLUNTARY;
                }
            }
            mgr->flight_recorder_.push(rec);
        }
        // Disk I/O (ReadInit=12, WriteInit=13, Read=10, Write=11)
        else if (opcode >= 10 && opcode <= 14) {
            rec.category = static_cast<uint16_t>(EventCategory::DISK);

            if (opcode == 12 || opcode == 13) {
                // Disk I/O Init: record IRP with initiating PID/TID
                if (p_event->UserDataLength >= 16) {
                    const uint64_t irp = *reinterpret_cast<const uint64_t*>(p_event->UserData);
                    const uint32_t size_bytes = (p_event->UserDataLength >= 24)
                        ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 16) : 0;
                    mgr->in_flight_disk_.insert(irp, { timestamp, pid, tid, size_bytes, (opcode == 13) });
                }
            } else if (opcode == 10 || opcode == 11) {
                // Disk I/O Complete: pair with Init to map true initiating process
                if (opcode == 11) {
                    rec.flags |= EventFlags::DISK_IS_WRITE;
                }
                if (p_event->UserDataLength >= 8) {
                    const uint64_t irp = *reinterpret_cast<const uint64_t*>(p_event->UserData);
                    DiskInFlight disk_data{};

                    if (mgr->in_flight_disk_.find_and_erase(irp, disk_data)) {
                        rec.pid = disk_data.pid;
                        rec.tid = disk_data.tid;
                        if (timestamp >= disk_data.start_qpc) {
                            rec.duration_us = static_cast<uint32_t>(qpc_delta_to_us(timestamp - disk_data.start_qpc, mgr->qpc_freq_));
                        }
                        rec.auxiliary_data = disk_data.size;
                        if (disk_data.is_write) {
                            rec.flags |= EventFlags::DISK_IS_WRITE;
                        }
                    } else {
                        // Graceful fallback for unpaired completions
                        rec.pid = 0;
                        if (p_event->UserDataLength >= 28) {
                            rec.duration_us = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p_event->UserData) + 20);
                        }
                    }
                }
                mgr->flight_recorder_.push(rec);
            }
        }
    }
}

} // namespace stuttometer
