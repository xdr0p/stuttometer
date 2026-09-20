#include "test_common.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/session_benchmark.hpp"
#include "stuttometer/frame_pacing_tracker.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>

static void test_schema_1_1_roundtrip_unredacted() {
    std::cout << "[TEST] Validating Schema 1.1 JSON Serialization Round-Trip (Unredacted)...\n";

    stuttometer::JsonReporter reporter;
    stuttometer::DiagnosticReport report;

    report.schema_version = "1.1";
    report.tool_version = "0.2.0";
    report.timestamp_utc = "2026-09-14T01:00:00.000Z";
    report.target_process = "Cyberpunk2077.exe";
    report.window_pre_ms = 250.0;
    report.window_post_ms = 30.0;
    report.present_threshold_ms = 16.67;
    report.provider_tier = "standard";
    report.redacted = false;

    // Trigger info
    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.reason = stuttometer::TriggerReason::STATIC_THRESHOLD;
    report.trigger.trigger_timestamp_qpc = 12345678900ULL;
    report.trigger.target_pid = 4321;
    report.trigger.target_tid = 8765;
    report.trigger.cpu_index = 3;
    report.trigger.duration_ms = 45.5;
    report.trigger.baseline_avg_ms = 16.6;
    report.trigger.baseline_fps = 60.2;
    report.trigger.spike_ratio = 2.74;

    // Attribution
    report.attribution = stuttometer::AttributionTag::EXTERNAL_CONTENTION;
    report.attribution_pid = 4;
    report.attribution_process = "nvlddmkm.sys (System)";
    report.attribution_redacted = false;

    // Diagnosis & Evidence
    stuttometer::Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "dpc_isr_spike";
    diag.confidence = 0.88;
    diag.summary = "DPC routine in nvlddmkm.sys executed for 3.5ms on CPU 3.";
    diag.factors = { 0.85, 0.90, 0.95 };

    stuttometer::EvidenceItem ev;
    ev.event_type = "DPC";
    ev.driver_module = "nvlddmkm.sys";
    ev.routine_address = "0xFFFFF80012345678";
    ev.duration_us = 3500;
    ev.cpu_core = 3;
    ev.offset_from_trigger_ms = -5.0;
    ev.pid = 4;
    diag.evidence.push_back(ev);
    report.diagnoses.push_back(diag);

    // Frame timeline
    stuttometer::FrameTimelinePoint p1;
    p1.frame_index = 0;
    p1.relative_index = -1;
    p1.qpc_timestamp = 12345500000ULL;
    p1.duration_ms = 16.6667;
    p1.offset_from_trigger_ms = -16.6667;
    p1.is_trigger_frame = false;
    p1.is_pacing_stall = false;

    stuttometer::FrameTimelinePoint p2;
    p2.frame_index = 1;
    p2.relative_index = 0;
    p2.qpc_timestamp = 12345678900ULL;
    p2.duration_ms = 45.5;
    p2.offset_from_trigger_ms = 0.0;
    p2.is_trigger_frame = true;
    p2.is_pacing_stall = true;

    report.frame_timeline.push_back(p1);
    report.frame_timeline.push_back(p2);

    // Serialize to string
    std::string json_str = reporter.to_json_string(report, false, 2);
    STUTTO_ASSERT(!json_str.empty());

    // Parse back
    auto root = nlohmann::json::parse(json_str);

    STUTTO_ASSERT(root["schema_version"] == "1.1");
    STUTTO_ASSERT(root["tool_version"] == "0.2.0");
    STUTTO_ASSERT(root["timestamp_utc"] == "2026-09-14T01:00:00.000Z");

    // Attribution
    STUTTO_ASSERT(root.contains("attribution"));
    STUTTO_ASSERT(root["attribution"]["tag"] == "EXTERNAL_CONTENTION");
    STUTTO_ASSERT(root["attribution"]["pid"] == 4);
    STUTTO_ASSERT(root["attribution"]["process"] == "nvlddmkm.sys (System)");

    // Trigger
    STUTTO_ASSERT(root["trigger"]["target_pid"] == 4321);
    STUTTO_ASSERT(root["trigger"]["target_tid"] == 8765);
    STUTTO_ASSERT(root["trigger"]["target_process"] == "Cyberpunk2077.exe");

    // Evidence PID
    STUTTO_ASSERT(root["diagnoses"][0]["evidence"][0]["pid"] == 4);

    // Frame timeline
    STUTTO_ASSERT(root.contains("frame_timeline"));
    STUTTO_ASSERT(root["frame_timeline"].is_array());
    STUTTO_ASSERT(root["frame_timeline"].size() == 2);

    const auto& pt0 = root["frame_timeline"][0];
    STUTTO_ASSERT(pt0["frame_index"] == 0);
    STUTTO_ASSERT(pt0["relative_index"] == -1);
    STUTTO_ASSERT(pt0["qpc_timestamp"] == 12345500000ULL);
    STUTTO_ASSERT(!pt0["is_trigger_frame"]);
    STUTTO_ASSERT(!pt0["is_pacing_stall"]);

    const auto& pt1 = root["frame_timeline"][1];
    STUTTO_ASSERT(pt1["frame_index"] == 1);
    STUTTO_ASSERT(pt1["relative_index"] == 0);
    STUTTO_ASSERT(pt1["is_trigger_frame"]);
    STUTTO_ASSERT(pt1["is_pacing_stall"]);

    std::cout << "  -> Unredacted schema 1.1 round-trip PASSED.\n";
}

