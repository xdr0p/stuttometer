#include "test_common.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "nlohmann/json.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

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

static void test_ndjson_idle_wakeup_and_stop() {
    std::cout << "[TEST] Validating NDJSON idle wakeup, pause/resume, and instant stop...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_idle";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "idle_test.ndjson";

    auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 10 * 1024 * 1024, 3);
    STUTTO_ASSERT(writer != nullptr);

    // Let the worker enter its idle state (wait on resume_cv_ with 2ms timeout)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 1. Verify pause_worker_for_test wakes worker instantly and pauses it
    auto t0 = std::chrono::steady_clock::now();
    writer->pause_worker_for_test();
    auto t_pause = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    STUTTO_ASSERT(t_pause < 100); // Should be nearly instantaneous

    // 2. Push an event while paused
    stuttometer::EtwEventRecord rec{};
    rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGI);
    rec.event_id = 43;
    rec.pid = 1234;
    rec.qpc_timestamp = 100;
    STUTTO_ASSERT(writer->push(rec));

    auto wait_for_written = [&](uint64_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (writer->written_records() < expected && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // 3. Resume worker and verify it drains
    writer->resume_worker_for_test();
    wait_for_written(1);
    STUTTO_ASSERT(writer->written_records() == 1);

    // 4. Let worker go idle again (sleep 10ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Push after idle resets idle_spins and writes within 2ms wait_for timeout
    rec.qpc_timestamp = 200;
    STUTTO_ASSERT(writer->push(rec));
    wait_for_written(2);
    STUTTO_ASSERT(writer->written_records() == 2);

    // 5. Verify stop() wakes sleeping worker instantly
    t0 = std::chrono::steady_clock::now();
    writer->stop();
    auto t_stop = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    STUTTO_ASSERT(t_stop < 100); // Instant shutdown without hang

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> NDJSON idle wakeup, pause/resume, and stop PASSED.\n";
}

static void test_ndjson_max_files_zero_clamping() {
    std::cout << "[TEST] Validating NDJSON max_files == 0 clamps to 1...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_clamp";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::filesystem::path file_path = test_dir / "clamp.ndjson";

    // Pass max_files = 0
    auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 2048, 0);
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
    // No .1 files should exist
    STUTTO_ASSERT(!std::filesystem::exists(file_path.string() + ".1"));
    STUTTO_ASSERT(writer->written_records() == 100);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> NDJSON max_files == 0 clamped to 1 PASSED.\n";
}

static void test_ndjson_rotation_failure_dropped_records() {
    std::cout << "[TEST] Validating dropped records on rotation file reopen failure...\n";

    const std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "stutto_test_ndjson_rot_fail";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);
    const std::filesystem::path file_path = test_dir / "rot_fail.ndjson";

    // Small max_bytes = 200 bytes so rotation occurs on second record, max_files = 1
    auto writer = stuttometer::NdjsonWriter::create_for_file(file_path, 200, 1);
    STUTTO_ASSERT(writer != nullptr);

    stuttometer::EtwEventRecord rec{};
    rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DISK);
    rec.event_id = 10;
    rec.pid = 1234;
    rec.qpc_timestamp = 1;
    writer->push(rec);

    // Wait until 1st record is written
    while (writer->written_records() == 0) {
        std::this_thread::yield();
    }

    // Mark file as read-only so that when rotation closes and reopens with "wb", _wfopen will fail with access denied
#ifdef _WIN32
    SetFileAttributesW(file_path.c_str(), FILE_ATTRIBUTE_READONLY);
#else
    std::filesystem::permissions(file_path, std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read, std::filesystem::perm_options::replace, ec);
#endif

    // Push more records to exceed 200 bytes and force rotation
    for (size_t i = 2; i <= 20; ++i) {
        rec.qpc_timestamp = i;
        writer->push(rec);
    }

    // Give worker time to process and encounter rotation reopen failure
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    writer->stop();

#ifdef _WIN32
    SetFileAttributesW(file_path.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif

    // Records pushed after rotation failure must be recorded as dropped due to file_ == nullptr
    STUTTO_ASSERT(writer->dropped_records() > 0);

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  -> Dropped records on rotation reopen failure PASSED (" 
              << writer->dropped_records() << " dropped).\n";
}

int main() {
    std::cout << "=== Stuttometer NDJSON Writer Unit Tests ===\n";
    try {
        test_ndjson_line_schema();
        test_deterministic_saturation();
        test_mpsc_concurrency_stress();
        test_file_rotation();
        test_ndjson_idle_wakeup_and_stop();
        test_ndjson_max_files_zero_clamping();
        test_ndjson_rotation_failure_dropped_records();
        std::cout << ">>> All NDJSON Writer tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
