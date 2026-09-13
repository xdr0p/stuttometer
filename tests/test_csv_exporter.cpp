#include "test_common.hpp"
#include "stuttometer/csv_exporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>

static void test_csv_export_and_crlf() {
    std::cout << "[TEST] Validating CSV Export and RFC 4180 CRLF format...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_csv_crlf";
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path csv_path = test_dir / "sample_pacing.csv";

    stuttometer::DiagnosticReport report;
    stuttometer::FrameTimelinePoint p1;
    p1.frame_index = 0;
    p1.relative_index = -1;
    p1.qpc_timestamp = 1000000;
    p1.duration_ms = 16.6667;
    p1.offset_from_trigger_ms = -16.6667;
    p1.is_trigger_frame = false;
    p1.is_pacing_stall = false;

    stuttometer::FrameTimelinePoint p2;
    p2.frame_index = 1;
    p2.relative_index = 0;
    p2.qpc_timestamp = 1100000;
    p2.duration_ms = 45.1234;
    p2.offset_from_trigger_ms = 0.0;
    p2.is_trigger_frame = true;
    p2.is_pacing_stall = true;

    report.frame_timeline.push_back(p1);
    report.frame_timeline.push_back(p2);

    bool ok = stuttometer::csv::export_to_file(report, csv_path);
    STUTTO_ASSERT(ok);

    // Read back in strict binary mode
    std::ifstream in(csv_path, std::ios::binary);
    STUTTO_ASSERT(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Verify CRLF: each line must end with \r\n, no \r\r\n
    size_t cr_count = 0;
    size_t lf_count = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            cr_count++;
            STUTTO_ASSERT(i + 1 < content.size() && content[i + 1] == '\n');
        } else if (content[i] == '\n') {
            lf_count++;
            STUTTO_ASSERT(i > 0 && content[i - 1] == '\r');
        }
    }
    STUTTO_ASSERT(cr_count == 3); // header + 2 rows
    STUTTO_ASSERT(lf_count == 3);
    STUTTO_ASSERT(content.find("\r\r\n") == std::string::npos);

    // Check header
    const std::string expected_header = "frame_index,relative_index,qpc_timestamp,duration_ms,offset_from_trigger_ms,is_trigger_frame,is_pacing_stall\r\n";
    STUTTO_ASSERT(content.rfind(expected_header, 0) == 0);

    // Check data row contents
    STUTTO_ASSERT(content.find("0,-1,1000000,16.6667,-16.6667,false,false\r\n") != std::string::npos);
    STUTTO_ASSERT(content.find("1,0,1100000,45.1234,0.0000,true,true\r\n") != std::string::npos);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> CSV CRLF and value formatting PASSED.\n";
}

static void test_directory_rotation() {
    std::cout << "[TEST] Validating Neutral Directory Rotation Retention Cap...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_csv_rot";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    // Create 105 files with prefix stutto_pacing_ and .csv extension
    // Assign varying last_write_time to ensure deterministic age ordering
    auto now = std::filesystem::file_time_type::clock::now();
    for (int i = 0; i < 105; ++i) {
        std::filesystem::path fpath = test_dir / ("stutto_pacing_" + std::to_string(i) + ".csv");
        std::ofstream out(fpath, std::ios::binary);
        out << "test";
        out.close();
        auto ftime = now - std::chrono::minutes(105 - i); // oldest has largest negative offset
        std::filesystem::last_write_time(fpath, ftime);
    }

    // Also create 2 unrelated files that should NOT be rotated
    {
        std::ofstream out(test_dir / "unrelated.csv", std::ios::binary);
        out << "keep";
    }
    {
        std::ofstream out(test_dir / "stutto_report_1.json", std::ios::binary);
        out << "keep";
    }

    // Retain 100
    stuttometer::rotate_directory_by_prefix(test_dir, "stutto_pacing_", ".csv", 100);

    // Verify 100 matching files remain
    size_t matching_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".csv") {
            const std::string fname = entry.path().filename().string();
            if (fname.rfind("stutto_pacing_", 0) == 0) {
                matching_count++;
            }
        }
    }
    STUTTO_ASSERT(matching_count == 100);

    // Oldest 5 (i = 0, 1, 2, 3, 4) should have been deleted
    for (int i = 0; i < 5; ++i) {
        std::filesystem::path old_path = test_dir / ("stutto_pacing_" + std::to_string(i) + ".csv");
        STUTTO_ASSERT(!std::filesystem::exists(old_path));
    }
    // Newer files (i = 5 .. 104) must still exist
    for (int i = 5; i < 105; ++i) {
        std::filesystem::path keep_path = test_dir / ("stutto_pacing_" + std::to_string(i) + ".csv");
        STUTTO_ASSERT(std::filesystem::exists(keep_path));
    }

    // Unrelated files must still exist
    STUTTO_ASSERT(std::filesystem::exists(test_dir / "unrelated.csv"));
    STUTTO_ASSERT(std::filesystem::exists(test_dir / "stutto_report_1.json"));

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> Directory rotation retention cap PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer CSV Exporter Unit Tests ===\n";
    try {
        test_csv_export_and_crlf();
        test_directory_rotation();
        std::cout << ">>> All CSV Exporter tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