static void test_schema_1_1_roundtrip_redacted() {
    std::cout << "[TEST] Validating Schema 1.1 JSON Serialization (Redacted)...\n";

    stuttometer::JsonReporter reporter;
    stuttometer::DiagnosticReport report;

    report.schema_version = "1.1";
    report.tool_version = "0.2.0";
    report.target_process = "SensitiveGame.exe";

    report.trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.target_pid = 7777;
    report.trigger.target_tid = 8888;

    report.attribution = stuttometer::AttributionTag::GAME_ENGINE;
    report.attribution_pid = 7777;
    report.attribution_process = "SensitiveGame.exe";
    report.attribution_redacted = true;

    stuttometer::Diagnosis diag;
    diag.hypothesis = "gpu_pipeline_stall";
    diag.confidence = 0.90;
    stuttometer::EvidenceItem ev;
    ev.event_type = "DXGI";
    ev.pid = 7777;
    diag.evidence.push_back(ev);
    report.diagnoses.push_back(diag);

    std::string json_str = reporter.to_json_string(report, true, 2);
    auto root = nlohmann::json::parse(json_str);

    STUTTO_ASSERT(root["configuration"]["redacted"] == true);
    STUTTO_ASSERT(root["attribution"]["tag"] == "GAME_ENGINE");
    STUTTO_ASSERT(root["attribution"]["pid"] == 0);
    STUTTO_ASSERT(root["attribution"]["process"] == "REDACTED");

    STUTTO_ASSERT(root["trigger"]["target_pid"] == 0);
    STUTTO_ASSERT(root["trigger"]["target_tid"] == 0);
    STUTTO_ASSERT(root["trigger"]["target_process"] == "Process_REDACTED");

    STUTTO_ASSERT(root["diagnoses"][0]["evidence"][0]["pid"] == 0);

    std::cout << "  -> Redacted schema 1.1 PASSED.\n";
}

