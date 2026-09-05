#include "stuttometer/privilege_utils.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cwctype>
#include <filesystem>

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

bool equals_case_insensitive(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
        return std::tolower(static_cast<unsigned char>(c1)) == 
               std::tolower(static_cast<unsigned char>(c2));
    });
}

bool matches_process_name(std::string_view actual, std::string_view target) noexcept {
    if (actual.empty() || target.empty()) return false;
    if (equals_case_insensitive(actual, target)) return true;
    if (target.size() >= 4 && equals_case_insensitive(target.substr(target.size() - 4), ".exe")) {
        return false;
    }
    char buf[270];
    if (target.size() + 4 > sizeof(buf)) return false;
    std::memcpy(buf, target.data(), target.size());
    std::memcpy(buf + target.size(), ".exe", 4);
    return equals_case_insensitive(actual, std::string_view(buf, target.size() + 4));
}

std::string utf16_to_utf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size_needed, nullptr, nullptr);
    return result;
}

static std::wstring utf8_to_utf16(const std::string& str) {
    if (str.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (size_needed <= 0) return {};
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size_needed);
    return result;
}

DriverSymbolResolver::DriverSymbolResolver() {
    refresh(true);
}

void DriverSymbolResolver::refresh(bool force) const {
    const uint64_t now_qpc = get_current_qpc();
    const uint64_t qpc_freq = get_qpc_frequency();
    if (!force) {
        const uint64_t last = last_refresh_qpc_.load(std::memory_order_relaxed);
        if (last > 0 && qpc_delta_to_ms(now_qpc - last, qpc_freq) < 5000.0) {
            return; // Rate-limit: avoid spamming EnumDeviceDrivers more than once per 5 seconds
        }
    }
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

    auto new_drivers = std::make_shared<std::vector<DriverEntry>>();
    new_drivers->reserve(drivers.size());
    wchar_t base_name[MAX_PATH];
    for (LPVOID drv : drivers) {
        if (drv && GetDeviceDriverBaseNameW(drv, base_name, static_cast<DWORD>(sizeof(base_name) / sizeof(wchar_t)))) {
            DriverEntry entry;
            entry.base_address = reinterpret_cast<uint64_t>(drv);
            entry.name = utf16_to_utf8(base_name);
            new_drivers->push_back(std::move(entry));
        }
    }

    // Sort descending by base address for binary search
    std::sort(new_drivers->begin(), new_drivers->end(), [](const DriverEntry& a, const DriverEntry& b) {
        return a.base_address > b.base_address;
    });

    drivers_.store(new_drivers, std::memory_order_release);
    last_refresh_qpc_.store(now_qpc, std::memory_order_relaxed);
}

