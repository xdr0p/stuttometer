#pragma once

#include "stuttometer/correlator.hpp"
#include "stuttometer/session_benchmark.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

namespace stuttometer {

/**
 * @brief Crash-safe incremental NDJSON session bundle writer.
 *
 * Writes one JSON object per line:
 *   Line 1 (header, written only when creating/empty):
 *     {"type":"header","bundle_schema":"1.0","tool_version":...,"started_utc":...,"config_snapshot":...}
 *   Per trigger (flushed immediately):
 *     {"type":"report","report":<DiagnosticReport JSON>}
 *   On clean stop:
 *     {"type":"summary","summary":<BenchmarkSummary JSON>}
 *     or for CLI (no BenchmarkSummary):
 *     {"type":"summary","reports_total":...,"duration_ms":...}
 */
class SessionBundleWriter {
public:
    static constexpr size_t DEFAULT_MAX_RETAINED_FILES = 20;

    SessionBundleWriter() = default;
    ~SessionBundleWriter();

    SessionBundleWriter(const SessionBundleWriter&) = delete;
    SessionBundleWriter& operator=(const SessionBundleWriter&) = delete;
    SessionBundleWriter(SessionBundleWriter&&) noexcept = default;
    SessionBundleWriter& operator=(SessionBundleWriter&&) noexcept = default;

    /**
     * @brief Generates a timestamped session filename in the given directory:
     *        `<dir>/stutto_session_YYYYMMDD_HHMMSS.jsonl`
     */
    static std::filesystem::path make_session_filename(const std::filesystem::path& dir);

    /**
     * @brief Opens an explicit session file in append mode.
     *        Writes the header line only if the file is newly created or empty.
     */
    bool open(
        const std::filesystem::path& file_path,
        const nlohmann::json& config_snapshot = nlohmann::json::object(),
        bool redact = false,
        size_t max_retained_files = DEFAULT_MAX_RETAINED_FILES,
        bool rotate_dir = false
    );

    /**
     * @brief Convenience helper that creates `<dir>/stutto_session_YYYYMMDD_HHMMSS.jsonl`
     *        and rotates older `stutto_session_*.jsonl` files to `max_retained_files`.
     */
    bool open_in_dir(
        const std::filesystem::path& dir,
        const nlohmann::json& config_snapshot = nlohmann::json::object(),
        bool redact = false,
        size_t max_retained_files = DEFAULT_MAX_RETAINED_FILES
    );

    bool write_report(const DiagnosticReport& report, bool redact = false);
    bool write_summary(const BenchmarkSummary& summary);
    bool write_cli_summary(uint64_t reports_total, double duration_ms);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::ofstream file_;
    std::filesystem::path path_;
};

/**
 * @brief Streaming single-file JSON session exporter for GUI in-memory history.
 */
class SessionBundleExporter {
public:
    static bool export_session_to_stream(
        std::ostream& out,
        const std::vector<DiagnosticReport>& reports,
        const BenchmarkSummary& summary,
        bool redact = false
    );

    static bool export_session_string(
        std::string& out_json,
        const std::vector<DiagnosticReport>& reports,
        const BenchmarkSummary& summary,
        bool redact = false
    );

    static bool export_session(
        const std::filesystem::path& file_path,
        const std::vector<DiagnosticReport>& reports,
        const BenchmarkSummary& summary,
        bool redact = false
    );
};

} // namespace stuttometer
