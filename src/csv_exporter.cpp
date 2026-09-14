#include "stuttometer/csv_exporter.hpp"
#include <fstream>
#include <iomanip>

namespace stuttometer::csv {

void export_to_stream(const DiagnosticReport& report, std::ostream& out) {
    out << escape_csv_field("frame_index") << ','
        << escape_csv_field("relative_index") << ','
        << escape_csv_field("qpc_timestamp") << ','
        << escape_csv_field("duration_ms") << ','
        << escape_csv_field("offset_from_trigger_ms") << ','
        << escape_csv_field("is_trigger_frame") << ','
        << escape_csv_field("is_pacing_stall") << "\r\n";
    for (const auto& pt : report.frame_timeline) {
        out << pt.frame_index << ','
            << pt.relative_index << ','
            << pt.qpc_timestamp << ','
            << std::fixed << std::setprecision(4) << pt.duration_ms << ','
            << std::fixed << std::setprecision(4) << pt.offset_from_trigger_ms << ','
            << (pt.is_trigger_frame ? "true" : "false") << ','
            << (pt.is_pacing_stall ? "true" : "false") << "\r\n";
    }
}

bool export_to_file(const DiagnosticReport& report, const std::filesystem::path& file_path) {
    if (file_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }
    std::ofstream out(file_path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    export_to_stream(report, out);
    out.flush();
    return out.good();
}

} // namespace stuttometer::csv
