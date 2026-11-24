#include "stuttometer/session_bundle.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/version.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace stuttometer {

namespace {

std::tm current_utc_tm() noexcept {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    gmtime_s(&utc_tm, &t);
    return utc_tm;
}

std::string format_utc_iso8601() {
    const std::tm utc_tm = current_utc_tm();
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string format_utc_filename_stamp() {
    const std::tm utc_tm = current_utc_tm();
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

bool is_path_key(const std::string& key) {
    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with(key, "_dir") ||
           ends_with(key, "_path") ||
           ends_with(key, "_file") ||
           key == "output_dir" ||
           key == "session_file";
}

nlohmann::json sanitize_config_snapshot(const nlohmann::json& snapshot, bool redact) {
    if (!snapshot.is_object()) {
        return nlohmann::json::object();
    }
    if (!redact) {
        return snapshot;
    }
    nlohmann::json cleaned = nlohmann::json::object();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (is_path_key(it.key())) {
            continue;
        }
        if (it.value().is_object()) {
            cleaned[it.key()] = sanitize_config_snapshot(it.value(), true);
        } else {
            cleaned[it.key()] = it.value();
        }
    }
    return cleaned;
}

} // namespace

SessionBundleWriter::~SessionBundleWriter() {
    close();
}

std::filesystem::path SessionBundleWriter::make_session_filename(const std::filesystem::path& dir) {
    const std::string name = "stutto_session_" + format_utc_filename_stamp() + ".jsonl";
    return dir / name;
}

bool SessionBundleWriter::open(
    const std::filesystem::path& file_path,
    const nlohmann::json& config_snapshot,
    bool redact,
    size_t max_retained_files,
    bool rotate_dir
) {
    close();

    std::error_code ec;
    const std::filesystem::path parent = file_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    bool needs_header = true;
    if (std::filesystem::exists(file_path, ec) && !ec) {
        const auto sz = std::filesystem::file_size(file_path, ec);
        if (!ec && sz > 0) {
            needs_header = false;
        }
    }

    file_.open(file_path, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        return false;
    }

    path_ = file_path;

    if (needs_header) {
        nlohmann::json header = {
            {"type", "header"},
            {"bundle_schema", "1.0"},
            {"tool_version", std::string(TOOL_VERSION)},
            {"started_utc", format_utc_iso8601()},
            {"redacted", redact},
            {"config_snapshot", sanitize_config_snapshot(config_snapshot, redact)}
        };
        file_ << header.dump() << '\n';
        file_.flush();
        if (!file_.good()) {
            close();
            return false;
        }
    }

    if (rotate_dir && !parent.empty() && max_retained_files > 0) {
        rotate_directory_by_prefix(parent, "stutto_session_", ".jsonl", max_retained_files);
    }

    return true;
}

bool SessionBundleWriter::open_in_dir(
    const std::filesystem::path& dir,
    const nlohmann::json& config_snapshot,
    bool redact,
    size_t max_retained_files
) {
    return open(make_session_filename(dir), config_snapshot, redact, max_retained_files, true);
}

bool SessionBundleWriter::write_report(const DiagnosticReport& report, bool redact) {
    if (!file_.is_open()) {
        return false;
    }

    JsonReporter reporter;
    nlohmann::json line = {
        {"type", "report"},
        {"report", reporter.to_json(report, redact)}
    };
    file_ << line.dump() << '\n';
    file_.flush();
    return file_.good();
}

bool SessionBundleWriter::write_summary(const BenchmarkSummary& summary) {
    if (!file_.is_open()) {
        return false;
    }

    nlohmann::json summary_json = nlohmann::json::parse(summary.to_json(), nullptr, false);
    if (summary_json.is_discarded()) {
        return false;
    }

    nlohmann::json line = {
        {"type", "summary"},
        {"summary", std::move(summary_json)}
    };
    file_ << line.dump() << '\n';
    file_.flush();
    return file_.good();
}

bool SessionBundleWriter::write_cli_summary(uint64_t reports_total, double duration_ms) {
    if (!file_.is_open()) {
        return false;
    }

    nlohmann::json line = {
        {"type", "summary"},
        {"reports_total", reports_total},
        {"duration_ms", duration_ms}
    };
    file_ << line.dump() << '\n';
    file_.flush();
    return file_.good();
}

void SessionBundleWriter::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    path_.clear();
}

bool SessionBundleExporter::export_session(
    const std::filesystem::path& file_path,
    const std::vector<DiagnosticReport>& reports,
    const BenchmarkSummary& summary,
    bool redact
) {
    std::error_code ec;
    const std::filesystem::path parent = file_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream out(file_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    nlohmann::json summary_json = nlohmann::json::parse(summary.to_json(), nullptr, false);
    if (summary_json.is_discarded()) {
        return false;
    }

    out << "{\n"
        << "  \"session_schema\": \"1.0\",\n"
        << "  \"tool_version\": " << nlohmann::json(std::string(TOOL_VERSION)).dump() << ",\n"
        << "  \"exported_utc\": " << nlohmann::json(format_utc_iso8601()).dump() << ",\n"
        << "  \"redacted\": " << (redact ? "true" : "false") << ",\n"
        << "  \"benchmark_summary\": " << summary_json.dump() << ",\n"
        << "  \"reports\": [";

    JsonReporter reporter;
    for (size_t i = 0; i < reports.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << "\n    " << reporter.to_json(reports[i], redact).dump();
    }

    if (!reports.empty()) {
        out << '\n' << "  ";
    }
    out << "]\n}\n";
    out.flush();
    return out.good();
}

} // namespace stuttometer
