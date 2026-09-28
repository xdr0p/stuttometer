#pragma once

#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace stuttometer {

struct WatcherCallbacks {
    // Query interface (allows ProcessWatcher to interact with TriggerEngine or GUI state without allocations)
    bool     (*try_attach)(uint32_t pid, void* user_data) = nullptr;
    bool     (*try_detach)(uint32_t pid, void* user_data) = nullptr;
    bool     (*is_waiting)(void* user_data) = nullptr;
    uint32_t (*get_active_pid)(void* user_data) = nullptr;

    // Notification callbacks (for logging)
    // Note: 'name' is a view into target_process_ which persists for the watcher's lifetime.
    void     (*on_attach_success)(uint32_t pid, std::string_view name, void* user_data) = nullptr;
    void     (*on_detach_success)(uint32_t pid, std::string_view name, void* user_data) = nullptr;

    void*    user_data = nullptr;
};

class ProcessWatcher {
public:
    ProcessWatcher() = default;
    ~ProcessWatcher() { stop(); }

    ProcessWatcher(const ProcessWatcher&) = delete;
    ProcessWatcher& operator=(const ProcessWatcher&) = delete;
    ProcessWatcher(ProcessWatcher&&) = delete;
    ProcessWatcher& operator=(ProcessWatcher&&) = delete;

    void start(std::string target_process, WatcherCallbacks callbacks);
    void stop();
    bool is_running() const noexcept { return is_running_.load(std::memory_order_acquire); }

private:
    void worker_loop(WatcherCallbacks callbacks);
    void stop_locked();

    std::mutex watcher_mutex_;
    std::string target_process_;
    std::thread worker_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> is_running_{false};
};

} // namespace stuttometer
