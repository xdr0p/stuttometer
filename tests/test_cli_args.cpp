#include "test_common.hpp"
#include "stuttometer/cli_parser.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

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
        STUTTO_ASSERT(out.str().find("Stuttometer v0.5.0") != std::string::npos);
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

int main() {
    try {
        test_pacing_profile_parsing();
        test_rejection_of_invalid_profiles();
        test_high_refresh_alias();
        test_pacing_profile_precedence_over_alias();
        test_precedence_explicit_override();
        test_version_and_self_check();
        test_range_validations();
        test_help_flags();

        std::cout << "\n[ALL CLI ARGS TESTS PASSED]\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\n[CLI ARGS TEST FAILED] " << ex.what() << "\n";
        return 1;
    }
}
