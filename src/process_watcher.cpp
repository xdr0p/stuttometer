#include "stuttometer/internal/process_watcher.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <cwctype>
#include <chrono>

namespace stuttometer {

bool ProcessWatcher::prepare_target(std::string_view target_process) {
    if (target_process.empty() || target_process.size() > 32768) {
        target_wide_.clear();
        target_wide_exe_.clear();
        target_wide_lower_.clear();
        target_has_exe_ = false;
        return false;
    }

    size_t slash = target_process.find_last_of("/\\:");
    std::string_view clean_name = (slash != std::string_view::npos) ? target_process.substr(slash + 1) : target_process;
    if (clean_name.empty()) {
        clean_name = target_process;
    }

    if (clean_name.empty() || clean_name.size() + 4 > 270) {
        target_wide_.clear();
        target_wide_exe_.clear();
        target_wide_lower_.clear();
        target_has_exe_ = false;
        return false;
    }

    int needed = MultiByteToWideChar(CP_UTF8, 0, clean_name.data(), static_cast<int>(clean_name.size()), nullptr, 0);
    if (needed <= 0) {
        target_wide_.clear();
        target_wide_exe_.clear();
        target_wide_lower_.clear();
        target_has_exe_ = false;
        return false;
    }

    target_wide_.resize(needed);
    MultiByteToWideChar(CP_UTF8, 0, clean_name.data(), static_cast<int>(clean_name.size()), target_wide_.data(), needed);

    if (target_wide_.length() >= 4 && _wcsicmp(target_wide_.c_str() + target_wide_.length() - 4, L".exe") == 0) {
        target_has_exe_ = true;
        target_wide_exe_ = target_wide_;
    } else {
        target_has_exe_ = false;
        target_wide_exe_ = target_wide_ + L".exe";
    }

    target_wide_lower_ = target_wide_;
    for (auto& ch : target_wide_lower_) {
        ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    }

    return true;
}

void ProcessWatcher::start(std::string target_process, WatcherCallbacks callbacks, bool exact_match_only) {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    stop_locked();
    if (!prepare_target(target_process)) {
        return;
    }

    target_process_ = std::move(target_process);
    exact_match_only_ = exact_match_only;
    attached_pid_.store(0, std::memory_order_release);
    stop_flag_.store(false, std::memory_order_release);
    is_running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&ProcessWatcher::worker_loop, this, callbacks);
}

bool ProcessWatcher::set_target_for_test(std::string_view target_process) {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    if (is_running_.load(std::memory_order_acquire)) {
        return false;
    }
    return prepare_target(target_process);
}

void ProcessWatcher::stop() {
    std::lock_guard<std::mutex> lock(watcher_mutex_);
    stop_locked();
}

