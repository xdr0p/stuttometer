#pragma once

#include "event_types.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdint>

namespace stuttometer {

// Default capacity: 2^18 = 262,144 slots (~16.8 MB)
inline constexpr size_t DEFAULT_RING_CAPACITY = 262144;

class FlightRecorder {
public:
    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        EtwEventRecord record{};
    };

    explicit FlightRecorder(size_t capacity = DEFAULT_RING_CAPACITY);
    ~FlightRecorder() = default;

    // Non-copyable, non-movable (pinned memory)
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // Push an event record into the ring buffer (lock-free, multi-producer safe)
    void push(const EtwEventRecord& event) noexcept;

    // Snapshot records within the given QPC timestamp window [from_qpc, to_qpc].
    // Extracted records are sorted chronologically by qpc_timestamp.
    std::vector<EtwEventRecord> snapshot(uint64_t from_qpc, uint64_t to_qpc, uint64_t* out_dropped_count = nullptr) const;

    // Diagnostics & Statistics
    uint64_t current_head() const noexcept { return head_.load(std::memory_order_relaxed); }
    uint64_t total_dropped_events() const noexcept { return dropped_events_.load(std::memory_order_relaxed); }
    size_t capacity() const noexcept { return capacity_; }

    // Reset buffer state
    void reset() noexcept;

private:
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<Slot[]> slots_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) mutable std::atomic<uint64_t> dropped_events_{0};
};

} // namespace stuttometer
