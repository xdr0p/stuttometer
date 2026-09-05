#include "gui_controller.hpp"
#include <psapi.h>
#include <algorithm>
#include <set>

namespace stuttometer::gui {

// -----------------------------------------------------------------------------
// GuiLogRedirector Implementation
// -----------------------------------------------------------------------------
GuiLogRedirector::GuiLogRedirector(HWND target_hwnd)
    : hwnd_(target_hwnd)
{
    old_cout_buf_ = std::cout.rdbuf(this);
    old_cerr_buf_ = std::cerr.rdbuf(this);
}

GuiLogRedirector::~GuiLogRedirector() {
    set_shutting_down(true);
    if (old_cout_buf_) std::cout.rdbuf(old_cout_buf_);
    if (old_cerr_buf_) std::cerr.rdbuf(old_cerr_buf_);
    flush_buffer();
}

void GuiLogRedirector::emit_line(std::string&& line) {
    if (line.empty()) return;

    // Send to Windows Debug Output unconditionally so no line is dropped
    std::string out = "[Stuttometer] " + line + "\n";
    OutputDebugStringA(out.c_str());

    if (!shutting_down_.load(std::memory_order_acquire) && hwnd_ && IsWindow(hwnd_)) {
        auto* p_msg = new std::string(std::move(line));
        if (!PostMessage(hwnd_, WM_STUTTO_LOG, 0, reinterpret_cast<LPARAM>(p_msg))) {
            delete p_msg;
        }
    }
}

std::streambuf::int_type GuiLogRedirector::overflow(int_type c) {
    std::string to_emit;
    if (c != traits_type::eof()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (c == '\n') {
            if (!line_buffer_.empty()) {
                to_emit = std::move(line_buffer_);
                line_buffer_.clear();
            }
        } else if (c != '\r') {
            line_buffer_ += static_cast<char>(c);
        }
    }
    if (!to_emit.empty()) {
        emit_line(std::move(to_emit));
    }
    return (c == traits_type::eof()) ? traits_type::not_eof(c) : c;
}

std::streamsize GuiLogRedirector::xsputn(const char* s, std::streamsize count) {
    std::vector<std::string> lines_to_emit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::streamsize i = 0; i < count; ++i) {
            if (s[i] == '\n') {
                if (!line_buffer_.empty()) {
                    lines_to_emit.push_back(std::move(line_buffer_));
                    line_buffer_.clear();
                }
            } else if (s[i] != '\r') {
                line_buffer_ += s[i];
            }
        }
    }
    for (auto& l : lines_to_emit) {
        emit_line(std::move(l));
    }
    return count;
}

int GuiLogRedirector::sync() {
    // std::cerr has unitbuf enabled, triggering sync() after each operator<<.
    // Do not flush partial lines on intermediate syncs so multi-token logs stay on a single line.
    return 0;
}

void GuiLogRedirector::flush_buffer() {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (line_buffer_.empty()) return;
        line = std::move(line_buffer_);
        line_buffer_.clear();
    }
    emit_line(std::move(line));
}

// -----------------------------------------------------------------------------
// GuiController Implementation
// -----------------------------------------------------------------------------
GuiController::GuiController(HWND main_hwnd)
    : hwnd_(main_hwnd)
{
    log_redirector_ = std::make_unique<GuiLogRedirector>(main_hwnd);
}

GuiController::~GuiController() {
    shutdown();
}

void GuiController::shutdown() {
    if (log_redirector_) {
        log_redirector_->set_shutting_down(true);
    }
    cancel_enumeration_.store(true, std::memory_order_release);
    stop_session_async();

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (session_thread_.joinable()) {
            session_thread_.join();
        }
        if (process_enum_thread_.joinable()) {
            process_enum_thread_.join();
        }
    }

    log_redirector_.reset();
}

bool GuiController::start_session_async(const GuiConfig& config) {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (is_busy()) {
        return false;
    }

    if (session_thread_.joinable()) {
        session_thread_.join();
    }

    stop_worker_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> te_lock(trigger_engine_mutex_);
        target_process_name_ = config.target_process_name;
    }
    state_.store(GuiSessionState::STARTING, std::memory_order_release);
    PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::STARTING), 0);

    try {
        session_thread_ = std::thread(&GuiController::session_worker_loop, this, config);
    } catch (const std::exception& ex) {
        std::cerr << "[STUTTOMETER] Error: Failed to start session worker thread: " << ex.what() << "\n";
        state_.store(GuiSessionState::IDLE, std::memory_order_release);
        PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
        return false;
    }
    return true;
}