void ProcessWatcher::stop_locked() {
    stop_flag_.store(true, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    attached_pid_.store(0, std::memory_order_release);
    is_running_.store(false, std::memory_order_release);
}

uint32_t ProcessWatcher::find_target_pid_snapshot() const noexcept {
    if (target_wide_.empty()) {
        return 0;
    }

    HANDLE raw_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw_snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    struct SnapshotGuard {
        HANDLE h;
        ~SnapshotGuard() {
            if (h && h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    } guard{raw_snapshot};

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(raw_snapshot, &pe)) {
        return 0;
    }

    uint32_t exact_pid = 0;

    // Pass 2: Prefix match (only when !target_has_exe_)
    uint32_t best_prefix_pid = 0;
    size_t best_prefix_len = static_cast<size_t>(-1);

    // Pass 3: Substring match (towlower scan)
    uint32_t best_substr_pid = 0;
    size_t best_substr_len = static_cast<size_t>(-1);

    do {
        if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) {
            continue;
        }

        const size_t exe_len = wcsnlen(pe.szExeFile, MAX_PATH);

        // Pass 1: Exact Match
        if (_wcsicmp(pe.szExeFile, target_wide_.c_str()) == 0 ||
            (!target_has_exe_ && _wcsicmp(pe.szExeFile, target_wide_exe_.c_str()) == 0)) {
            exact_pid = pe.th32ProcessID;
            break;
        }

        if (exact_match_only_) {
            continue; // Skip prefix (Pass 2) and expensive towlower scan (Pass 3)
        }

        // Pass 2: Prefix match (only when !target_has_exe_)
        if (!target_has_exe_) {
            if (_wcsnicmp(pe.szExeFile, target_wide_.c_str(), target_wide_.length()) == 0) {
                if (exe_len < best_prefix_len || (exe_len == best_prefix_len && pe.th32ProcessID < best_prefix_pid)) {
                    best_prefix_len = exe_len;
                    best_prefix_pid = pe.th32ProcessID;
                }
            }
        }

        // Pass 3: Substring match (zero allocation)
        const size_t target_len = target_wide_lower_.length();
        if (target_len <= exe_len) {
            for (size_t i = 0; i <= exe_len - target_len; ++i) {
                bool match = true;
                for (size_t j = 0; j < target_len; ++j) {
                    if (std::towlower(static_cast<wint_t>(pe.szExeFile[i + j])) != 
                        static_cast<wint_t>(target_wide_lower_[j])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    if (exe_len < best_substr_len || (exe_len == best_substr_len && pe.th32ProcessID < best_substr_pid)) {
                        best_substr_len = exe_len;
                        best_substr_pid = pe.th32ProcessID;
                    }
                    break;
                }
            }
        }
    } while (Process32NextW(raw_snapshot, &pe));

    if (exact_pid != 0) return exact_pid;
    if (exact_match_only_) return 0;
    if (best_prefix_pid != 0) return best_prefix_pid;
    if (best_substr_pid != 0) return best_substr_pid;
    return 0;
}

void ProcessWatcher::worker_loop(WatcherCallbacks callbacks) {
    const std::string_view name_view{target_process_};

    // Subtlety note: If ETW wins the attach race (on_process_launched -> try_attach_pid),
    // TriggerEngine state is already attached. On the next watcher poll (up to 250ms later),
    // idempotent try_attach(found_pid) returns true, allowing the watcher to record attached_pid_,
    // fire on_attach_success, and notify the GUI within the 250ms window.
    // Edge case note: If a target launches and terminates within a single 250ms watcher interval,
    // ETW handles both transitions in the engine, but the GUI never sees the transient process.
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        poll_count_for_test_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t found_pid = find_target_pid_snapshot();
        const uint32_t current_attached = attached_pid_.load(std::memory_order_relaxed);

        if (found_pid != 0) {
            // Case 2c: Process restarted with a different PID; detach old PID first
            if (current_attached != 0 && current_attached != found_pid) {
                if (callbacks.try_detach && callbacks.try_detach(current_attached, callbacks.user_data)) {
                    if (callbacks.on_detach_success) {
                        callbacks.on_detach_success(current_attached, name_view, callbacks.user_data);
                    }
                }
                attached_pid_.store(0, std::memory_order_release);
            }

            // Case 2a: Initial attach or re-attach after restart
            if (attached_pid_.load(std::memory_order_relaxed) == 0) {
                if (callbacks.try_attach && callbacks.try_attach(found_pid, callbacks.user_data)) {
                    attached_pid_.store(found_pid, std::memory_order_release);
                    if (callbacks.on_attach_success) {
                        callbacks.on_attach_success(found_pid, name_view, callbacks.user_data);
                    }
                }
            }
            // Case 2b: Steady state (current_attached == found_pid) -> no-op
        } else {
            // Case 3a: Target just exited (found_pid == 0 but was attached)
            if (current_attached != 0) {
                if (callbacks.try_detach && callbacks.try_detach(current_attached, callbacks.user_data)) {
                    if (callbacks.on_detach_success) {
                        callbacks.on_detach_success(current_attached, name_view, callbacks.user_data);
                    }
                }
                attached_pid_.store(0, std::memory_order_release);
            }
            // Case 3b: Steady state waiting (found_pid == 0 and attached_pid_ == 0) -> no-op
        }

        // Poll 250ms (5 * 50ms) while waiting for target to launch; poll ~2s once attached
        const bool is_waiting = callbacks.is_waiting ? callbacks.is_waiting(callbacks.user_data) : false;
        const int sleep_steps = is_waiting ? 5 : 40;
        for (int i = 0; i < sleep_steps && !stop_flag_.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace stuttometer
