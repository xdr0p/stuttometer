#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <sstream>
#include <cassert>

static void test_json_serialization_and_redaction() {
    std::cout << "[TEST] Validating JSON Schema v1.0 and PII Redaction...\n";

    stuttometer::DiagnosticReport report;
    report.schema_version = "1.0";
    report.tool_version = "0.1.0";
    report.timestamp_utc = "2026-08-11T02:50:00.000Z";
    report.window_pre_ms = 250.0;
    report.window_post_ms = 30.0;
    report.present_threshold_ms = 25.0;
    report.total_events = 1500;

    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.trigger_timestamp_qpc = 1000000000;
    report.trigger.duration_ms = 48.5;
    report.trigger.target_pid = 7788;
    report.trigger.target_tid = 9911;
    report.trigger.target_process = "ConfidentialGame.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "dpc_isr_spike";
    diag.confidence = 0.88;
    diag.summary = "Driver nvlddmkm.sys executed a single DPC routine for 3.42ms on Core 4.";
    diag.factors = { 0.85, 1.0, 0.95 };

    stuttometer::EvidenceItem ev;
    ev.event_type = "DPC";
    ev.driver_module = "nvlddmkm.sys";
    ev.routine_address = "0xFFFFF8012A34B100";
    ev.duration_us = 3420;
    ev.cpu_core = 4;
    ev.offset_from_trigger_ms = -4.2;
    diag.evidence.push_back(ev);

    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;

    // Test standard output
    auto j_plain = reporter.to_json(report, false);
    assert(j_plain["schema_version"] == "1.0");
    assert(j_plain["trigger"]["target_process"] == "ConfidentialGame.exe");
    assert(j_plain["diagnoses"][0]["evidence"][0]["routine_address"] == "0xFFFFF8012A34B100");
    std::cout << "  -> Plain JSON Schema v1.0 validation PASSED.\n";

    // Test redacted JSON output
    auto j_redacted = reporter.to_json(report, true);
    assert(j_redacted["configuration"]["redacted"] == true);
    assert(j_redacted["trigger"]["target_process"] == "Process_7788");
    assert(j_redacted["diagnoses"][0]["evidence"][0]["routine_address"] == "0xREDACTED");
    std::cout << "  -> Redacted JSON sanitization validation PASSED.\n";

    // Test console summary redaction
    std::stringstream ss_redacted;
    reporter.print_console_summary(report, ss_redacted, true);
    const std::string console_str = ss_redacted.str();
    assert(console_str.find("Process_7788") != std::string::npos);
    assert(console_str.find("ConfidentialGame.exe") == std::string::npos);
    assert(console_str.find("0xREDACTED") != std::string::npos);
    assert(console_str.find("0xFFFFF8012A34B100") == std::string::npos);
    std::cout << "  -> Redacted Console Summary sanitization validation PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer JSON Schema & Redaction Tests ===\n";
    test_json_serialization_and_redaction();
    std::cout << ">>> All JSON Schema tests PASSED! <<<\n\n";
    return 0;
}
