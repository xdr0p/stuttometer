#include "test_common.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <sstream>

static void test_json_serialization_and_deep_redaction() {
    std::cout << "[TEST] Validating JSON Schema v1.0, Audio Counts & Deep PII Redaction...\n";

    stuttometer::DiagnosticReport report;
    report.schema_version = "1.0";
    report.tool_version = "0.1.0";
    report.timestamp_utc = "2026-08-11T02:50:00.000Z";
    report.window_pre_ms = 250.0;
    report.window_post_ms = 30.0;
    report.present_threshold_ms = 25.0;
    report.total_events = 1500;
    report.event_counts.audio = 3;
    report.etw_events_lost = 0;
    report.etw_buffers_lost = 0;
    report.provider_context.kernel_dpc_active = true;
    report.provider_context.user_dxgkrnl_active = true;
    report.thresholds.dpc_threshold_us = 1000;

    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.trigger_timestamp_qpc = 1000000000;
    report.trigger.duration_ms = 48.5;
    report.trigger.target_pid = 7788;
    report.trigger.target_tid = 9911;
    report.target_process = "ConfidentialGame.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "dpc_isr_spike";
    diag.confidence = 0.88;
    diag.summary = "Critical thread 9911 stalled. A driver executed a single DPC routine for 3.42ms on Core 4.";
    diag.factors = { 0.85, 1.0, 0.95 };

    stuttometer::EvidenceItem ev;
    ev.event_type = "DPC";
    ev.driver_module = "nvlddmkm.sys";
    ev.routine_address = "0xFFFFF8012A34B100";
    ev.duration_us = 3420;
    ev.cpu_core = 4;
    ev.offset_from_trigger_ms = -4.2;
    ev.extra_info = "Resumed TID 9911 after 3.4ms";
    diag.evidence.push_back(ev);

    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;

    // Test standard plain output
    auto j_plain = reporter.to_json(report, false);
    STUTTO_ASSERT(j_plain["schema_version"] == "1.0");
    STUTTO_ASSERT(j_plain["trigger"]["target_process"] == "ConfidentialGame.exe");
    STUTTO_ASSERT(j_plain["trigger"]["target_pid"] == 7788);
    STUTTO_ASSERT(j_plain["trigger"]["duration_ms"] == 48.5);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["dpc_threshold_us"] == 1000);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["smi_severity_threshold_ms"] == 33.3);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["d3d12_pso_threshold_ms"] == 5);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["vram_demoted_threshold_mb"] == 8);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["mem_alloc_threshold_mb"] == 16);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["mem_trim_threshold_mb"] == 4);
    STUTTO_ASSERT(j_plain["configuration"]["thresholds"]["mem_physical_latency_us"] == 1000);
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"]["kernel_dpc"] == true);
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"]["user_dxgkrnl"] == true);
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("kernel_pagefault"));
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("user_processor_power"));
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("user_antimalware"));
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("user_d3d12"));
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("user_vram_paging"));
    STUTTO_ASSERT(j_plain["configuration"]["active_providers"].contains("kernel_memory"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"]["AUDIO"] == 3);
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("PAGE_FAULT"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("THERMAL_THROTTLE"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("ANTIMALWARE_SCAN"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("D3D12_PSO_CREATE"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("DXGKRNL_VRAM_PAGING"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("MEM_VIRTUAL_ALLOC"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("MEM_WORKING_SET_TRIM"));
    STUTTO_ASSERT(j_plain["statistics"]["events_by_category"].contains("MEM_PHYSICAL_ALLOC"));
    STUTTO_ASSERT(j_plain["statistics"].contains("in_flight_insertion_failures"));
    STUTTO_ASSERT(j_plain["statistics"].contains("ring_buffer_extraction_drops"));
    STUTTO_ASSERT(j_plain["statistics"].contains("ring_buffer_producer_drops"));
    STUTTO_ASSERT(j_plain["diagnoses"][0]["evidence"][0]["routine_address"] == "0xFFFFF8012A34B100");
    STUTTO_ASSERT(j_plain["diagnoses"][0]["evidence"][0]["extra_info"] == "Resumed TID 9911 after 3.4ms");
    std::cout << "  -> Plain JSON Schema v1.0 and Audio category counting PASSED.\n";

    // Test deep redacted JSON output
    auto j_redacted = reporter.to_json(report, true);
    STUTTO_ASSERT(j_redacted["configuration"]["redacted"] == true);
    STUTTO_ASSERT(j_redacted["trigger"]["target_process"] == "Process_REDACTED");
    STUTTO_ASSERT(j_redacted["trigger"]["target_pid"] == 0);
    STUTTO_ASSERT(j_redacted["trigger"]["target_tid"] == 0);
    STUTTO_ASSERT(j_redacted["diagnoses"][0]["evidence"][0]["driver_module"] == "driver_REDACTED.sys");
    STUTTO_ASSERT(j_redacted["diagnoses"][0]["evidence"][0]["routine_address"] == "0xREDACTED");
    // Summary and Evidence extra_info must have TID 9911 scrubbed
    const std::string red_summary = j_redacted["diagnoses"][0]["summary"];
    STUTTO_ASSERT(red_summary.find("9911") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("REDACTED") != std::string::npos);
    const std::string red_extra = j_redacted["diagnoses"][0]["evidence"][0]["extra_info"];
    STUTTO_ASSERT(red_extra.find("9911") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("REDACTED") != std::string::npos);
    std::cout << "  -> Deep Redacted JSON sanitization validation PASSED.\n";

    // Test console summary deep redaction
    std::stringstream ss_redacted;
    reporter.print_console_summary(report, ss_redacted, true);
    const std::string console_str = ss_redacted.str();
    STUTTO_ASSERT(console_str.find("Process_REDACTED") != std::string::npos);
    STUTTO_ASSERT(console_str.find("ConfidentialGame.exe") == std::string::npos);
    STUTTO_ASSERT(console_str.find("7788") == std::string::npos);
    STUTTO_ASSERT(console_str.find("9911") == std::string::npos);
    STUTTO_ASSERT(console_str.find("driver_REDACTED.sys") != std::string::npos);
    STUTTO_ASSERT(console_str.find("nvlddmkm.sys") == std::string::npos);
    STUTTO_ASSERT(console_str.find("0xREDACTED") != std::string::npos);
    STUTTO_ASSERT(console_str.find("0xFFFFF8012A34B100") == std::string::npos);
    std::cout << "  -> Deep Redacted Console Summary sanitization validation PASSED.\n";
}

static void test_digit_boundary_tid_redaction() {
    std::cout << "[TEST] Validating Digit-Boundary TID Redaction (non-corruption of numbers containing TID digits)...\n";

    stuttometer::DiagnosticReport report;
    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.target_pid = 4444;
    report.trigger.target_tid = 12; // Short 2-digit TID
    report.target_process = "Game.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "context_switch_interference";
    // Text contains 12 as TID, but also decimal 12.5ms, 3.12ms, 12000 bytes, port 9124, 51234 offsets, and hex addresses 0xFFFFF801000012FF / 0x12ABCD00
    diag.summary = "Thread 12 blocked for 12.5ms while reading 12000 bytes with latency 3.12ms from address 9124 after 51234 ticks (driver 0xFFFFF801000012FF, routine 0x12ABCD00).";
    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;
    auto j_redacted = reporter.to_json(report, true);

    const std::string red_summary = j_redacted["diagnoses"][0]["summary"];
    // "Thread 12 " -> "Thread REDACTED "
    STUTTO_ASSERT(red_summary.find("Thread REDACTED") != std::string::npos);
    // "12.5ms" -> must NOT become "REDACTED.5ms"
    STUTTO_ASSERT(red_summary.find("12.5ms") != std::string::npos);
    // "3.12ms" -> must NOT become "3.REDACTEDms"
    STUTTO_ASSERT(red_summary.find("3.12ms") != std::string::npos);
    // "12000 bytes" -> must NOT become "REDACTED000 bytes"
    STUTTO_ASSERT(red_summary.find("12000 bytes") != std::string::npos);
    // "9124" -> must NOT become "9REDACTED4"
    STUTTO_ASSERT(red_summary.find("9124") != std::string::npos);
    // "51234" -> must NOT become "5REDACTED34"
    STUTTO_ASSERT(red_summary.find("51234") != std::string::npos);
    // "0xFFFFF801000012FF" -> must NOT become "0xFFFFF8010000REDACTEDFF"
    STUTTO_ASSERT(red_summary.find("0xFFFFF801000012FF") != std::string::npos);
    // "0x12ABCD00" -> must NOT become "0xREDACTEDABCD00"
    STUTTO_ASSERT(red_summary.find("0x12ABCD00") != std::string::npos);

    std::cout << "  -> Digit-boundary and hex-address TID redaction PASSED (numbers & addresses with TID substrings preserved).\n";
}

static void test_audio_glitch_json_serialization() {
    std::cout << "[TEST] Validating Audio Glitch Trigger JSON Serialization...\n";

    stuttometer::DiagnosticReport report;
    report.trigger.source = stuttometer::TriggerSource::AUDIO_GLITCH;
    report.trigger.glitch_count = 5;
    report.trigger.target_pid = 1234;
    report.target_process = "audiodg.exe";

    stuttometer::JsonReporter reporter;
    auto j = reporter.to_json(report, false);

    STUTTO_ASSERT(j["trigger"]["source"] == "AUDIO_GLITCH");
    STUTTO_ASSERT(j["trigger"]["glitch_count"] == 5);
    STUTTO_ASSERT(j["trigger"]["duration_ms"] == 0.0);

    std::cout << "  -> Audio Glitch trigger fields serialized correctly.\n";
}

static void test_save_to_file_non_ascii_path() {
    std::cout << "[TEST] Validating save_to_file with non-ASCII / Unicode paths...\n";

    stuttometer::DiagnosticReport report;
    report.schema_version = "1.0";
    report.tool_version = "0.1.0";
    report.target_process = "UnicodeTest.exe";

    stuttometer::JsonReporter reporter;

    // Test with std::filesystem::path containing non-ASCII / accented characters
    const std::filesystem::path utf8_dir = std::filesystem::temp_directory_path() / "stutto_árvíztűrő_test";
    const std::filesystem::path file_path = utf8_dir / "report_tést.json";

    struct DirCleanupGuard {
        std::filesystem::path dir;
        ~DirCleanupGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } dir_guard{ utf8_dir };

    std::error_code ec;
    std::filesystem::create_directories(utf8_dir, ec);

    STUTTO_ASSERT(reporter.save_to_file(report, file_path, false));
    STUTTO_ASSERT(std::filesystem::exists(file_path));
    STUTTO_ASSERT(std::filesystem::file_size(file_path) > 10);

    std::cout << "  -> Non-ASCII / Unicode file path save PASSED.\n";
}

static void test_path_and_username_pii_redaction() {
    std::cout << "[TEST] Validating Windows file path and username PII redaction...\n";

    stuttometer::DiagnosticReport report;
    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.target_pid = 5555;
    report.trigger.target_tid = 8888;
    report.target_process = "Game.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "disk_io_stall";
    diag.summary = "I/O stall reading C:\\Users\\Administrator\\Saved Games\\save.dat on TID 8888 (path D:\\Games\\Steam\\steam.exe and \\\\NAS\\Share\\data.bin).";
    
    stuttometer::EvidenceItem ev;
    ev.event_type = "DISK";
    ev.extra_info = "Accessing file \"C:\\Program Files\\Game\\level1.pak\" took 35ms";
    diag.evidence.push_back(ev);
    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;
    auto j_redacted = reporter.to_json(report, true);

    const std::string red_summary = j_redacted["diagnoses"][0]["summary"];
    // Drive letter paths and UNC paths must be replaced with [PATH_REDACTED]
    STUTTO_ASSERT(red_summary.find("C:\\Users\\Administrator\\Saved Games\\save.dat") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("D:\\Games\\Steam\\steam.exe") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("\\\\NAS\\Share\\data.bin") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("[PATH_REDACTED]") != std::string::npos);

    const std::string red_extra = j_redacted["diagnoses"][0]["evidence"][0]["extra_info"];
    STUTTO_ASSERT(red_extra.find("C:\\Program Files\\Game\\level1.pak") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("[PATH_REDACTED]") != std::string::npos);

    std::cout << "  -> Windows file path and username PII redaction PASSED.\n";
}

static void test_secondary_tid_and_pid_redaction() {
    std::cout << "[TEST] Validating secondary context-switch TID and PID PII redaction...\n";

    stuttometer::DiagnosticReport report;
    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.target_pid = 4000;
    report.trigger.target_tid = 8000;
    report.target_process = "Game.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "context_switch_interference";
    diag.summary = "Critical thread 8000 was involuntarily preempted on Core 2 (switched to TID 9999, PID 1234).";

    stuttometer::EvidenceItem ev;
    ev.event_type = "CSWITCH";
    ev.driver_module = "ntoskrnl.exe";
    ev.routine_address = "0x0000000000000000";
    ev.duration_us = 25000;
    ev.cpu_core = 2;
    ev.secondary_tid = 9999;
    ev.secondary_pid = 1234;
    ev.extra_info = "Preempted TID 8000 for TID 9999 (prev PID 1234)";
    diag.evidence.push_back(ev);
    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;
    auto j_redacted = reporter.to_json(report, true);

    const std::string red_summary = j_redacted["diagnoses"][0]["summary"];
    // Target 8000 must be redacted
    STUTTO_ASSERT(red_summary.find("8000") == std::string::npos);
    // Secondary TID 9999 and PID 1234 must be redacted
    STUTTO_ASSERT(red_summary.find("9999") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("1234") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("REDACTED") != std::string::npos);

    const std::string red_extra = j_redacted["diagnoses"][0]["evidence"][0]["extra_info"];
    STUTTO_ASSERT(red_extra.find("8000") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("9999") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("1234") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("REDACTED") != std::string::npos);

    std::cout << "  -> Secondary TID/PID deep redaction PASSED.\n";
}

static void test_case_insensitive_tid_and_pid_redaction() {
    std::cout << "[TEST] Validating case-insensitive TID, PID, and Thread marker redaction...\n";

    stuttometer::DiagnosticReport report;
    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.target_pid = 4000;
    report.trigger.target_tid = 8000;
    report.target_process = "Game.exe";

    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "context_switch_interference";
    diag.summary = "Interference from Thread 5555 and pid 7777 during frame.";

    stuttometer::EvidenceItem ev;
    ev.event_type = "CSWITCH";
    ev.extra_info = "tid 5555 preempted tid 8000 on core 1";
    diag.evidence.push_back(ev);
    report.diagnoses.push_back(diag);

    stuttometer::JsonReporter reporter;
    auto j_redacted = reporter.to_json(report, true);

    const std::string red_summary = j_redacted["diagnoses"][0]["summary"];
    STUTTO_ASSERT(red_summary.find("5555") == std::string::npos);
    STUTTO_ASSERT(red_summary.find("7777") == std::string::npos);

    const std::string red_extra = j_redacted["diagnoses"][0]["evidence"][0]["extra_info"];
    STUTTO_ASSERT(red_extra.find("5555") == std::string::npos);
    STUTTO_ASSERT(red_extra.find("8000") == std::string::npos);

    std::cout << "  -> Case-insensitive TID/PID redaction PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer JSON Schema & Redaction Tests ===\n";
    try {
        test_json_serialization_and_deep_redaction();
        test_digit_boundary_tid_redaction();
        test_secondary_tid_and_pid_redaction();
        test_case_insensitive_tid_and_pid_redaction();
        test_path_and_username_pii_redaction();
        test_audio_glitch_json_serialization();
        test_save_to_file_non_ascii_path();
        std::cout << ">>> All JSON Schema tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}

