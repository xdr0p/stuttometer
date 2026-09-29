#include "test_common.hpp"
#include "stuttometer/cli_parser.hpp"
#include "stuttometer/version.hpp"
#include "stuttometer/internal/process_watcher.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <windows.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <thread>
#include <algorithm>
#include <new>
#include <cstdlib>

static thread_local bool g_disallow_allocations = false;
static thread_local bool g_allocation_detected = false;

void* operator new(size_t size) {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

void* operator new[](size_t size) {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    std::free(p);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
    return std::malloc(size);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
    return std::malloc(size);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

#if defined(__cpp_aligned_new)
void* operator new(size_t size, std::align_val_t al) {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
#if defined(_WIN32)
    void* p = _aligned_malloc(size, static_cast<size_t>(al));
#else
    void* p = std::aligned_alloc(static_cast<size_t>(al), size);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete(void* p, size_t, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* operator new[](size_t size, std::align_val_t al) {
    if (g_disallow_allocations) {
        g_allocation_detected = true;
    }
#if defined(_WIN32)
    void* p = _aligned_malloc(size, static_cast<size_t>(al));
#else
    void* p = std::aligned_alloc(static_cast<size_t>(al), size);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, size_t, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
#endif

using namespace stuttometer;

static void test_pacing_profile_parsing() {
    std::cout << "[TEST] Testing --pacing-profile parsing...\n";

    {
        const char* argv[] = { "stuttometer.exe", "--pacing-profile", "auto" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.pacing_profile == PacingProfile::AUTO_ADAPTIVE);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--pacing-profile", "high-refresh" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.pacing_profile == PacingProfile::HIGH_REFRESH);
        STUTTO_ASSERT(config.spike_multiplier == HIGH_REFRESH_SPIKE_MULTIPLIER);
        STUTTO_ASSERT(config.min_spike_delta_ms == HIGH_REFRESH_MIN_DELTA_MS);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--pacing-profile", "conservative" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.pacing_profile == PacingProfile::CONSERVATIVE);
        STUTTO_ASSERT(config.spike_multiplier == CONSERVATIVE_SPIKE_MULTIPLIER);
        STUTTO_ASSERT(config.min_spike_delta_ms == CONSERVATIVE_MIN_DELTA_MS);
    }

    std::cout << "  -> --pacing-profile parsing passed.\n";
}

static void test_rejection_of_invalid_profiles() {
    std::cout << "[TEST] Testing rejection of invalid profile names...\n";

    {
        const char* argv[] = { "stuttometer.exe", "--pacing-profile", "custom" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("Invalid --pacing-profile") != std::string::npos);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--pacing-profile", "invalid" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("Invalid --pacing-profile") != std::string::npos);
    }

    std::cout << "  -> Invalid profile names rejected.\n";
}

static void test_high_refresh_alias() {
    std::cout << "[TEST] Testing --high-refresh alias resolution...\n";

    const char* argv[] = { "stuttometer.exe", "--high-refresh" };
    CliConfig config;
    std::ostringstream out, err;
    auto res = parse_cli_args(2, argv, config, out, err);
    STUTTO_ASSERT(res == CliParseResult::SUCCESS);
    STUTTO_ASSERT(config.pacing_profile == PacingProfile::HIGH_REFRESH);
    STUTTO_ASSERT(config.spike_multiplier == HIGH_REFRESH_SPIKE_MULTIPLIER);
    STUTTO_ASSERT(config.min_spike_delta_ms == HIGH_REFRESH_MIN_DELTA_MS);

    std::cout << "  -> --high-refresh alias resolved.\n";
}

static void test_pacing_profile_precedence_over_alias() {
    std::cout << "[TEST] Testing --pacing-profile taking precedence over --high-refresh (NM3)...\n";

    const char* argv[] = { "stuttometer.exe", "--pacing-profile", "auto", "--high-refresh" };
    CliConfig config;
    std::ostringstream out, err;
    auto res = parse_cli_args(4, argv, config, out, err);
    STUTTO_ASSERT(res == CliParseResult::SUCCESS);
    STUTTO_ASSERT(config.pacing_profile == PacingProfile::AUTO_ADAPTIVE);
    STUTTO_ASSERT(err.str().find("[Config] Note: --pacing-profile took precedence over --high-refresh") != std::string::npos);

    std::cout << "  -> Precedence over alias verified.\n";
}

static void test_precedence_explicit_override() {
    std::cout << "[TEST] Testing explicit parameter overrides...\n";

    {
        // --high-refresh --spike-multiplier 3.0 -> CUSTOM with 3.0 / 4.0
        const char* argv[] = { "stuttometer.exe", "--high-refresh", "--spike-multiplier", "3.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(4, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.pacing_profile == PacingProfile::CUSTOM);
        STUTTO_ASSERT(config.spike_multiplier == 3.0);
        STUTTO_ASSERT(config.min_spike_delta_ms == 4.0);
        STUTTO_ASSERT(err.str().find("[Config] Note: Explicit parameter override active; operating in CUSTOM profile.") != std::string::npos);
    }

    {
        // Implicit custom: --spike-multiplier 3.0 without --pacing-profile
        const char* argv[] = { "stuttometer.exe", "--spike-multiplier", "3.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.pacing_profile == PacingProfile::CUSTOM);
        STUTTO_ASSERT(config.spike_multiplier == 3.0);
        STUTTO_ASSERT(config.min_spike_delta_ms == 4.0);
    }

    std::cout << "  -> Explicit parameter overrides verified.\n";
}

static void test_version_and_self_check() {
    std::cout << "[TEST] Testing --version vs --self-check...\n";

    {
        // --version beats --self-check and returns EXIT_SUCCESS
        const char* argv[] = { "stuttometer.exe", "--version", "--self-check" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_OK);
        std::string expected_banner = "Stuttometer v" + std::string(stuttometer::TOOL_VERSION);
        STUTTO_ASSERT(out.str().find(expected_banner) != std::string::npos);
    }

    {
        // --dump-events - combined with --version returns EXIT_FAILURE
        const char* argv[] = { "stuttometer.exe", "--dump-events", "-", "--version" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(4, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("--dump-events - cannot be combined with --version") != std::string::npos);
    }

    {
        // --dump-events - combined with --self-check returns SUCCESS with config.run_self_check == true, dump_events_path == "-", no out text
        const char* argv[] = { "stuttometer.exe", "--dump-events", "-", "--self-check" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(4, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.run_self_check == true);
        STUTTO_ASSERT(config.dump_events_path == "-");
        STUTTO_ASSERT(out.str().empty());
    }

    std::cout << "  -> --version and --self-check verified.\n";
}

static void test_range_validations() {
    std::cout << "[TEST] Testing range validations...\n";

    {
        const char* argv[] = { "stuttometer.exe", "--window-ms", "10" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("--window-ms must be between") != std::string::npos);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--present-threshold-ms", "1.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("--present-threshold-ms must be between") != std::string::npos);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--spike-multiplier", "0.5" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
        STUTTO_ASSERT(err.str().find("--spike-multiplier must be between") != std::string::npos);
    }

    std::cout << "  -> Range validations verified.\n";
}

static bool is_integer_option(std::string_view name) {
    return name == "--dpc-threshold-us" ||
           name == "--isr-threshold-us" ||
           name == "--disk-threshold-ms" ||
           name == "--cswitch-threshold-ms" ||
           name == "--d3d12-pso-threshold-ms" ||
           name == "--vram-threshold-mb" ||
           name == "--mem-alloc-threshold-mb" ||
           name == "--mem-trim-threshold-mb" ||
           name == "--mem-physical-latency-us" ||
           name == "--buffer-slots";
}

static std::string format_cli_val(double val, bool is_int) {
    if (is_int) {
        return std::to_string(static_cast<int64_t>(val));
    }
    std::ostringstream oss;
    oss << val;
    return oss.str();
}

static double get_cli_eps(std::string_view name) {
    if (is_integer_option(name)) return 1.0;
    if (name == "--judder-swing-ratio") return 0.05;
    if (name == "--spike-multiplier" || name == "--min-spike-delta-ms" || name == "--present-threshold-ms") return 0.1;
    if (name == "--smi-threshold-ms") return 0.5;
    return 1.0;
}

static void test_cli_ranges_table() {
    std::cout << "[TEST] Testing CLI_RANGES comprehensive boundary and range validation...\n";

    for (const auto& r : CLI_RANGES) {
        const bool is_int = is_integer_option(r.name);
        const double eps = get_cli_eps(r.name);

        // 1. Direct unit checks on validate_option_range
        {
            std::ostringstream err;
            STUTTO_ASSERT(validate_option_range(err, r.name, r.min_val, r.min_val, r.max_val, r.unit));
            STUTTO_ASSERT(err.str().empty());
        }
        {
            std::ostringstream err;
            STUTTO_ASSERT(validate_option_range(err, r.name, r.max_val, r.min_val, r.max_val, r.unit));
            STUTTO_ASSERT(err.str().empty());
        }
        {
            std::ostringstream err;
            STUTTO_ASSERT(!validate_option_range(err, r.name, r.min_val - eps, r.min_val, r.max_val, r.unit));
            std::string err_str = err.str();
            STUTTO_ASSERT(err_str.find(r.name) != std::string::npos);
            STUTTO_ASSERT(err_str.find("must be between") != std::string::npos);
        }
        {
            std::ostringstream err;
            STUTTO_ASSERT(!validate_option_range(err, r.name, r.max_val + eps, r.min_val, r.max_val, r.unit));
            std::string err_str = err.str();
            STUTTO_ASSERT(err_str.find(r.name) != std::string::npos);
            STUTTO_ASSERT(err_str.find("must be between") != std::string::npos);
        }

        // 2. Integration via parse_cli_args
        const std::string min_str = format_cli_val(r.min_val, is_int);
        const std::string max_str = format_cli_val(r.max_val, is_int);
        const std::string below_str = format_cli_val(r.min_val - eps, is_int);
        const std::string above_str = format_cli_val(r.max_val + eps, is_int);

        // Min bound CLI test
        {
            const char* argv[] = { "stuttometer.exe", r.name, min_str.c_str() };
            CliConfig config;
            std::ostringstream out, err;
            auto res = parse_cli_args(3, argv, config, out, err);
            STUTTO_ASSERT(res == CliParseResult::SUCCESS);
            STUTTO_ASSERT(std::abs(r.getter(config) - r.min_val) < 0.001);
        }

        // Max bound CLI test
        {
            const char* argv[] = { "stuttometer.exe", r.name, max_str.c_str() };
            CliConfig config;
            std::ostringstream out, err;
            auto res = parse_cli_args(3, argv, config, out, err);
            STUTTO_ASSERT(res == CliParseResult::SUCCESS);
            STUTTO_ASSERT(std::abs(r.getter(config) - r.max_val) < 0.001);
        }

        // Below min CLI test
        {
            const char* argv[] = { "stuttometer.exe", r.name, below_str.c_str() };
            CliConfig config;
            std::ostringstream out, err;
            auto res = parse_cli_args(3, argv, config, out, err);
            STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
            STUTTO_ASSERT(err.str().find(r.name) != std::string::npos || err.str().find("must be between") != std::string::npos);
        }

        // Above max CLI test
        {
            const char* argv[] = { "stuttometer.exe", r.name, above_str.c_str() };
            CliConfig config;
            std::ostringstream out, err;
            auto res = parse_cli_args(3, argv, config, out, err);
            STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
            STUTTO_ASSERT(err.str().find(r.name) != std::string::npos);
            STUTTO_ASSERT(err.str().find("must be between") != std::string::npos);
        }
    }

    std::cout << "  -> All 18 CLI_RANGES bounds and boundary violations verified successfully.\n";
}

static void test_help_flags() {
    std::cout << "[TEST] Testing -h and --help flags (NB4)...\n";

    {
        const char* argv[] = { "stuttometer.exe", "-h" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(2, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_OK);
        STUTTO_ASSERT(out.str().find("Stuttometer") != std::string::npos);
        STUTTO_ASSERT(out.str().find("--pacing-profile") != std::string::npos);
    }

    {
        const char* argv[] = { "stuttometer.exe", "--help" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(2, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_OK);
        STUTTO_ASSERT(out.str().find("Stuttometer") != std::string::npos);
        STUTTO_ASSERT(out.str().find("--pacing-profile") != std::string::npos);
    }

    std::cout << "  -> Help flags verified.\n";
}

static void test_manual_threshold_flags() {
    std::cout << "[TEST] Testing manual threshold tracking flags...\n";

    // 1. Defaults: neither flag present
    {
        const char* argv[] = { "stuttometer.exe" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(1, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(!config.present_threshold_manual);
        STUTTO_ASSERT(!config.smi_threshold_manual);
        STUTTO_ASSERT(config.present_threshold_ms == 16.67);
        STUTTO_ASSERT(config.smi_severity_threshold_ms == 33.3);
    }

    // 2. Explicit --present-threshold-ms
    {
        const char* argv[] = { "stuttometer.exe", "--present-threshold-ms", "20.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.present_threshold_manual);
        STUTTO_ASSERT(!config.smi_threshold_manual);
        STUTTO_ASSERT(config.present_threshold_ms == 20.0);
    }

    // 3. Explicit --smi-threshold-ms
    {
        const char* argv[] = { "stuttometer.exe", "--smi-threshold-ms", "45.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(!config.present_threshold_manual);
        STUTTO_ASSERT(config.smi_threshold_manual);
        STUTTO_ASSERT(config.smi_severity_threshold_ms == 45.0);
    }

    // 4. Both flags together
    {
        const char* argv[] = { "stuttometer.exe", "--present-threshold-ms", "8.33", "--smi-threshold-ms", "25.0" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(5, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.present_threshold_manual);
        STUTTO_ASSERT(config.smi_threshold_manual);
        STUTTO_ASSERT(config.present_threshold_ms == 8.33);
        STUTTO_ASSERT(config.smi_severity_threshold_ms == 25.0);
    }

    std::cout << "  -> Manual threshold tracking flags verified.\n";
}

static void test_min_report_severity_flag() {
    std::cout << "[TEST] Testing --min-report-severity flag...\n";

    // 1. Default (no flag passed)
    {
        const char* argv[] = { "stuttometer.exe" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(1, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(!config.min_report_severity_manual);
        STUTTO_ASSERT(config.min_report_severity == ReportSeverity::ALL);
    }

    // 2. Explicit 'all'
    {
        const char* argv[] = { "stuttometer.exe", "--min-report-severity", "all" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.min_report_severity_manual);
        STUTTO_ASSERT(config.min_report_severity == ReportSeverity::ALL);
    }

    // 3. Explicit 'warning'
    {
        const char* argv[] = { "stuttometer.exe", "--min-report-severity", "warning" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.min_report_severity_manual);
        STUTTO_ASSERT(config.min_report_severity == ReportSeverity::WARNING);
    }

    // 4. Explicit 'danger'
    {
        const char* argv[] = { "stuttometer.exe", "--min-report-severity", "danger" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::SUCCESS);
        STUTTO_ASSERT(config.min_report_severity_manual);
        STUTTO_ASSERT(config.min_report_severity == ReportSeverity::DANGER);
    }

    // 5. Invalid severity value
    {
        const char* argv[] = { "stuttometer.exe", "--min-report-severity", "ultra" };
        CliConfig config;
        std::ostringstream out, err;
        auto res = parse_cli_args(3, argv, config, out, err);
        STUTTO_ASSERT(res == CliParseResult::EXIT_ERROR);
    }

    std::cout << "  -> --min-report-severity flag parsing verified.\n";
}

static void test_process_watcher_lifecycle() {
    std::cout << "[TEST] Testing ProcessWatcher lifecycle and query interface...\n";

    struct MockState {
        uint32_t active_pid = 0;
        bool waiting = true;
        std::vector<std::string> log;
    } mock;

    stuttometer::WatcherCallbacks cb;
    cb.user_data = &mock;
    cb.try_attach = [](uint32_t pid, void* ud) -> bool {
        auto* m = static_cast<MockState*>(ud);
        if (m->active_pid == 0 && m->waiting) {
            m->active_pid = pid;
            m->waiting = false;
            return true;
        }
        return false;
    };
    cb.try_detach = [](uint32_t pid, void* ud) -> bool {
        auto* m = static_cast<MockState*>(ud);
        if (m->active_pid == pid) {
            m->active_pid = 0;
            m->waiting = true;
            return true;
        }
        return false;
    };
    cb.is_waiting = [](void* ud) -> bool {
        return static_cast<MockState*>(ud)->waiting;
    };
    cb.get_active_pid = [](void* ud) -> uint32_t {
        return static_cast<MockState*>(ud)->active_pid;
    };
    cb.on_attach_success = [](uint32_t pid, std::string_view name, void* ud) {
        auto* m = static_cast<MockState*>(ud);
        m->log.push_back("ATTACH:" + std::string(name) + ":" + std::to_string(pid));
    };
    cb.on_detach_success = [](uint32_t pid, std::string_view name, void* ud) {
        auto* m = static_cast<MockState*>(ud);
        m->log.push_back("DETACH:" + std::string(name) + ":" + std::to_string(pid));
    };

    stuttometer::ProcessWatcher watcher;
    STUTTO_ASSERT(!watcher.is_running());

    // 1. Starting with empty target should not launch thread
    watcher.start("", cb);
    STUTTO_ASSERT(!watcher.is_running());

    // 2. Stop is idempotent on unstarted watcher
    watcher.stop();
    STUTTO_ASSERT(!watcher.is_running());

    // 3. Start with non-existent process (polling path exercised)
    watcher.start("nonexistent_test_proc_123456789.exe", cb);
    STUTTO_ASSERT(watcher.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    STUTTO_ASSERT(mock.active_pid == 0);
    STUTTO_ASSERT(mock.waiting);

    // 4. Clean stop
    watcher.stop();
    STUTTO_ASSERT(!watcher.is_running());

    // 5. Repeated stop is idempotent
    watcher.stop();
    STUTTO_ASSERT(!watcher.is_running());

    std::cout << "  -> ProcessWatcher lifecycle and query interface PASSED.\n";
}

static void test_process_watcher_parity() {
    std::cout << "[TEST] Testing ProcessWatcher parity with resolve_process_name_to_pid...\n";

    wchar_t self_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    STUTTO_ASSERT(len > 0);

    std::string self_full_backslash = stuttometer::utf16_to_utf8(self_path);
    std::string self_full_forward = self_full_backslash;
    std::replace(self_full_forward.begin(), self_full_forward.end(), '\\', '/');

    size_t last_slash = self_full_backslash.find_last_of('\\');
    std::string self_name_exe = (last_slash != std::string::npos) ? self_full_backslash.substr(last_slash + 1) : self_full_backslash;
    std::string self_name_no_exe = self_name_exe;
    if (self_name_no_exe.length() > 4 && _stricmp(self_name_no_exe.c_str() + self_name_no_exe.length() - 4, ".exe") == 0) {
        self_name_no_exe = self_name_no_exe.substr(0, self_name_no_exe.length() - 4);
    }

    std::string self_prefix = (self_name_no_exe.length() > 3) ? self_name_no_exe.substr(0, self_name_no_exe.length() - 2) : self_name_no_exe;
    std::string self_substr = (self_name_no_exe.length() > 4) ? self_name_no_exe.substr(1, self_name_no_exe.length() - 2) : self_name_no_exe;

    std::string self_drive_relative = (self_full_backslash.length() >= 2 && self_full_backslash[1] == ':')
                                          ? self_full_backslash.substr(0, 2) + self_name_exe
                                          : self_name_exe;
    std::string self_long_path = "C:\\" + std::string(220, 'a') + "\\..\\" + self_name_exe;

    std::vector<std::string> test_variants = {
        self_name_exe,
        self_name_no_exe,
        self_full_backslash,
        self_full_forward,
        self_prefix,
        self_substr,
        self_drive_relative,
        self_long_path,
        "stutto_nonexistent_proc_987654321.exe",
        ""
    };

    stuttometer::ProcessWatcher watcher;
    for (size_t i = 0; i < test_variants.size(); ++i) {
        const auto& variant = test_variants[i];
        bool ok = false;
        for (int retry = 0; retry < 2; ++retry) {
            uint32_t expected_pid = stuttometer::resolve_process_name_to_pid(variant);
            watcher.set_target_for_test(variant);
            uint32_t watcher_pid = watcher.find_target_pid_snapshot();
            if (expected_pid == watcher_pid) {
                if (i < 8) {
                    if (expected_pid != 0) {
                        ok = true;
                        break;
                    }
                } else {
                    if (expected_pid == 0) {
                        ok = true;
                        break;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        STUTTO_ASSERT_MSG(ok, ("Parity mismatch for variant: " + variant).c_str());
    }

    std::cout << "  -> ProcessWatcher parity verified across " << test_variants.size() << " target variants.\n";
}

static void test_process_watcher_cadence() {
    std::cout << "[TEST] Testing ProcessWatcher adaptive cadence (50ms waiting vs ~2000ms attached)...\n";

    std::atomic<bool> is_waiting_flag{true};
    stuttometer::WatcherCallbacks cb;
    cb.user_data = &is_waiting_flag;
    cb.is_waiting = [](void* ud) -> bool {
        return static_cast<std::atomic<bool>*>(ud)->load(std::memory_order_relaxed);
    };

    stuttometer::ProcessWatcher watcher;
    watcher.start("stutto_cadence_test_proc_987654321.exe", cb);
    STUTTO_ASSERT(watcher.is_running());

    // Verify set_target_for_test rejects modification while running to prevent data races
    STUTTO_ASSERT(!watcher.set_target_for_test("other.exe"));

    // 1. Waiting mode (~50ms cadence)
    is_waiting_flag.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t c0 = watcher.get_poll_count_for_test();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    uint64_t c1 = watcher.get_poll_count_for_test();
    uint64_t delta_waiting = c1 - c0;
    STUTTO_ASSERT(delta_waiting >= 1 && delta_waiting <= 5);

    // 2. Attached mode (~2000ms cadence)
    is_waiting_flag.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t c2 = watcher.get_poll_count_for_test();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    uint64_t c3 = watcher.get_poll_count_for_test();
    uint64_t delta_attached = c3 - c2;
    STUTTO_ASSERT(delta_attached <= 1);

    watcher.stop();
    STUTTO_ASSERT(!watcher.is_running());

    std::cout << "  -> ProcessWatcher adaptive cadence verified (waiting delta: " 
              << delta_waiting << ", attached delta: " << delta_attached << ").\n";
}

static void test_process_watcher_zero_allocation() {
    std::cout << "[TEST] Testing ProcessWatcher zero-allocation in find_target_pid_snapshot...\n";

    stuttometer::ProcessWatcher watcher;
    wchar_t self_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    STUTTO_ASSERT(len > 0);
    std::string self_full = stuttometer::utf16_to_utf8(self_path);
    size_t last_slash = self_full.find_last_of('\\');
    std::string self_name = (last_slash != std::string::npos) ? self_full.substr(last_slash + 1) : "test_cli_args.exe";

    std::string self_name_no_exe = self_name;
    if (self_name_no_exe.length() > 4 && _stricmp(self_name_no_exe.c_str() + self_name_no_exe.length() - 4, ".exe") == 0) {
        self_name_no_exe = self_name_no_exe.substr(0, self_name_no_exe.length() - 4);
    }
    std::string self_prefix = (self_name_no_exe.length() > 3) ? self_name_no_exe.substr(0, self_name_no_exe.length() - 2) : self_name_no_exe;
    std::string self_substr = (self_name_no_exe.length() > 4) ? self_name_no_exe.substr(1, self_name_no_exe.length() - 2) : self_name_no_exe;

    // Warm-up call (page-in code / CRT tables)
    watcher.set_target_for_test(self_name);
    uint32_t warm_pid = watcher.find_target_pid_snapshot();
    STUTTO_ASSERT(warm_pid != 0);

    // 1. Pass 1: Exact match zero-allocation
    watcher.set_target_for_test(self_name);
    g_allocation_detected = false;
    g_disallow_allocations = true;
    uint32_t pid_exact = watcher.find_target_pid_snapshot();
    g_disallow_allocations = false;
    STUTTO_ASSERT(pid_exact == warm_pid);
    STUTTO_ASSERT(!g_allocation_detected);

    // 2. Pass 2: Prefix match zero-allocation
    watcher.set_target_for_test(self_prefix);
    g_allocation_detected = false;
    g_disallow_allocations = true;
    uint32_t pid_prefix = watcher.find_target_pid_snapshot();
    g_disallow_allocations = false;
    STUTTO_ASSERT(pid_prefix != 0);
    STUTTO_ASSERT(!g_allocation_detected);

    // 3. Pass 3: Substring match zero-allocation
    watcher.set_target_for_test(self_substr);
    g_allocation_detected = false;
    g_disallow_allocations = true;
    uint32_t pid_substr = watcher.find_target_pid_snapshot();
    g_disallow_allocations = false;
    STUTTO_ASSERT(pid_substr != 0);
    STUTTO_ASSERT(!g_allocation_detected);

    // 4. Non-existent process (exhaustive full-snapshot scan across all passes)
    watcher.set_target_for_test("stutto_nonexistent_proc_987654321.exe");
    g_allocation_detected = false;
    g_disallow_allocations = true;
    uint32_t pid_none = watcher.find_target_pid_snapshot();
    g_disallow_allocations = false;
    STUTTO_ASSERT(pid_none == 0);
    STUTTO_ASSERT(!g_allocation_detected);

    // 5. Empty target
    watcher.set_target_for_test("");
    g_allocation_detected = false;
    g_disallow_allocations = true;
    uint32_t pid_empty = watcher.find_target_pid_snapshot();
    g_disallow_allocations = false;
    STUTTO_ASSERT(pid_empty == 0);
    STUTTO_ASSERT(!g_allocation_detected);

    std::cout << "  -> ProcessWatcher find_target_pid_snapshot zero dynamic allocation verified across all matching passes and full table scans.\n";
}

int main() {
    try {
        test_pacing_profile_parsing();
        test_rejection_of_invalid_profiles();
        test_high_refresh_alias();
        test_pacing_profile_precedence_over_alias();
        test_precedence_explicit_override();
        test_version_and_self_check();
        test_range_validations();
        test_cli_ranges_table();
        test_help_flags();
        test_manual_threshold_flags();
        test_min_report_severity_flag();
        test_process_watcher_lifecycle();
        test_process_watcher_parity();
        test_process_watcher_cadence();
        test_process_watcher_zero_allocation();

        std::cout << "\n[ALL CLI ARGS TESTS PASSED]\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\n[CLI ARGS TEST FAILED] " << ex.what() << "\n";
        return 1;
    }
}
