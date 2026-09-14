#pragma once

#include "event_types.hpp"
#include <atomic>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <condition_variable>
#include <mutex>

namespace stuttometer {

inline constexpr uint32_t NDJSON_LINE_SCHEMA_VERSION = 1;

class NdjsonWriter {
public:
    // Factory for owning file writer (performs rotation, calls fclose on destroy)
    // Returns nullptr on file open failure.
    static std::unique_ptr<NdjsonWriter> create_for_file(
        const std::filesystem::path& base_path,
        size_t max_bytes_per_file = 100 * 1024 * 1024,
        size_t max_files = 3
    );

    // Factory for borrowing stream writer (writes to stream, never rotates, never calls fclose)
    static std::unique_ptr<NdjsonWriter> create_for_stream(std::FILE* stream);

    ~NdjsonWriter();

    // Enqueue an event record. Non-blocking, allocation-free, lock-free on hot path.
    // Returns false and increments dropped_records_ on queue saturation.
    // If stopped, returns false without incrementing dropped_records_.
    bool push(const EtwEventRecord& record) noexcept;

    // Callers must ensure all producer threads have stopped calling push() before invoking stop().
    // Synchronously flushes remaining ring buffer events and closes/flushes target.
    void stop() noexcept;

    [[nodiscard]] uint64_t written_records() const noexcept { return written_records_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_records() const noexcept { return dropped_records_.load(std::memory_order_relaxed); }

    // Test inspection hooks for deterministic saturation testing.
    // pause_worker_for_test() must be invoked before producers begin pushing.
    void pause_worker_for_test() noexcept;
    void resume_worker_for_test() noexcept;

private:
    NdjsonWriter(std::FILE* file, bool owns_file, std::filesystem::path base_path, size_t max_bytes, size_t max_files);

    void worker_loop();
    void rotate_files();

    static constexpr size_t RING_CAPACITY = 65536; // Must be power of 2
    static constexpr size_t RING_MASK = RING_CAPACITY - 1;

    struct Cell {
        std::atomic<size_t> sequence{0};
        EtwEventRecord record{};
    };

    std::unique_ptr<Cell[]> ring_;
    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};

    std::FILE* file_{nullptr};
    bool owns_file_{false};
    std::filesystem::path base_path_;
    size_t max_bytes_{0};
    size_t max_files_{3};
    size_t current_file_bytes_{0};
    std::atomic<bool> rotation_error_logged_{false};

    std::atomic<bool> running_{true};
    std::atomic<bool> stopped_{false};
    std::atomic<uint64_t> written_records_{0};
    std::atomic<uint64_t> dropped_records_{0};

    std::thread worker_thread_;

    // Test pause synchronization
    std::atomic<bool> test_paused_{false};
    std::atomic<bool> worker_is_paused_{false};
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    std::condition_variable resume_cv_;
};

} // namespace stuttometer