std::string DriverSymbolResolver::resolve_driver_name(uint64_t routine_address) const {
    if (routine_address == 0) {
        return "unknown_module";
    }

    auto lookup_in_current = [routine_address](const std::vector<DriverEntry>& drivers) -> std::string {
        if (drivers.empty()) return {};
        auto it = std::lower_bound(
            drivers.begin(), drivers.end(), routine_address,
            [](const DriverEntry& entry, uint64_t addr) {
                return entry.base_address > addr;
            }
        );
        if (it != drivers.end()) {
            const size_t index = std::distance(drivers.begin(), it);
            constexpr uint64_t MAX_DRIVER_SPAN = 128ULL * 1024 * 1024; // 128 MB max driver text span
            const uint64_t upper_bound = (index > 0) ? drivers[index - 1].base_address : (it->base_address + MAX_DRIVER_SPAN);
            if (routine_address >= it->base_address && routine_address < upper_bound) {
                return it->name;
            }
        }
        return {};
    };

    auto current_drivers = drivers_.load(std::memory_order_acquire);
    if (current_drivers) {
        std::string name = lookup_in_current(*current_drivers);
        if (!name.empty()) {
            return name;
        }
    }

    // Cache miss / newly loaded driver: double-checked locking with single-threaded rate-limited refresh
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    current_drivers = drivers_.load(std::memory_order_acquire);
    if (current_drivers) {
        std::string name = lookup_in_current(*current_drivers);
        if (!name.empty()) {
            return name;
        }
    }

    const uint64_t now_qpc = get_current_qpc();
    const uint64_t qpc_freq = get_qpc_frequency();
    const uint64_t last = last_refresh_qpc_.load(std::memory_order_relaxed);
    if (last == 0 || qpc_delta_to_ms(now_qpc - last, qpc_freq) >= 3000.0) {
        refresh(true);
        current_drivers = drivers_.load(std::memory_order_acquire);
        if (current_drivers) {
            std::string name = lookup_in_current(*current_drivers);
            if (!name.empty()) {
                return name;
            }
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
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at).count() < 3) {
                return it->second.name;
            }
        }
    }

    std::string name = "PID_" + std::to_string(pid);
    HANDLE h_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h_proc) {
        wchar_t full_path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h_proc, 0, full_path, &size)) {
            std::wstring full_str(full_path);
            const size_t slash_pos = full_str.find_last_of(L"\\/");
            std::wstring exe_wstr = (slash_pos != std::wstring::npos) ? full_str.substr(slash_pos + 1) : full_str;
            name = utf16_to_utf8(exe_wstr);
        }
        CloseHandle(h_proc);
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache.size() >= 512) {
            // Evict stale entries older than 15s
            for (auto it = cache.begin(); it != cache.end(); ) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at).count() > 15) {
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }
            // If still full, prune the oldest 128 entries (25%) instead of nuking the entire cache
            if (cache.size() >= 512) {
                std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>> entries;
                entries.reserve(cache.size());
                for (const auto& [entry_pid, item] : cache) {
                    entries.emplace_back(entry_pid, item.cached_at);
                }
                std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                    return a.second < b.second;
                });
                for (size_t i = 0; i < 128 && i < entries.size(); ++i) {
                    cache.erase(entries[i].first);
                }
            }
        }
        cache[pid] = { name, now };
    }
    return name;
}

static bool contains_case_insensitive_w(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](wchar_t ch1, wchar_t ch2) {
            return std::towlower(ch1) == std::towlower(ch2);
        }
    );
    return (it != haystack.end());
}

