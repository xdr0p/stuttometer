#include "test_common.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "nlohmann/json.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <filesystem>
#include <string>

static void test_ndjson_line_schema() {
    std::cout << "[TEST] Validating NDJSON Line Schema...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_schema";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "schema_test.ndjson";

    {
        auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 10 * 1024 * 1024, 3);
        STUTTO_ASSERT(writer != nullptr);

        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
        rec.event_id = 43;
        rec.pid = 4321;
        rec.tid = 8765;
        rec.cpu_index = 2;
        rec.duration_us = 16670;
        rec.qpc_timestamp = 9876543210ULL;
        rec.auxiliary_data = 12345ULL;
        rec.flags = 7;

        bool pushed = writer->push(rec);
        STUTTO_ASSERT(pushed);
        writer->stop();
    }

    // Read back and parse JSON
    std::ifstream in(file_path);
    STUTTO_ASSERT(in.is_open());
    std::string line;
    STUTTO_ASSERT(std::getline(in, line));
    in.close();

    auto j = nlohmann::json::parse(line);
    STUTTO_ASSERT(j["v"] == stuttometer::NDJSON_LINE_SCHEMA_VERSION);
    STUTTO_ASSERT(j["v"] == 1);
    STUTTO_ASSERT(j["ts_qpc"] == 9876543210ULL);
    STUTTO_ASSERT(j["cat"] == "DXGI");
    STUTTO_ASSERT(j["id"] == 43);
    STUTTO_ASSERT(j["pid"] == 4321);
    STUTTO_ASSERT(j["tid"] == 8765);
    STUTTO_ASSERT(j["cpu"] == 2);
    STUTTO_ASSERT(j["dur_us"] == 16670);
    STUTTO_ASSERT(j["aux"] == 12345ULL);
    STUTTO_ASSERT(j["flags"] == 7);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> NDJSON schema verification PASSED.\n";
}

static void test_deterministic_saturation() {
    std::cout << "[TEST] Validating Deterministic Saturation (65536 capacity)...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_sat";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "saturation.ndjson";

    auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 100 * 1024 * 1024, 3);
    STUTTO_ASSERT(writer != nullptr);

    // Pause worker thread before any pushes occur
    writer->pause_worker_for_test();

    stuttometer::EtwEventRecord rec{};
    rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    rec.event_id = 43;
    rec.pid = 1000;

    // Push exactly 65,536 records into the un-drained ring buffer
    for (size_t i = 0; i < 65536; ++i) {
        rec.qpc_timestamp = i;
        bool ok = writer->push(rec);
        if (!ok) {
            std::cerr << "Failed push at index: " << i << "\n";
        }
        STUTTO_ASSERT(ok);
    }

    // Next 100 pushes MUST fail and increment dropped_records_
    for (size_t i = 0; i < 100; ++i) {
        rec.qpc_timestamp = 65536 + i;
        bool ok = writer->push(rec);
        STUTTO_ASSERT(!ok);
    }

    STUTTO_ASSERT(writer->dropped_records() == 100);

    // Resume worker thread to drain buffer
    writer->resume_worker_for_test();

    // Stop synchronously drains all 65,536 records and closes file
    writer->stop();

    STUTTO_ASSERT(writer->written_records() == 65536);
    STUTTO_ASSERT(writer->dropped_records() == 100);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> Deterministic saturation PASSED (65536 written, 100 dropped).\n";
}

static void test_mpsc_concurrency_stress() {
    std::cout << "[TEST] Validating MPSC Concurrency Stress (8 threads x 5000 records)...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_mpsc";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "mpsc_stress.ndjson";

    auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 100 * 1024 * 1024, 3);
    STUTTO_ASSERT(writer != nullptr);

    constexpr size_t THREAD_COUNT = 8;
    constexpr size_t RECORDS_PER_THREAD = 5000;

    std::vector<std::thread> producers;
    producers.reserve(THREAD_COUNT);

    for (size_t t = 0; t < THREAD_COUNT; ++t) {
        producers.emplace_back([&writer, t]() {
            stuttometer::EtwEventRecord rec{};
            rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DPC);
            rec.event_id = 1;
            rec.pid = 4;
            rec.cpu_index = static_cast<uint8_t>(t);
            for (size_t i = 0; i < RECORDS_PER_THREAD; ++i) {
                rec.qpc_timestamp = (t * 1000000) + i;
                writer->push(rec);
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    writer->stop();

    const uint64_t written = writer->written_records();
    const uint64_t dropped = writer->dropped_records();
    STUTTO_ASSERT(written + dropped == THREAD_COUNT * RECORDS_PER_THREAD);
    STUTTO_ASSERT(written > 0);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> MPSC concurrency stress PASSED (" << written << " written, " << dropped << " dropped).\n";
}

static void test_file_rotation() {
    std::cout << "[TEST] Validating File Rotation bounded to max_files...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_rot";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "trace.ndjson";

    // 1. max_files = 3, small max_bytes = 4096 bytes (~30 lines)
    {
        auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 4096, 3);
        STUTTO_ASSERT(writer != nullptr);

        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
        rec.event_id = 10;
        rec.pid = 1234;

        // Push enough records to cause multiple rotations (> 200 records * ~130 bytes > 26KB)
        for (size_t i = 0; i < 200; ++i) {
            rec.qpc_timestamp = i;
            writer->push(rec);
        }

        writer->stop();

        // Check files on disk: trace.ndjson, trace.ndjson.1, trace.ndjson.2 should exist
        // trace.ndjson.3 must NOT exist
        STUTTO_ASSERT(std::filesystem::exists(file_path));
        STUTTO_ASSERT(std::filesystem::exists(file_path.string() + ".1"));
        STUTTO_ASSERT(std::filesystem::exists(file_path.string() + ".2"));
        STUTTO_ASSERT(!std::filesystem::exists(file_path.string() + ".3"));
    }

    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    // 2. max_files = 2: Exactly 2 files (trace.ndjson, trace.ndjson.1), no trace.ndjson.2
    {
        auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 4096, 2);
        STUTTO_ASSERT(writer != nullptr);

        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
        rec.event_id = 10;
        rec.pid = 1234;

        for (size_t i = 0; i < 200; ++i) {
            rec.qpc_timestamp = i;
            writer->push(rec);
        }

        writer->stop();

        STUTTO_ASSERT(std::filesystem::exists(file_path));
        STUTTO_ASSERT(std::filesystem::exists(file_path.string() + ".1"));
        STUTTO_ASSERT(!std::filesystem::exists(file_path.string() + ".2"));
    }

    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    // 3. max_files = 1: Truncation behavior, no .1 files created
    {
        auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 2048, 1);
        STUTTO_ASSERT(writer != nullptr);

        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
        rec.event_id = 10;
        rec.pid = 1234;

        for (size_t i = 0; i < 100; ++i) {
            rec.qpc_timestamp = i;
            writer->push(rec);
        }

        writer->stop();

        STUTTO_ASSERT(std::filesystem::exists(file_path));
        STUTTO_ASSERT(!std::filesystem::exists(file_path.string() + ".1"));
        // Final file size should be within bounds
        auto sz = std::filesystem::file_size(file_path);
        STUTTO_ASSERT(sz <= 3000); // within 1-2 lines of 2048
    }

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> File rotation bounding PASSED (max_files = 3, 2, 1).\n";
}

int main() {
    std::cout << "=== Stuttometer NDJSON Writer Unit Tests ===\n";
    try {
        test_ndjson_line_schema();
        test_deterministic_saturation();
        test_mpsc_concurrency_stress();
        test_file_rotation();
        std::cout << ">>> All NDJSON Writer tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