static void test_file_save_and_load() {
    std::cout << "[TEST] Validating JSON File Save & Re-load Consistency...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_json_save";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "report.json";

    stuttometer::JsonReporter reporter;
    stuttometer::DiagnosticReport report;
    report.schema_version = "1.1";
    report.tool_version = "0.2.0";
    report.attribution = stuttometer::AttributionTag::DWM_COMPOSITION;
    report.attribution_pid = 444;
    report.attribution_process = "dwm.exe";

    bool saved = reporter.save_to_file(report, file_path, false);
    STUTTO_ASSERT(saved);
    STUTTO_ASSERT(std::filesystem::exists(file_path));

    std::ifstream in(file_path);
    STUTTO_ASSERT(in.is_open());
    auto root = nlohmann::json::parse(in);
    in.close();

    STUTTO_ASSERT(root["schema_version"] == "1.1");
    STUTTO_ASSERT(root["tool_version"] == "0.2.0");
    STUTTO_ASSERT(root["attribution"]["tag"] == "DWM_COMPOSITION");
    STUTTO_ASSERT(root["attribution"]["pid"] == 444);
    STUTTO_ASSERT(root["attribution"]["process"] == "dwm.exe");

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> JSON file save and reload PASSED.\n";
}

static void test_schema_1_2_benchmark_summary_serialization() {
    std::cout << "[TEST] Validating Schema 1.2 JSON Serialization (BenchmarkSummary)...\n";

    stuttometer::BenchmarkSummary summary;
    summary.target_process = "Cyberpunk2077.exe";
    summary.target_pid = 4321;
    summary.duration_ms = 60000.0;
    summary.total_frames = 3600;
    summary.stutters_detected = 2;
    summary.dropped_pause_frames = 0;
    summary.redacted = false;
    summary.frametimes.avg_fps = 60.0;
    summary.frametimes.p99_frametime_ms = 18.2;
    summary.frametimes.low_1pct_fps = 54.9;
    summary.frametimes.p999_frametime_ms = 25.0;
    summary.frametimes.low_01pct_fps = 40.0;
    summary.frametimes.max_frametime_ms = 45.5;
    summary.net_stall_ms = 35.0;
    summary.worst_stutter_ms = 45.5;
    summary.worst_stutter_hypothesis = "dpc_isr_spike";

    // Cadence fields for State 1 / State 2
    summary.pacing_profile = stuttometer::PacingProfile::HIGH_REFRESH;
    summary.binding_floor_source = stuttometer::BindingFloorSource::DYNAMIC;
    summary.rolling_baseline_ms = 7.14;
    summary.rolling_fps = 140.0;
    summary.estimated_dynamic_floor_ms = 11.0;
    summary.static_floor_ms = 17.5;
    summary.estimated_binding_floor_ms = 11.0;

    std::string json_str = summary.to_json();
    STUTTO_ASSERT(!json_str.empty());

    auto root = nlohmann::json::parse(json_str);

    STUTTO_ASSERT(root["schema_version"] == "1.2");
    STUTTO_ASSERT(root["target_process"] == "Cyberpunk2077.exe");
    STUTTO_ASSERT(root["target_pid"] == 4321);
    STUTTO_ASSERT(root["total_frames"] == 3600);

    // Presentation cadence validation
    STUTTO_ASSERT(root.contains("presentation_cadence"));
    const auto& cad = root["presentation_cadence"];
    STUTTO_ASSERT(cad["pacing_profile"] == "high_refresh");
    STUTTO_ASSERT(cad["binding_floor_source"] == "dynamic");
    STUTTO_ASSERT(std::abs(cad["rolling_baseline_ms"].get<double>() - 7.14) < 1e-4);
    STUTTO_ASSERT(std::abs(cad["rolling_fps"].get<double>() - 140.0) < 1e-4);
    STUTTO_ASSERT(std::abs(cad["estimated_dynamic_floor_ms"].get<double>() - 11.0) < 1e-4);
    STUTTO_ASSERT(std::abs(cad["static_floor_ms"].get<double>() - 17.5) < 1e-4);
    STUTTO_ASSERT(std::abs(cad["estimated_binding_floor_ms"].get<double>() - 11.0) < 1e-4);

    // State 3 (Warmup / Empty Session / NONE) cadence validation
    summary.binding_floor_source = stuttometer::BindingFloorSource::NONE;
    std::string s3_json_str = summary.to_json();
    auto s3_root = nlohmann::json::parse(s3_json_str);
    const auto& s3_cad = s3_root["presentation_cadence"];
    STUTTO_ASSERT(s3_cad["pacing_profile"] == "high_refresh");
    STUTTO_ASSERT(s3_cad["binding_floor_source"] == "none");
    STUTTO_ASSERT(s3_cad["estimated_dynamic_floor_ms"].is_null());
    STUTTO_ASSERT(s3_cad["estimated_binding_floor_ms"].is_null());
    STUTTO_ASSERT(s3_cad["rolling_baseline_ms"].is_null());
    STUTTO_ASSERT(s3_cad["rolling_fps"].is_null());
    STUTTO_ASSERT(std::abs(s3_cad["static_floor_ms"].get<double>() - 17.5) < 1e-4);

    std::cout << "  -> Schema 1.2 BenchmarkSummary JSON serialization PASSED.\n";
}