struct SnapshotGuard {
    HANDLE h;
    ~SnapshotGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

struct ProcessResolveCacheEntry {
    uint32_t pid{0};
    std::chrono::steady_clock::time_point cached_at;
};

uint32_t resolve_process_name_to_pid(const std::string& process_name) {
    if (process_name.empty()) return 0;
    std::filesystem::path p(process_name);
    std::string clean_name = p.filename().string();
    if (clean_name.empty()) clean_name = process_name;

    static std::unordered_map<std::string, ProcessResolveCacheEntry> s_resolve_cache;
    static std::mutex s_resolve_mutex;

    const auto now = std::chrono::steady_clock::now();
    const std::wstring target_wname = utf8_to_utf16(clean_name);

    std::string cache_key = clean_name;
    std::transform(cache_key.begin(), cache_key.end(), cache_key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    {
        std::lock_guard<std::mutex> lock(s_resolve_mutex);
        auto it = s_resolve_cache.find(cache_key);
        if (it != s_resolve_cache.end()) {
            const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at).count();
            if (it->second.pid != 0) {
                HANDLE h_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->second.pid);
                if (h_proc) {
                    DWORD exit_code = 0;
                    if (GetExitCodeProcess(h_proc, &exit_code) && exit_code == STILL_ACTIVE) {
                        wchar_t full_path[MAX_PATH];
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(h_proc, 0, full_path, &size)) {
                            std::wstring path_str(full_path);
                            size_t slash = path_str.find_last_of(L"\\/");
                            std::wstring exe_name = (slash != std::wstring::npos) ? path_str.substr(slash + 1) : path_str;
                            if (matches_process_name(utf16_to_utf8(exe_name), clean_name)) {
                                it->second.cached_at = now;
                                CloseHandle(h_proc);
                                return it->second.pid;
                            }
                        }
                    }
                    CloseHandle(h_proc);
                }
            } else if (elapsed_sec < 3) {
                return 0; // Short negative cache (3s) before re-scanning for launch
            }
        }
    }

    HANDLE raw_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw_snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    SnapshotGuard snapshot_guard{ raw_snapshot };

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(raw_snapshot, &pe)) {
        return 0;
    }

    struct ProcessCandidate {
        DWORD pid;
        std::wstring exe_name;
    };
    std::vector<ProcessCandidate> candidates;

    uint32_t resolved_pid = 0;

    // Pass 1: Iterate snapshot and test exact/normalized match immediately
    do {
        if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;

        if (matches_process_name(utf16_to_utf8(pe.szExeFile), clean_name)) {
            resolved_pid = pe.th32ProcessID;
            break;
        }

        candidates.push_back({ pe.th32ProcessID, pe.szExeFile });
    } while (Process32NextW(raw_snapshot, &pe));

    const bool has_exe_ext = (clean_name.length() >= 4 && _stricmp(clean_name.c_str() + clean_name.length() - 4, ".exe") == 0);

    if (resolved_pid == 0 && !has_exe_ext) {
        // Pass 2: Prefix match ranking fallback on in-memory candidates (only when .exe was not explicitly specified)
        std::vector<ProcessCandidate> prefix_matches;
        for (const auto& cand : candidates) {
            if (_wcsnicmp(cand.exe_name.c_str(), target_wname.c_str(), target_wname.length()) == 0) {
                prefix_matches.push_back(cand);
            }
        }
        if (!prefix_matches.empty()) {
            std::sort(prefix_matches.begin(), prefix_matches.end(), [](const ProcessCandidate& a, const ProcessCandidate& b) {
                if (a.exe_name.length() != b.exe_name.length()) {
                    return a.exe_name.length() < b.exe_name.length(); // Prefer shortest name (e.g. Game.exe over GameLauncher.exe)
                }
                return a.pid < b.pid; // Deterministic total ordering: lowest PID
            });
            resolved_pid = prefix_matches.front().pid;
        }
    }

    if (resolved_pid == 0) {
        // Pass 3: General substring match fallback on in-memory candidates
        std::vector<ProcessCandidate> substr_matches;
        for (const auto& cand : candidates) {
            if (contains_case_insensitive_w(cand.exe_name, target_wname)) {
                substr_matches.push_back(cand);
            }
        }
        if (!substr_matches.empty()) {
            std::sort(substr_matches.begin(), substr_matches.end(), [](const ProcessCandidate& a, const ProcessCandidate& b) {
                if (a.exe_name.length() != b.exe_name.length()) {
                    return a.exe_name.length() < b.exe_name.length();
                }
                return a.pid < b.pid;
            });
            resolved_pid = substr_matches.front().pid;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_resolve_mutex);
        if (s_resolve_cache.size() >= 64) {
            for (auto it = s_resolve_cache.begin(); it != s_resolve_cache.end();) {
                if ((now - it->second.cached_at) > std::chrono::milliseconds(2000)) {
                    it = s_resolve_cache.erase(it);
                } else {
                    ++it;
                }
            }
            if (s_resolve_cache.size() >= 64) {
                std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> entries;
                entries.reserve(s_resolve_cache.size());
                for (const auto& [name_key, item] : s_resolve_cache) {
                    entries.emplace_back(name_key, item.cached_at);
                }
                std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                    return a.second < b.second;
                });
                for (size_t i = 0; i < 16 && i < entries.size(); ++i) {
                    s_resolve_cache.erase(entries[i].first);
                }
            }
        }
        s_resolve_cache[cache_key] = { resolved_pid, now };
    }

    return resolved_pid;
}

} // namespace stuttometer
