#pragma once

#include "correlator.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <iostream>
#include <filesystem>

namespace stuttometer {

class JsonReporter {
public:
    JsonReporter() = default;
    ~JsonReporter() = default;

    // Serializes report to nlohmann::json object conforming to schema v1.0
    nlohmann::json to_json(const DiagnosticReport& report, bool redact = false) const;

    // Serializes report to formatted JSON string
    std::string to_json_string(const DiagnosticReport& report, bool redact = false, int indent = 2) const;

    // Writes report to file path
    bool save_to_file(const DiagnosticReport& report, const std::filesystem::path& file_path, bool redact = false) const;
    bool save_to_file(const DiagnosticReport& report, const std::string& file_path, bool redact = false) const;

    // Prints human-readable summary to standard console with optional redaction
    void print_console_summary(const DiagnosticReport& report, std::ostream& out = std::cout, bool redact = false) const;
};

} // namespace stuttometer
