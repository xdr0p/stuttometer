#pragma once

#include "correlator.hpp"
#include "json_reporter.hpp"
#include <string>

namespace stuttometer {

struct MockSimulationResult {
    DiagnosticReport report;
    std::string summary_text;
    std::string json_text;
};

// Generates a deterministic synthetic DPC spike & DXGI present stutter trace
// and runs the complete correlation and reporting pipeline.
MockSimulationResult run_mock_simulation_pipeline(bool redact = false);

} // namespace stuttometer
