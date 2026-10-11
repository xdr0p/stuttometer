#pragma once

#include "event_types.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstddef>

namespace stuttometer {

static_assert(offsetof(EtwEventRecord, qpc_timestamp) == 0,
              "FlightRecorder fast-path requires qpc_timestamp at offset 0 (Slot offset 8)");

// Default capacity: 2^18 = 262,144 slots (strictly 16.77 MB)
inline constexpr size_t DEFAULT_RING_CAPACITY = 262144;

class FlightRecorder {
public:
    // Slot is strictly 64 bytes (1 CPU cache line)
    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0}; // Offset 0 (8 bytes)
        EtwEventRecord record{};           // Offset 8 (56 bytes)
    };
    static_assert(sizeof(Slot) == 64, "Slot must be strictly 64 bytes");

    explicit FlightRecorder(size_t capacity = DEFAULT_RING_CAPACITY, uint64_t qpc_freq = 0);
    ~FlightRecorder() = default;

    // Non-copyable, non-movable (pinned memory)
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // Push an event record into the ring buffer (lock-free, multi-producer safe)
    void push(const EtwEventRecord& event) noexcept;

    // Snapshot records within the given QPC timestamp window [from_qpc, to_qpc].
    // Extracted records are sorted chronologically by qpc_timestamp.
    // out_dropped_count: optional pointer receiving the count of reader-observed dropped
    // events in this snapshot (slots lapped by newer writes or torn mid-copy).
    std::vector<EtwEventRecord> snapshot(uint64_t from_qpc, uint64_t to_qpc, uint64_t* out_dropped_count = nullptr) const;

    // In-place snapshot: snapshot() clears out_records and reserves up to 4096 entries if its
    // capacity is smaller, preserving the underlying buffer across calls. Callers that reuse
    // the same vector across invocations avoid heap reallocation after the first invocation.
    void snapshot(
        uint64_t from_qpc,
        uint64_t to_qpc,
        std::vector<EtwEventRecord>& out_records,
        uint64_t* out_dropped_count = nullptr
    ) const;

    // Diagnostics & Statistics
    uint64_t current_head() const noexcept { return head_.load(std::memory_order_relaxed); }
    uint64_t total_dropped_events() const noexcept { return dropped_events_.load(std::memory_order_relaxed); }
    size_t capacity() const noexcept { return capacity_; }

    // Test-only diagnostics. Valid on the same thread that most recently
    // called snapshot(); cross-thread queries may observe stale values.
    uint64_t last_snapshot_dropped_for_test() const noexcept {
        return last_snapshot_dropped_.load(std::memory_order_relaxed);
    }
    size_t last_snapshot_scanned_count_for_test() const noexcept {
        return last_snapshot_scanned_count_.load(std::memory_order_relaxed);
    }

    // Reset buffer state
    void reset() noexcept;

private:
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<Slot[]> slots_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> dropped_events_{0};
    uint64_t qpc_freq_{0};
    mutable std::atomic<uint64_t> last_snapshot_dropped_{0};
    mutable std::atomic<size_t> last_snapshot_scanned_count_{0};
};

} // namespace stuttometer
