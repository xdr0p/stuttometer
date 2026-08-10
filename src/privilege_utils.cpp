#include "stuttometer/privilege_utils.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace stuttometer {

bool is_running_as_admin() {
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
            &nt_authority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    return is_admin != FALSE;
}

bool enable_system_profile_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_PROFILE_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);

    return (ok && err == ERROR_SUCCESS);
}

uint64_t get_qpc_frequency() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<uint64_t>(freq.QuadPart);
}

uint64_t get_current_qpc() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

double qpc_delta_to_ms(uint64_t delta_qpc, uint64_t qpc_freq) {
    if (qpc_freq == 0) return 0.0;
    return (static_cast<double>(delta_qpc) * 1000.0) / static_cast<double>(qpc_freq);
}

double qpc_delta_to_us(uint64_t delta_qpc, uint64_t qpc_freq) {
    if (qpc_freq == 0) return 0.0;
    return (static_cast<double>(delta_qpc) * 1000000.0) / static_cast<double>(qpc_freq);
}

uint64_t ms_to_qpc_delta(double ms, uint64_t qpc_freq) {
    return static_cast<uint64_t>((ms * static_cast<double>(qpc_freq)) / 1000.0);
}

DriverSymbolResolver::DriverSymbolResolver() {
    refresh();
}

void DriverSymbolResolver::refresh() {
    drivers_.clear();
    LPVOID drivers[1024];
    DWORD cb_needed = 0;

    if (EnumDeviceDrivers(drivers, sizeof(drivers), &cb_needed) && cb_needed < sizeof(drivers)) {
        const int num_drivers = cb_needed / sizeof(LPVOID);
        drivers_.reserve(num_drivers);

        char base_name[MAX_PATH];
        for (int i = 0; i < num_drivers; ++i) {
            if (GetDeviceDriverBaseNameA(drivers[i], base_name, sizeof(base_name))) {
                DriverEntry entry;
                entry.base_address = reinterpret_cast<uint64_t>(drivers[i]);
                entry.name = base_name;
                drivers_.push_back(std::move(entry));
            }
        }

        // Sort descending by base address for binary search / lower_bound
        std::sort(drivers_.begin(), drivers_.end(), [](const DriverEntry& a, const DriverEntry& b) {
            return a.base_address > b.base_address;
        });
    }
}

std::string DriverSymbolResolver::resolve_driver_name(uint64_t routine_address) const {
    if (drivers_.empty() || routine_address == 0) {
        return "unknown_module";
    }

    // Since drivers_ is sorted descending by base_address, find first driver with base <= address
    for (const auto& driver : drivers_) {
        if (routine_address >= driver.base_address) {
            return driver.name;
        }
    }
    return "unknown_kernel_address";
}

std::string get_process_name_by_pid(uint32_t pid) {
    if (pid == 0) return "System Idle";
    if (pid == 4) return "System";

    static std::unordered_map<uint32_t, std::string> cache;
    static std::mutex cache_mutex;

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(pid);
        if (it != cache.end()) {
            return it->second;
        }
    }

    std::string name = "Process_" + std::to_string(pid);
    HANDLE h_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h_proc) {
        char full_path[MAX_PATH];
        DWORD size = sizeof(full_path);
        if (QueryFullProcessImageNameA(h_proc, 0, full_path, &size)) {
            std::string full_str(full_path);
            const size_t slash_pos = full_str.find_last_of("\\/");
            if (slash_pos != std::string::npos) {
                name = full_str.substr(slash_pos + 1);
            } else {
                name = full_str;
            }
        }
        CloseHandle(h_proc);
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[pid] = name;
    }
    return name;
}

} // namespace stuttometer
