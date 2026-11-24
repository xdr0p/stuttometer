#include "test_common.hpp"
#include "stuttometer/session_bundle.hpp"
#include "stuttometer/version.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

stuttometer::DiagnosticReport make_sample_report(uint32_t pid, const std::string& proc_name, double dur_ms) {
    stuttometer::DiagnosticReport rep;
    rep.timestamp_utc = "2026-11-25T01:00:00Z";
    rep.qpc_frequency = 10000000ULL;
    rep.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    rep.trigger.reason = stuttometer::TriggerReason::RELATIVE_SPIKE;
    rep.trigger.target_pid = pid;
    rep.trigger.target_tid = pid + 1;
    rep.trigger.duration_ms = dur_ms;
    rep.trigger.baseline_avg_ms = 5.0;
    rep.trigger.baseline_fps = 200.0;
    rep.trigger.spike_ratio = dur_ms / 5.0;
    rep.target_process = proc_name;
    return rep;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path);
    STUTTO_ASSERT(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

} // namespace

static void test_writer_header_flush_and_reopen() {
    std::cout << "[TEST] Validating SessionBundleWriter header, per-line flush, reopen, and summary...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_session_bundle_writer";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    const std::filesystem::path bundle_path = test_dir / "session_test.jsonl";

    nlohmann::json cfg = {
        {"provider_tier", "standard"},
        {"output_dir", "C:\\Users\\Alice\\Documents\\Stuttometer"},
        {"session_file", "C:\\Users\\Alice\\session.jsonl"}
    };

    {
        stuttometer::SessionBundleWriter writer;
        STUTTO_ASSERT(writer.open(bundle_path, cfg, /*redact=*/false));
        STUTTO_ASSERT(writer.is_open());
        STUTTO_ASSERT(writer.path() == bundle_path);

        const auto rep1 = make_sample_report(1234, "SkyrimSE.exe", 42.5);
        STUTTO_ASSERT(writer.write_report(rep1, /*redact=*/false));

        // Per-line flush survival: readable on disk before close()
        const auto lines_mid = read_lines(bundle_path);
        STUTTO_ASSERT(lines_mid.size() == 2);

        const auto h = nlohmann::json::parse(lines_mid[0]);
        STUTTO_ASSERT(h["type"] == "header");
        STUTTO_ASSERT(h["bundle_schema"] == "1.0");
        STUTTO_ASSERT(h["tool_version"] == std::string(stuttometer::TOOL_VERSION));
        STUTTO_ASSERT(h.contains("started_utc") && h["started_utc"].is_string());
        STUTTO_ASSERT(h["config_snapshot"]["provider_tier"] == "standard");
        STUTTO_ASSERT(h["config_snapshot"]["output_dir"] == "C:\\Users\\Alice\\Documents\\Stuttometer");

        const auto r1 = nlohmann::json::parse(lines_mid[1]);
        STUTTO_ASSERT(r1["type"] == "report");
        STUTTO_ASSERT(r1["report"]["trigger"]["target_pid"] == 1234);
        STUTTO_ASSERT(r1["report"]["trigger"]["target_process"] == "SkyrimSE.exe");
    }

    // Append-after-reopen: must NOT duplicate header line
    {
        stuttometer::SessionBundleWriter writer2;
        STUTTO_ASSERT(writer2.open(bundle_path, cfg, /*redact=*/false));

        const auto rep2 = make_sample_report(5678, "game.exe", 110.0);
        STUTTO_ASSERT(writer2.write_report(rep2, /*redact=*/false));

        stuttometer::BenchmarkSummary summary{};
        summary.target_process = "SkyrimSE.exe";
        summary.target_pid = 1234;
        summary.total_frames = 1000;
        summary.stutters_detected = 2;
        summary.minor_stutters = 5;
        summary.total_triggers = 7;
        STUTTO_ASSERT(writer2.write_summary(summary));
        writer2.close();
        STUTTO_ASSERT(!writer2.is_open());
    }

    const auto lines_end = read_lines(bundle_path);
    STUTTO_ASSERT(lines_end.size() == 4);
    STUTTO_ASSERT(nlohmann::json::parse(lines_end[0])["type"] == "header");
    STUTTO_ASSERT(nlohmann::json::parse(lines_end[1])["type"] == "report");
    STUTTO_ASSERT(nlohmann::json::parse(lines_end[2])["type"] == "report");
    const auto s = nlohmann::json::parse(lines_end[3]);
    STUTTO_ASSERT(s["type"] == "summary");
    STUTTO_ASSERT(s["summary"]["stutters_detected"] == 2);
    STUTTO_ASSERT(s["summary"]["minor_stutters"] == 5);

    std::filesystem::remove_all(test_dir, ec);
}

