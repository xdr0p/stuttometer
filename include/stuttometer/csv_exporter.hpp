#pragma once

#include "correlator.hpp"
#include <iostream>
#include <filesystem>

namespace stuttometer::csv {

// Exports frame timeline to RFC 4180 CRLF UTF-8 formatted CSV stream.
// Callers passing file streams must ensure the stream was opened with std::ios::binary.
void export_to_stream(const DiagnosticReport& report, std::ostream& out);

// Exports frame timeline to file in binary mode (std::ios::binary) with flush and status check
bool export_to_file(const DiagnosticReport& report, const std::filesystem::path& file_path);

} // namespace stuttometer::csv