void GuiController::stop_session_async() {
    if (!is_capturing()) return;

    stop_worker_requested_.store(true, std::memory_order_release);
    state_.store(GuiSessionState::STOPPING, std::memory_order_release);
    PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::STOPPING), 0);

    // Thread joins will complete on background worker and post IDLE
}

void GuiController::update_target_filter(uint32_t pid, const std::string& process_name) {
    std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
    target_process_name_ = process_name;
    if (active_trigger_engine_) {
        active_trigger_engine_->update_target_pid(pid, (pid == 0 && !process_name.empty()));
    }
}

void GuiController::session_worker_loop(GuiConfig config) {
    try {
        if (!enable_system_profile_privilege()) {
            std::cerr << "[STUTTOMETER] Warning: Failed to enable SeSystemprofilePrivilege. Kernel session may be degraded.\n";
        }

        const uint64_t qpc_freq = get_qpc_frequency();

        TriggerConfig trig_config;
        trig_config.window_pre_ms = config.window_pre_ms;
        trig_config.window_post_ms = config.window_post_ms;
        trig_config.present_threshold_ms = config.present_threshold_ms;
        trig_config.audio_trigger_enabled = config.enable_audio;
        trig_config.cooldown_ms = config.cooldown_ms;
        trig_config.target_pid = config.target_pid;
        trig_config.target_process_name = config.target_process_name;
        trig_config.frame_trigger_mode = config.frame_trigger_mode;
        trig_config.spike_multiplier = config.spike_multiplier;
        trig_config.min_spike_delta_ms = config.min_spike_delta_ms;
        trig_config.enable_judder_detection = config.enable_judder_detection;
        trig_config.judder_swing_ratio = config.judder_swing_ratio;

        auto trigger_engine = std::make_unique<TriggerEngine>(trig_config, qpc_freq);
        TriggerEngine* engine_ptr = trigger_engine.get();
        {
            std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
            active_trigger_engine_ = std::move(trigger_engine);
        }

        EtwSessionConfig etw_config;
        etw_config.enable_dxgi = true;
        etw_config.enable_audio = config.enable_audio;
        etw_config.enable_kernel_dpc = (config.provider_tier != "minimal");
        etw_config.enable_kernel_disk = (config.provider_tier != "minimal");
        etw_config.enable_kernel_cswitch = (config.provider_tier == "full");
        etw_config.enable_dxgkrnl = (config.provider_tier != "minimal");
        etw_config.enable_dwm_core = (config.provider_tier != "minimal");
        etw_config.enable_kernel_pagefault = (config.provider_tier != "minimal");
        etw_config.enable_processor_power = (config.provider_tier != "minimal");
        etw_config.enable_antimalware = (config.provider_tier != "minimal");
        etw_config.enable_d3d12 = (config.provider_tier != "minimal");
        etw_config.enable_kernel_memory = (config.provider_tier != "minimal");

        uint32_t slots = (config.buffer_slots >= 65536) ? std::min(config.buffer_slots, 1048576U) : 262144;
        if ((etw_config.enable_dxgkrnl || etw_config.enable_dwm_core) && slots < 262144U) {
            std::cout << "[STUTTOMETER] Notice: Buffer capacity increased to 262,144 slots for DxgKrnl/DWM high-frequency frame events.\n";
            slots = 262144U;
        }
        FlightRecorder flight_recorder(slots);

        auto session_mgr = std::make_unique<EtwSessionManager>(flight_recorder, *engine_ptr, etw_config);

        const auto start_result = session_mgr->start();
        if (start_result == SessionStartResult::FAILED) {
            std::cerr << "[STUTTOMETER] Error: Failed to start ETW trace sessions.\n";
            state_.store(GuiSessionState::IDLE, std::memory_order_release);
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
            session_mgr.reset();
            {
                std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
                active_trigger_engine_.reset();
            }
            return;
        }

        if (stop_worker_requested_.load(std::memory_order_acquire)) {
            state_.store(GuiSessionState::IDLE, std::memory_order_release);
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
            session_mgr->stop();
            session_mgr.reset();
            {
                std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
                active_trigger_engine_.reset();
            }
            return;
        } else if (start_result == SessionStartResult::DEGRADED_USER_ONLY) {
            state_.store(GuiSessionState::DEGRADED_USER_ONLY, std::memory_order_release);
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::DEGRADED_USER_ONLY), 0);
        } else if (start_result == SessionStartResult::DEGRADED_KERNEL_ONLY) {
            state_.store(GuiSessionState::DEGRADED_KERNEL_ONLY, std::memory_order_release);
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::DEGRADED_KERNEL_ONLY), 0);
        } else {
            state_.store(GuiSessionState::RUNNING, std::memory_order_release);
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::RUNNING), 0);
        }

    DriverSymbolResolver driver_resolver;
    CorrelatorThresholds thresholds;
    thresholds.dpc_threshold_us = config.dpc_threshold_us;
    thresholds.isr_threshold_us = config.isr_threshold_us;
    thresholds.disk_threshold_ms = config.disk_threshold_ms;
    thresholds.cswitch_preempt_ms = config.cswitch_preempt_ms;
    thresholds.smi_severity_threshold_ms = config.smi_severity_threshold_ms;
    thresholds.d3d12_pso_threshold_ms = config.d3d12_pso_threshold_ms;
    thresholds.vram_demoted_threshold_mb = config.vram_demoted_threshold_mb;
    thresholds.mem_alloc_threshold_mb = config.mem_alloc_threshold_mb;
    thresholds.mem_trim_threshold_mb = config.mem_trim_threshold_mb;
    thresholds.mem_physical_latency_us = config.mem_physical_latency_us;
    CorrelationEngine correlator(driver_resolver, thresholds);

    // Dedicated background thread for asynchronous process name watcher (zero jitter on trigger loop)
    std::atomic<bool> watcher_running{true};
    std::thread watcher_thread([this, &watcher_running]() {
        while (watcher_running.load(std::memory_order_relaxed)) {
            std::string proc;
            bool is_waiting = false;
            {
                std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
                proc = target_process_name_;
                if (active_trigger_engine_) {
                    is_waiting = active_trigger_engine_->is_target_waiting();
                }
            }
            if (!proc.empty()) {
                const uint32_t found = resolve_process_name_to_pid(proc);
                std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
                if (active_trigger_engine_) {
                    if (found != 0) {
                        active_trigger_engine_->try_attach_pid(found);
                    } else {
                        const uint32_t active = active_trigger_engine_->active_target_pid();
                        if (active != 0) {
                            active_trigger_engine_->try_detach_pid(active);
                        }
                    }
                    is_waiting = active_trigger_engine_->is_target_waiting();
                }
            }
            // Poll responsive 50ms while waiting for target to launch; poll ~2s once attached
            const int sleep_steps = is_waiting ? 1 : 40;
            for (int i = 0; i < sleep_steps && watcher_running.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    struct WatcherJoinGuard {
        std::atomic<bool>& running;
        std::thread& th;
        ~WatcherJoinGuard() {
            running.store(false, std::memory_order_release);
            if (th.joinable()) {
                th.join();
            }
        }
    } watcher_guard{watcher_running, watcher_thread};

    if (!config.output_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(config.output_dir), ec);
        if (ec) {
            std::cerr << "[STUTTOMETER] Error: Failed to create auto-save directory: " << ec.message() << "\n";
            config.output_dir.clear();
        } else {
            std::cout << "[STUTTOMETER] Auto-saving reports to directory enabled.\n";
        }
    }

    JsonReporter reporter;
    uint32_t report_count = 0;
    uint32_t loop_counter = 0;

    while (!stop_worker_requested_.load(std::memory_order_relaxed)) {
        const uint64_t current_qpc = get_current_qpc();
        TriggerInfo trigger_info;
        uint64_t from_qpc = 0;
        uint64_t to_qpc = 0;

        // Live metrics update (every ~200ms)
        if (++loop_counter % 20 == 0 && hwnd_ && IsWindow(hwnd_)) {
            auto* p_metrics = new GuiMetrics();
            const uint64_t head = flight_recorder.current_head();
            const uint64_t dropped = flight_recorder.total_dropped_events();
            p_metrics->head_sequence = head;
            p_metrics->total_events_recorded = (head >= dropped) ? (head - dropped) : head;
            p_metrics->events_lost = session_mgr->events_lost();
            p_metrics->buffers_lost = session_mgr->buffers_lost();
            p_metrics->unpaired_evictions = session_mgr->unpaired_evictions();
            p_metrics->insertion_failures = session_mgr->insertion_failures();
            {
                std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
                if (active_trigger_engine_) {
                    p_metrics->suppressed_triggers = static_cast<uint32_t>(active_trigger_engine_->suppressed_trigger_count());
                }
            }

            if (!PostMessage(hwnd_, WM_STUTTO_METRICS, 0, reinterpret_cast<LPARAM>(p_metrics))) {
                delete p_metrics;
            }
        }

        // Detect if consumer degraded or failed during active capture
        if (session_mgr->consumer_failed()) {
            if (session_mgr->kernel_consumer_failed()) {
                session_mgr->stop_kernel_session();
            }
            if (session_mgr->user_consumer_failed()) {
                session_mgr->stop_user_session();
            }
            if (session_mgr->kernel_consumer_failed() && session_mgr->user_consumer_failed()) {
                std::cerr << "[STUTTOMETER] Error: All ETW trace consumers terminated unexpectedly.\n";
                stop_worker_requested_.store(true, std::memory_order_release);
            } else {
                auto cur = state_.load(std::memory_order_relaxed);
                if (cur == GuiSessionState::RUNNING) {
                    auto new_state = session_mgr->kernel_consumer_failed() ? GuiSessionState::DEGRADED_USER_ONLY : GuiSessionState::DEGRADED_KERNEL_ONLY;
                    state_.store(new_state, std::memory_order_release);
                    PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(new_state), 0);
                } else if ((cur == GuiSessionState::DEGRADED_USER_ONLY && session_mgr->user_consumer_failed()) ||
                           (cur == GuiSessionState::DEGRADED_KERNEL_ONLY && session_mgr->kernel_consumer_failed())) {
                    std::cerr << "[STUTTOMETER] Error: All ETW trace consumers terminated unexpectedly.\n";
                    stop_worker_requested_.store(true, std::memory_order_release);
                }
            }
        }

        bool triggered = false;
        {
            std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
            if (active_trigger_engine_) {
                triggered = active_trigger_engine_->poll_state(current_qpc, trigger_info, from_qpc, to_qpc);
            }
        }

        if (triggered) {
            struct ReportScopeGuard {
                std::unique_ptr<TriggerEngine>& engine_ptr;
                std::mutex& engine_mtx;
                ~ReportScopeGuard() {
                    std::lock_guard<std::mutex> lock(engine_mtx);
                    if (engine_ptr) {
                        engine_ptr->on_report_completed(get_current_qpc());
                    }
                }
            } guard{active_trigger_engine_, trigger_engine_mutex_};

            try {
                // Synchronously flush active buffers and deterministically drain post-trigger window
                session_mgr->flush_buffers();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
                while (session_mgr->last_processed_qpc() < to_qpc && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                uint64_t drops = 0;
                auto snapshot = flight_recorder.snapshot(from_qpc, to_qpc, &drops);

                ProviderContext p_ctx;
                p_ctx.kernel_dpc_active = session_mgr->is_kernel_session_active() && etw_config.enable_kernel_dpc;
                p_ctx.kernel_disk_active = session_mgr->is_kernel_session_active() && etw_config.enable_kernel_disk;
                p_ctx.kernel_cswitch_active = session_mgr->is_kernel_session_active() && etw_config.enable_kernel_cswitch;
                p_ctx.user_dxgi_active = session_mgr->is_user_session_active() && etw_config.enable_dxgi;
                p_ctx.user_audio_active = session_mgr->is_user_session_active() && etw_config.enable_audio;
                p_ctx.user_dxgkrnl_active = session_mgr->is_user_session_active() && etw_config.enable_dxgkrnl;
                p_ctx.user_dwm_active = session_mgr->is_user_session_active() && etw_config.enable_dwm_core;
                p_ctx.kernel_pagefault_active = session_mgr->is_kernel_session_active() && etw_config.enable_kernel_pagefault;
                p_ctx.user_processor_power_active = session_mgr->is_user_session_active() && etw_config.enable_processor_power;
                p_ctx.user_antimalware_active = session_mgr->is_user_session_active() && etw_config.enable_antimalware;
                p_ctx.user_d3d12_active = session_mgr->is_user_session_active() && etw_config.enable_d3d12;
                p_ctx.user_vram_paging_active = session_mgr->is_user_session_active() && etw_config.enable_dxgkrnl;
                // Note: Kernel-Memory provider runs on the user trace session, not the NT Kernel Logger
                p_ctx.kernel_memory_active = session_mgr->is_user_session_active() && etw_config.enable_kernel_memory;
                p_ctx.etw_events_lost = session_mgr->events_lost();
                p_ctx.etw_buffers_lost = session_mgr->buffers_lost();

                uint64_t unpaired_evicts = session_mgr->unpaired_evictions();
                uint64_t ins_failures = session_mgr->insertion_failures();
                auto report = correlator.correlate(snapshot, trigger_info, qpc_freq, p_ctx, drops, unpaired_evicts, ins_failures, flight_recorder.total_dropped_events());
                report.target_process = get_process_name_by_pid(trigger_info.target_pid);
                report.window_pre_ms = config.window_pre_ms;
                report.window_post_ms = config.window_post_ms;
                report.present_threshold_ms = config.present_threshold_ms;
                report.provider_tier = config.provider_tier;
                report.redacted = config.redact;

                // Auto-save JSON report to disk if configured (with 100-file rotation cap)
                if (!config.output_dir.empty()) {
                    const std::filesystem::path dir(config.output_dir);
                    const std::wstring filename = L"stutto_report_" + std::to_wstring(report_count + 1) + L"_" + std::to_wstring(current_qpc) + L".json";
                    const std::filesystem::path file_path = dir / filename;
                    if (!reporter.save_to_file(report, file_path, config.redact)) {
                        std::cerr << "[STUTTOMETER] Warning: Failed to auto-save JSON report to '" << file_path.generic_string() << "'\n";
                    } else {
                        // Keep directory capped to 100 latest reports
                        constexpr size_t MAX_SAVED_REPORTS = 100;
                        std::error_code dir_ec;
                        std::vector<std::filesystem::directory_entry> entries;
                        for (const auto& entry : std::filesystem::directory_iterator(dir, dir_ec)) {
                            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                                entries.push_back(entry);
                            }
                        }
                        if (entries.size() > MAX_SAVED_REPORTS) {
                            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                                return a.last_write_time() < b.last_write_time();
                            });
                            for (size_t i = 0; i < entries.size() - MAX_SAVED_REPORTS; ++i) {
                                std::filesystem::remove(entries[i].path(), dir_ec);
                            }
                        }
                    }
                }
                ++report_count;

                // Transfer ownership of report across thread boundary only on success
                auto p_report = std::make_unique<DiagnosticReport>(std::move(report));
                if (hwnd_ && IsWindow(hwnd_)) {
                    if (PostMessage(hwnd_, WM_STUTTO_TRIGGER, 0, reinterpret_cast<LPARAM>(p_report.get()))) {
                        p_report.release();
                    }
                }
            } catch (const std::exception& ex) {
                std::cerr << "[STUTTOMETER] Exception during correlation: " << ex.what() << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    watcher_running.store(false, std::memory_order_release);
    if (watcher_thread.joinable()) {
        watcher_thread.join();
    }

    // Teardown ETW sessions (joins threads off the UI thread)
    session_mgr->stop();
    session_mgr.reset();

    {
        std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
        active_trigger_engine_.reset();
    }

    state_.store(GuiSessionState::IDLE, std::memory_order_release);
    if (hwnd_ && IsWindow(hwnd_)) {
        PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
    }
    } catch (const std::exception& ex) {
        std::cerr << "[STUTTOMETER] Fatal error in session worker loop: " << ex.what() << "\n";
        {
            std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
            active_trigger_engine_.reset();
        }
        state_.store(GuiSessionState::IDLE, std::memory_order_release);
        if (hwnd_ && IsWindow(hwnd_)) {
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
        }
    } catch (...) {
        std::cerr << "[STUTTOMETER] Unknown fatal error in session worker loop.\n";
        {
            std::lock_guard<std::mutex> lock(trigger_engine_mutex_);
            active_trigger_engine_.reset();
        }
        state_.store(GuiSessionState::IDLE, std::memory_order_release);
        if (hwnd_ && IsWindow(hwnd_)) {
            PostMessage(hwnd_, WM_STUTTO_STATE_CHANGE, static_cast<WPARAM>(GuiSessionState::IDLE), 0);
        }
    }
}

// -----------------------------------------------------------------------------
// Graphical Process Enumerator (EnumWindows)
// -----------------------------------------------------------------------------
namespace {
struct EnumContext {
    std::atomic<bool>* cancel_flag{nullptr};
    ProcessList list;
    std::set<uint32_t> seen_pids;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (ctx && ctx->cancel_flag && ctx->cancel_flag->load(std::memory_order_relaxed)) {
        return FALSE;
    }

    if (!IsWindowVisible(hwnd)) return TRUE;

    // Check window style (exclude tool windows and zero-size windows)
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return TRUE;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    if ((rc.right - rc.left) <= 0 || (rc.bottom - rc.top) <= 0) return TRUE;

    // Guard against hung processes blocking cross-process WM_GETTEXT
    if (IsHungAppWindow(hwnd)) return TRUE;

    wchar_t title_buf[256] = {0};
    DWORD_PTR result_len = 0;
    LRESULT lr = SendMessageTimeoutW(
        hwnd,
        WM_GETTEXT,
        static_cast<WPARAM>(std::size(title_buf)),
        reinterpret_cast<LPARAM>(title_buf),
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        100,
        &result_len
    );
    if (lr == 0 || result_len == 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    if (ctx->seen_pids.find(pid) != ctx->seen_pids.end()) return TRUE;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return TRUE;

    std::wstring full_path(4096, L'\0');
    DWORD path_size = static_cast<DWORD>(full_path.size());
    if (QueryFullProcessImageNameW(hProcess, 0, full_path.data(), &path_size)) {
        full_path.resize(path_size);
        size_t last_slash = full_path.find_last_of(L"\\/");
        std::wstring exe_name = (last_slash != std::wstring::npos) ? full_path.substr(last_slash + 1) : full_path;

        ProcessEntry entry;
        entry.pid = pid;
        entry.name = exe_name;
        entry.window_title = std::wstring(title_buf);

        ctx->seen_pids.insert(pid);
        ctx->list.push_back(std::move(entry));
    }
    CloseHandle(hProcess);

    return TRUE;
}
} // namespace

void GuiController::enumerate_graphical_processes_async() {
    bool expected = false;
    if (!is_enumerating_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // Scan already in flight, ignore rapid clicks
    }

    try {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (process_enum_thread_.joinable()) {
            process_enum_thread_.join();
        }
        if (cancel_enumeration_.load(std::memory_order_acquire)) {
            is_enumerating_.store(false, std::memory_order_release);
            return;
        }
        process_enum_thread_ = std::thread([this]() {
            EnumContext ctx;
            ctx.cancel_flag = &cancel_enumeration_;
            EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

            if (cancel_enumeration_.load(std::memory_order_acquire)) {
                is_enumerating_.store(false, std::memory_order_release);
                return;
            }

            // Sort alphabetically by exe name (case-insensitive)
            std::sort(ctx.list.begin(), ctx.list.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
                return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });

            is_enumerating_.store(false, std::memory_order_release);

            if (cancel_enumeration_.load(std::memory_order_acquire)) {
                return;
            }

            auto p_list = std::make_unique<ProcessList>(std::move(ctx.list));
            if (hwnd_ && IsWindow(hwnd_)) {
                if (PostMessage(hwnd_, WM_STUTTO_PROCESSES_UPDATED, 0, reinterpret_cast<LPARAM>(p_list.get()))) {
                    p_list.release();
                }
            }
        });
    } catch (const std::exception& ex) {
        std::cerr << "[STUTTOMETER] Error: Failed to start process enumeration thread: " << ex.what() << "\n";
        is_enumerating_.store(false, std::memory_order_release);
    }
}

} // namespace stuttometer::gui