static void test_writer_redaction_and_cli_summary() {
    std::cout << "[TEST] Validating SessionBundleWriter redaction and CLI summary...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_session_bundle_redact";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    const std::filesystem::path bundle_path = test_dir / "session_redacted.jsonl";
    nlohmann::json cfg = {
        {"provider_tier", "full"},
        {"output_dir", "C:\\Secret\\User\\Dir"},
        {"config_path", "C:\\Secret\\cfg.json"},
        {"nested", {
            {"trace_file", "D:\\traces\\t.etl"},
            {"keep_flag", true}
        }}
    };

    stuttometer::SessionBundleWriter writer;
    STUTTO_ASSERT(writer.open(bundle_path, cfg, /*redact=*/true));
    STUTTO_ASSERT(writer.write_report(make_sample_report(4242, "SecretGame.exe", 55.0), /*redact=*/true));
    STUTTO_ASSERT(writer.write_cli_summary(1, 1234.5));
    writer.close();

    const auto lines = read_lines(bundle_path);
    STUTTO_ASSERT(lines.size() == 3);

    const auto h = nlohmann::json::parse(lines[0]);
    STUTTO_ASSERT(h["redacted"] == true);
    STUTTO_ASSERT(h["config_snapshot"]["provider_tier"] == "full");
    STUTTO_ASSERT(!h["config_snapshot"].contains("output_dir"));
    STUTTO_ASSERT(!h["config_snapshot"].contains("config_path"));
    STUTTO_ASSERT(!h["config_snapshot"]["nested"].contains("trace_file"));
    STUTTO_ASSERT(h["config_snapshot"]["nested"]["keep_flag"] == true);

    const auto r = nlohmann::json::parse(lines[1]);
    STUTTO_ASSERT(r["report"]["trigger"]["target_pid"] == 0);
    STUTTO_ASSERT(r["report"]["trigger"]["target_process"] != "SecretGame.exe");

    const auto s = nlohmann::json::parse(lines[2]);
    STUTTO_ASSERT(s["type"] == "summary");
    STUTTO_ASSERT(s["reports_total"] == 1);
    STUTTO_ASSERT(s["duration_ms"] == 1234.5);

    std::filesystem::remove_all(test_dir, ec);
}

static void test_exporter_single_file() {
    std::cout << "[TEST] Validating SessionBundleExporter streaming export...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_session_bundle_export";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    const std::filesystem::path export_path = test_dir / "exported_session.json";
    std::vector<stuttometer::DiagnosticReport> reports = {
        make_sample_report(101, "gameA.exe", 25.0),
        make_sample_report(101, "gameA.exe", 75.0)
    };

    stuttometer::BenchmarkSummary summary{};
    summary.target_process = "gameA.exe";
    summary.target_pid = 101;
    summary.total_frames = 500;
    summary.stutters_detected = 2;

    STUTTO_ASSERT(stuttometer::SessionBundleExporter::export_session(export_path, reports, summary, /*redact=*/false));

    std::ifstream in(export_path);
    STUTTO_ASSERT(in.is_open());
    const auto j = nlohmann::json::parse(in);
    STUTTO_ASSERT(j["session_schema"] == "1.0");
    STUTTO_ASSERT(j["tool_version"] == std::string(stuttometer::TOOL_VERSION));
    STUTTO_ASSERT(j["redacted"] == false);
    STUTTO_ASSERT(j["benchmark_summary"]["total_frames"] == 500);
    STUTTO_ASSERT(j["reports"].is_array() && j["reports"].size() == 2);
    STUTTO_ASSERT(j["reports"][0]["trigger"]["duration_ms"] == 25.0);
    STUTTO_ASSERT(j["reports"][1]["trigger"]["duration_ms"] == 75.0);

    std::filesystem::remove_all(test_dir, ec);
}

int main() {
    test_writer_header_flush_and_reopen();
    test_writer_redaction_and_cli_summary();
    test_exporter_single_file();
    std::cout << "[PASS] All SessionBundle tests passed!\n";
    return 0;
}
