#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <cstddef>
#include "privilege_utils.hpp"
#include "fixed_table.hpp"

namespace stuttometer {

enum class FilterKind : uint8_t {
    SEVERITY_GATE = 0,
    VBLANK_FLOOR  = 1
};

struct FilteredEvent {
    uint64_t qpc_timestamp{0};    // offset  0, size 8
    double   duration_ms{0.0};      // offset  8, size 8
    double   baseline_avg_ms{0.0};  // offset 16, size 8
    double   spike_ratio{0.0};      // offset 24, size 8
    uint32_t target_pid{0};         // offset 32, size 4
    uint32_t target_tid{0};         // offset 36, size 4
    uint16_t reason{0};             // offset 40, size 2 (TriggerReason)
    uint16_t source{0};             // offset 42, size 2 (TriggerSource)
    uint8_t  severity{0};           // offset 44, size 1 (MetricSeverity)
    uint8_t  cpu_index{0};          // offset 45, size 1
    uint8_t  filter_kind{0};        // offset 46, size 1 (FilterKind)
    uint8_t  _pad[9]{0};            // offset 47, size 9
};
static_assert(sizeof(FilteredEvent) == 56, "FilteredEvent must be exactly 56 bytes");
static_assert(alignof(FilteredEvent) == 8, "FilteredEvent must be 8-byte aligned");
static_assert(offsetof(FilteredEvent, qpc_timestamp) == 0);
static_assert(offsetof(FilteredEvent, severity) == 44);
static_assert(offsetof(FilteredEvent, filter_kind) == 46);

class FilteredEventRing {
public:
    static constexpr size_t CAPACITY = 256;
    static constexpr size_t MASK = CAPACITY - 1;
    static constexpr size_t MAX_READ_SPINS = 64;

    void push(const FilteredEvent& ev) noexcept {
        const uint64_t w = write_pos_.fetch_add(1, std::memory_order_relaxed);
        Cell& cell = ring_[w & MASK];

        // Fast-path drop if we've been lapped before we even started.
        const uint64_t cur_w = write_pos_.load(std::memory_order_acquire);
        if (cur_w > w && (cur_w - w) >= CAPACITY) {
            dropped_filtered_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const uint64_t writing_seq = (w * 2) + 1;
        const uint64_t ready_seq   = (w * 2) + 2;

        uint64_t seq_val = cell.sequence.load(std::memory_order_acquire);
        constexpr size_t MAX_SPINS = 256;
        size_t spins = 0;

        while (true) {
            if (seq_val >= writing_seq) {
                // A later-generation writer already owns this cell.
                // Our event is stale; drop it rather than clobber the newer one.
                dropped_filtered_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if ((seq_val % 2) != 0) {
                // Previous writer still in-flight on this cell.
                if (++spins > MAX_SPINS) {
                    dropped_filtered_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                cpu_pause();
                seq_val = cell.sequence.load(std::memory_order_acquire);
                continue;
            }
            if (cell.sequence.compare_exchange_weak(seq_val, writing_seq, std::memory_order_acq_rel)) {
                // Claimed. Publish inside the success path.
                cell.event = ev;
                cell.sequence.store(ready_seq, std::memory_order_release);
                return;
            }
            // CAS failed; seq_val holds the current value. Loop re-evaluates.
        }
    }

    bool pop(FilteredEvent& out) noexcept {
        for (;;) {
            const uint64_t r = read_pos_.load(std::memory_order_relaxed);
            const uint64_t w = write_pos_.load(std::memory_order_acquire);
            if (r >= w) return false;

            Cell& cell = ring_[r & MASK];
            const uint64_t expected_writing = (r * 2) + 1;
            const uint64_t expected_ready   = (r * 2) + 2;

            const uint64_t seq1 = cell.sequence.load(std::memory_order_acquire);

            if (seq1 == expected_writing) {
                size_t spins = 0;
                uint64_t s = seq1;
                while (s == expected_writing && ++spins <= MAX_READ_SPINS) {
                    cpu_pause();
                    s = cell.sequence.load(std::memory_order_acquire);
                }
                if (s == expected_writing) return false;
                continue;
            }
            if (seq1 < expected_writing) {
                size_t spins = 0;
                uint64_t s = seq1;
                while (s < expected_writing && ++spins <= MAX_READ_SPINS) {
                    cpu_pause();
                    s = cell.sequence.load(std::memory_order_acquire);
                }
                if (s < expected_writing) return false;
                continue;
            }
            if (seq1 > expected_ready) {
                dropped_filtered_.fetch_add(1, std::memory_order_relaxed);
                read_pos_.store(r + 1, std::memory_order_release);
                continue;
            }

            std::atomic_thread_fence(std::memory_order_acquire);
            FilteredEvent tmp = cell.event;
            std::atomic_thread_fence(std::memory_order_acquire);

            const uint64_t seq2 = cell.sequence.load(std::memory_order_acquire);
            if (seq2 != seq1) {
                dropped_filtered_.fetch_add(1, std::memory_order_relaxed);
                read_pos_.store(r + 1, std::memory_order_release);
                continue;
            }

            out = tmp;
            read_pos_.store(r + 1, std::memory_order_release);
            return true;
        }
    }

    uint64_t dropped_filtered() const noexcept {
        return dropped_filtered_.load(std::memory_order_relaxed);
    }

    // Test-only accessors
    void set_cell_sequence_for_test(size_t idx, uint64_t seq) noexcept {
        ring_[idx & MASK].sequence.store(seq, std::memory_order_release);
    }
    void set_cell_event_for_test(size_t idx, const FilteredEvent& ev) noexcept {
        ring_[idx & MASK].event = ev;
    }
    uint64_t get_cell_sequence_for_test(size_t idx) const noexcept {
        return ring_[idx & MASK].sequence.load(std::memory_order_acquire);
    }
    void set_write_pos_for_test(uint64_t w) noexcept {
        write_pos_.store(w, std::memory_order_release);
    }
    void set_read_pos_for_test(uint64_t r) noexcept {
        read_pos_.store(r, std::memory_order_release);
    }
    uint64_t get_write_pos_for_test() const noexcept {
        return write_pos_.load(std::memory_order_acquire);
    }
    uint64_t get_read_pos_for_test() const noexcept {
        return read_pos_.load(std::memory_order_acquire);
    }

private:
    struct alignas(64) Cell {
        std::atomic<uint64_t> sequence{0};
        FilteredEvent event{};
    };
    static_assert(sizeof(Cell) == 64, "Cell must be exactly one cache line");

    std::unique_ptr<Cell[]> ring_ = std::make_unique<Cell[]>(CAPACITY);
    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    alignas(64) std::atomic<uint64_t> dropped_filtered_{0};
};

} // namespace stuttometer
