#pragma once

#include "correlator.hpp"
#include <iostream>
#include <filesystem>
#include <string>
#include <string_view>

namespace stuttometer::csv {

// Escapes a field according to RFC 4180 §2.6
[[nodiscard]] inline std::string escape_csv_field(std::string_view field) {
    if (field.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(field);
    }
    std::string result = "\"";
    for (char c : field) {
        if (c == '"') {
            result += "\"\"";
        } else {
            result += c;
        }
    }
    result += '"';
    return result;
}

// Exports frame timeline to RFC 4180 CRLF UTF-8 formatted CSV stream.
// Callers passing file streams must ensure the stream was opened with std::ios::binary.
void export_to_stream(const DiagnosticReport& report, std::ostream& out);

// Exports frame timeline to file in binary mode (std::ios::binary) with flush and status check
bool export_to_file(const DiagnosticReport& report, const std::filesystem::path& file_path);

} // namespace stuttometer::csv
