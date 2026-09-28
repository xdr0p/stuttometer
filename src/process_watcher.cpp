#include "stuttometer/internal/process_watcher.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <chrono>

namespace stuttometer {

void ProcessWatcher::start(std::string target_process, WatcherCallbacks callbacks) {
    stop();
    if (target_process.empty()) {
        return;
    }

    target_process_ = std::move(target_process);
    stop_flag_.store(false, std::memory_order_release);
    is_running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&ProcessWatcher::worker_loop, this, callbacks);
}

void ProcessWatcher::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    is_running_.store(false, std::memory_order_release);
}

void ProcessWatcher::worker_loop(WatcherCallbacks callbacks) {
    const std::string_view name_view{target_process_};

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const uint32_t found_pid = resolve_process_name_to_pid(target_process_);
        if (found_pid != 0) {
            if (callbacks.try_attach && callbacks.try_attach(found_pid, callbacks.user_data)) {
                if (callbacks.on_attach_success) {
                    callbacks.on_attach_success(found_pid, name_view, callbacks.user_data);
                }
            }
        } else {
            const uint32_t active = callbacks.get_active_pid ? callbacks.get_active_pid(callbacks.user_data) : 0;
            if (active != 0 && callbacks.try_detach && callbacks.try_detach(active, callbacks.user_data)) {
                if (callbacks.on_detach_success) {
                    callbacks.on_detach_success(active, name_view, callbacks.user_data);
                }
            }
        }

        // Poll responsive 50ms while waiting for target to launch; poll ~2s once attached
        const bool is_waiting = callbacks.is_waiting ? callbacks.is_waiting(callbacks.user_data) : false;
        const int sleep_steps = is_waiting ? 1 : 40;
        for (int i = 0; i < sleep_steps && !stop_flag_.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace stuttometer
