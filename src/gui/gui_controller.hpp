#pragma once

#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <iostream>
#include <sstream>

namespace stuttometer::gui {

// Custom Win32 notification messages
constexpr UINT WM_STUTTO_STATE_CHANGE        = WM_USER + 101; // WPARAM: GuiSessionState
constexpr UINT WM_STUTTO_TRIGGER             = WM_USER + 102; // LPARAM: DiagnosticReport* (caller takes ownership)
constexpr UINT WM_STUTTO_METRICS             = WM_USER + 103; // LPARAM: GuiMetrics* (caller takes ownership)
constexpr UINT WM_STUTTO_PROCESSES_UPDATED   = WM_USER + 104; // LPARAM: ProcessList* (caller takes ownership)
constexpr UINT WM_STUTTO_LOG                 = WM_USER + 105; // LPARAM: std::string* (caller takes ownership)

enum class GuiSessionState : uint32_t {
    IDLE = 0,
    STARTING = 1,
    RUNNING = 2,
    DEGRADED_USER_ONLY = 3,
    DEGRADED_KERNEL_ONLY = 4,
    STOPPING = 5
};

struct GuiConfig {
    // Window & Timing
    double window_pre_ms{250.0};
    double window_post_ms{30.0};
    double present_threshold_ms{16.67};
    double cooldown_ms{1000.0};
    bool enable_audio{true};
    std::string provider_tier{"standard"};
    uint32_t buffer_slots{262144};

    // Correlation Diagnostic Thresholds
    uint32_t dpc_threshold_us{1000};
    uint32_t isr_threshold_us{500};
    uint32_t disk_threshold_ms{20};
    uint32_t cswitch_preempt_ms{5};
    double smi_severity_threshold_ms{33.3};
    uint32_t d3d12_pso_threshold_ms{5};
    uint32_t vram_demoted_threshold_mb{8};
    uint32_t mem_alloc_threshold_mb{16};
    uint32_t mem_trim_threshold_mb{4};
    uint32_t mem_physical_latency_us{1000};

    // Session Filters & Redaction & Auto-Save
    uint32_t target_pid{0};
    std::string target_process_name;
    bool redact{false};
    std::wstring output_dir;

    // Frame Pacing & Dynamic Relative Trigger configuration
    FrameTriggerMode frame_trigger_mode{FrameTriggerMode::HYBRID};
    double spike_multiplier{2.0};
    double min_spike_delta_ms{4.0};
    bool enable_judder_detection{true};
    double judder_swing_ratio{0.35};
};

struct GuiMetrics {
    uint64_t head_sequence{0};
    uint64_t total_events_recorded{0};
    uint32_t events_lost{0};
    uint32_t buffers_lost{0};
    uint64_t unpaired_evictions{0};
    uint64_t insertion_failures{0};
    uint32_t suppressed_triggers{0};
};

struct ProcessEntry {
    uint32_t pid{0};
    std::wstring name;
    std::wstring window_title;

    bool operator==(const ProcessEntry& other) const = default;
};

using ProcessList = std::vector<ProcessEntry>;

// Thread-safe streambuf redirector to intercept std::cout and std::cerr in WIN32 subsystem
class GuiLogRedirector : public std::streambuf {
public:
    explicit GuiLogRedirector(HWND target_hwnd);
    ~GuiLogRedirector() override;

    void set_shutting_down(bool val) noexcept { shutting_down_.store(val, std::memory_order_release); }

protected:
    int_type overflow(int_type c) override;
    std::streamsize xsputn(const char* s, std::streamsize count) override;
    int sync() override;

private:
    void flush_buffer();
    void emit_line(std::string&& line);

    HWND hwnd_{nullptr};
    std::streambuf* old_cout_buf_{nullptr};
    std::streambuf* old_cerr_buf_{nullptr};
    std::string line_buffer_;
    std::mutex mutex_;
    std::atomic<bool> shutting_down_{false};
};

class GuiController {
public:
    explicit GuiController(HWND main_hwnd);
    ~GuiController();

    // Start background ETW session asynchronously
    bool start_session_async(const GuiConfig& config);

    // Stop background ETW session asynchronously and join worker threads
    void stop_session_async();

    // Scan running graphical applications asynchronously (EnumWindows)
    void enumerate_graphical_processes_async();

    // Update target PID/process while running
    void update_target_filter(uint32_t pid, const std::string& process_name);

    // Clean shutdown: stops and joins all threads, drains queue
    void shutdown();

    GuiSessionState current_state() const noexcept {
        return state_.load(std::memory_order_relaxed);
    }

    bool is_busy() const noexcept {
        auto s = current_state();
        return s != GuiSessionState::IDLE;
    }

    bool is_capturing() const noexcept {
        auto s = current_state();
        return s == GuiSessionState::RUNNING || s == GuiSessionState::DEGRADED_USER_ONLY || s == GuiSessionState::DEGRADED_KERNEL_ONLY || s == GuiSessionState::STARTING;
    }

private:
    void session_worker_loop(GuiConfig config);

    HWND hwnd_{nullptr};
    std::atomic<GuiSessionState> state_{GuiSessionState::IDLE};
    std::atomic<bool> stop_worker_requested_{false};
    std::atomic<bool> is_enumerating_{false};
    std::atomic<bool> cancel_enumeration_{false};

    std::thread session_thread_;
    std::thread process_enum_thread_;
    std::mutex worker_mutex_;

    std::unique_ptr<GuiLogRedirector> log_redirector_;
    std::string target_process_name_;
    std::unique_ptr<TriggerEngine> active_trigger_engine_;
    std::mutex trigger_engine_mutex_;
};

} // namespace stuttometer::gui