static void test_cli_to_json_profile_string_mapping() {
    std::cout << "[TEST] Validating CLI String -> PacingProfile Enum -> JSON String Mapping...\n";

    // "high-refresh" -> HIGH_REFRESH -> "high_refresh"
    auto opt_hr = stuttometer::pacing_profile_from_cli_string("high-refresh");
    STUTTO_ASSERT(opt_hr.has_value());
    STUTTO_ASSERT(*opt_hr == stuttometer::PacingProfile::HIGH_REFRESH);
    STUTTO_ASSERT(stuttometer::pacing_profile_to_string(*opt_hr) == "high_refresh");

    // "auto" -> AUTO_ADAPTIVE -> "auto_adaptive"
    auto opt_auto = stuttometer::pacing_profile_from_cli_string("auto");
    STUTTO_ASSERT(opt_auto.has_value());
    STUTTO_ASSERT(*opt_auto == stuttometer::PacingProfile::AUTO_ADAPTIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_to_string(*opt_auto) == "auto_adaptive");

    // "conservative" -> CONSERVATIVE -> "conservative"
    auto opt_cons = stuttometer::pacing_profile_from_cli_string("conservative");
    STUTTO_ASSERT(opt_cons.has_value());
    STUTTO_ASSERT(*opt_cons == stuttometer::PacingProfile::CONSERVATIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_to_string(*opt_cons) == "conservative");

    // "custom" is rejected on CLI
    auto opt_custom = stuttometer::pacing_profile_from_cli_string("custom");
    STUTTO_ASSERT(!opt_custom.has_value());

    // But "custom" enum serializes to "custom" in JSON
    STUTTO_ASSERT(stuttometer::pacing_profile_to_string(stuttometer::PacingProfile::CUSTOM) == "custom");

    // Deserialization via pacing_profile_from_string (used for JSON & settings.json)
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("high_refresh") == stuttometer::PacingProfile::HIGH_REFRESH);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("high-refresh") == stuttometer::PacingProfile::HIGH_REFRESH);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("auto_adaptive") == stuttometer::PacingProfile::AUTO_ADAPTIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("auto-adaptive") == stuttometer::PacingProfile::AUTO_ADAPTIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("auto") == stuttometer::PacingProfile::AUTO_ADAPTIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("conservative") == stuttometer::PacingProfile::CONSERVATIVE);
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("custom") == stuttometer::PacingProfile::CUSTOM);
    // Unrecognized strings default to AUTO_ADAPTIVE
    STUTTO_ASSERT(stuttometer::pacing_profile_from_string("unknown_future_profile") == stuttometer::PacingProfile::AUTO_ADAPTIVE);

    std::cout << "  -> CLI String -> Enum -> JSON String Mapping PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Report Serialization Consistency Tests ===\n";
    try {
        test_schema_1_1_roundtrip_unredacted();
        test_schema_1_1_roundtrip_redacted();
        test_file_save_and_load();
        test_schema_1_2_benchmark_summary_serialization();
        test_cli_to_json_profile_string_mapping();
        std::cout << ">>> All Report Serialization Consistency tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
