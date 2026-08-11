#include "stuttometer/privilege_utils.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <chrono>

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

typedef LONG(WINAPI* RtlGetVersionPtr)(OSVERSIONINFOEXW*);

bool is_supported_windows_build() {
    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!h_ntdll) return true;

    auto p_fn = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(h_ntdll, "RtlGetVersion"));
    if (!p_fn) return true;

    OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (p_fn(&vi) == 0) { // STATUS_SUCCESS (0)
        // Windows 10/11 build >= 19041 (20H1 and newer)
        return (vi.dwMajorVersion > 10) || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 19041);
    }
    return true;
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
    DWORD cb_needed = 0;
    std::vector<LPVOID> drivers(1024);

    while (true) {
        if (!EnumDeviceDrivers(drivers.data(), static_cast<DWORD>(drivers.size() * sizeof(LPVOID)), &cb_needed)) {
            return;
        }

        const size_t num_drivers = cb_needed / sizeof(LPVOID);
        if (num_drivers <= drivers.size()) {
            drivers.resize(num_drivers);
            break;
        }
        drivers.resize(num_drivers * 2);
    }

    drivers_.reserve(drivers.size());
    char base_name[MAX_PATH];
    for (LPVOID drv : drivers) {
        if (drv && GetDeviceDriverBaseNameA(drv, base_name, sizeof(base_name))) {
            DriverEntry entry;
            entry.base_address = reinterpret_cast<uint64_t>(drv);
            entry.name = base_name;
            drivers_.push_back(std::move(entry));
        }
    }

    // Sort descending by base address for binary search
    std::sort(drivers_.begin(), drivers_.end(), [](const DriverEntry& a, const DriverEntry& b) {
        return a.base_address > b.base_address;
    });
}

std::string DriverSymbolResolver::resolve_driver_name(uint64_t routine_address) const {
    if (drivers_.empty() || routine_address == 0) {
        return "unknown_module";
    }

    // Binary search (lower_bound with greater comparator on descending list)
    auto it = std::lower_bound(
        drivers_.begin(), drivers_.end(), routine_address,
        [](const DriverEntry& entry, uint64_t addr) {
            return entry.base_address > addr;
        }
    );

    if (it != drivers_.end()) {
        const size_t index = std::distance(drivers_.begin(), it);
        const uint64_t upper_bound = (index > 0) ? drivers_[index - 1].base_address : UINT64_MAX;

        // Relative upper bound validation
        if (routine_address >= it->base_address && routine_address < upper_bound) {
            return it->name;
        }
    }
    return "unknown_kernel_address";
}

struct CachedProcessName {
    std::string name;
    std::chrono::steady_clock::time_point cached_at;
};

std::string get_process_name_by_pid(uint32_t pid) {
    if (pid == 0) return "System Idle";
    if (pid == 4) return "System";

    static std::unordered_map<uint32_t, CachedProcessName> cache;
    static std::mutex cache_mutex;

    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(pid);
        if (it != cache.end()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at).count() < 5) {
                return it->second.name;
            }
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
        cache[pid] = { name, now };
    }
    return name;
}

uint32_t resolve_process_name_to_pid(const std::string& process_name) {
    if (process_name.empty()) return 0;

    DWORD pids[2048];
    DWORD cb_needed = 0;

    if (!EnumProcesses(pids, sizeof(pids), &cb_needed)) {
        return 0;
    }

    const DWORD count = cb_needed / sizeof(DWORD);
    char full_path[MAX_PATH];

    for (DWORD i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0 || pid == 4) continue;

        HANDLE h_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h_proc) {
            DWORD size = sizeof(full_path);
            if (QueryFullProcessImageNameA(h_proc, 0, full_path, &size)) {
                std::string full_str(full_path);
                const size_t slash_pos = full_str.find_last_of("\\/");
                const std::string exe_name = (slash_pos != std::string::npos) ? full_str.substr(slash_pos + 1) : full_str;

                if (_stricmp(exe_name.c_str(), process_name.c_str()) == 0 ||
                    exe_name.find(process_name) != std::string::npos) {
                    CloseHandle(h_proc);
                    return pid;
                }
            }
            CloseHandle(h_proc);
        }
    }
    return 0;
}

} // namespace stuttometer
