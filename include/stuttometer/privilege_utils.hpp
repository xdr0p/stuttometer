#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace stuttometer {

// Check if current process has Administrator elevation
bool is_running_as_admin();

// Check and enable SeSystemprofilePrivilege for kernel ETW tracing
bool enable_system_profile_privilege();

// High-resolution QPC helpers
uint64_t get_qpc_frequency();
uint64_t get_current_qpc();
double qpc_delta_to_ms(uint64_t delta_qpc, uint64_t qpc_freq);
double qpc_delta_to_us(uint64_t delta_qpc, uint64_t qpc_freq);
uint64_t ms_to_qpc_delta(double ms, uint64_t qpc_freq);

// Kernel driver address to module name resolver (e.g. nvlddmkm.sys, ndis.sys)
class DriverSymbolResolver {
public:
    DriverSymbolResolver();
    ~DriverSymbolResolver() = default;

    void refresh();
    std::string resolve_driver_name(uint64_t routine_address) const;

private:
    struct DriverEntry {
        uint64_t base_address{0};
        std::string name;
    };
    std::vector<DriverEntry> drivers_;
};

// Process ID to process name resolver (used in background/reporting)
std::string get_process_name_by_pid(uint32_t pid);

// Process name to Process ID resolver (used at startup/background watcher)
uint32_t resolve_process_name_to_pid(const std::string& process_name);

} // namespace stuttometer
