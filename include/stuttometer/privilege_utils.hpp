#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

namespace stuttometer {

// Check if current process has Administrator elevation
bool is_running_as_admin();

// Check and enable SeSystemprofilePrivilege for kernel ETW tracing
bool enable_system_profile_privilege();

// Validate Windows OS version is Windows 10/11 x64 (build >= 19041)
bool is_supported_windows_build();

// High-resolution QPC helpers
uint64_t get_qpc_frequency();
uint64_t get_current_qpc();
double qpc_delta_to_ms(uint64_t delta_qpc, uint64_t qpc_freq);
double qpc_delta_to_us(uint64_t delta_qpc, uint64_t qpc_freq);
uint64_t ms_to_qpc_delta(double ms, uint64_t qpc_freq);

// Kernel driver address to module name resolver
class DriverSymbolResolver {
public:
    struct DriverEntry {
        uint64_t base_address{0};
        std::string name;
    };

    DriverSymbolResolver();
    ~DriverSymbolResolver() = default;

    void refresh(bool force = false) const;
    std::string resolve_driver_name(uint64_t routine_address) const;

private:
    mutable std::mutex refresh_mutex_;
    mutable std::atomic<std::shared_ptr<const std::vector<DriverEntry>>> drivers_{nullptr};
    mutable std::atomic<uint64_t> last_refresh_qpc_{0};
};

// String helpers
bool equals_case_insensitive(std::string_view a, std::string_view b) noexcept;
bool matches_process_name(std::string_view actual, std::string_view target) noexcept;
std::string utf16_to_utf8(std::wstring_view wstr);

// Process ID to process name resolver with TTL cache
std::string get_process_name_by_pid(uint32_t pid);

// Process name to Process ID resolver
uint32_t resolve_process_name_to_pid(const std::string& process_name);

} // namespace stuttometer
