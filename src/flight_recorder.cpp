#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/fixed_table.hpp"
#include <cstring>
#include <algorithm>

namespace stuttometer {

FlightRecorder::FlightRecorder(size_t capacity)
    : capacity_([capacity]() {
          size_t cap = 1;
          while (cap < capacity) {
              cap <<= 1;
          }
          return cap;
      }())
    , mask_(capacity_ - 1)
    , slots_(std::make_unique<Slot[]>(capacity_))
{
    reset();
}

void FlightRecorder::reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    dropped_events_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < capacity_; ++i) {
        slots_[i].sequence.store(0, std::memory_order_relaxed);
        std::memset(&slots_[i].record, 0, sizeof(EtwEventRecord));
    }
}

void FlightRecorder::push(const EtwEventRecord& event) noexcept {
    // Atomically claim a monotonic ticket
    const uint64_t seq = head_.fetch_add(1, std::memory_order_relaxed);
    const size_t slot_idx = static_cast<size_t>(seq & mask_);
    Slot& slot = slots_[slot_idx];

    // Wraparound protection: check if producer was delayed and ring wrapped past its ticket
    const uint64_t cur_head = head_.load(std::memory_order_relaxed);
    if (cur_head > seq && (cur_head - seq) >= capacity_) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return; // Safe abort: do not overwrite newer generation
    }

    const uint64_t writing_seq = (seq * 2) + 1;
    const uint64_t ready_seq   = (seq * 2) + 2;

    // Slot sequence generation CAS: ensure we only write to slots where
    // previous writes have fully completed (even sequence). If an older writer
    // is currently in mid-write (odd sequence), spin-wait with cpu_pause() up to 256 cycles.
    uint64_t seq_val = slot.sequence.load(std::memory_order_acquire);
    constexpr size_t MAX_SPINS = 256;
    size_t spins = 0;

    while (seq_val < writing_seq) {
        if ((seq_val % 2) != 0) {
            // Write in progress by an older generation; wait for it to complete publication
            if (++spins > MAX_SPINS) {
                dropped_events_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cpu_pause();
            seq_val = slot.sequence.load(std::memory_order_acquire);
            continue;
        }

        if (slot.sequence.compare_exchange_weak(seq_val, writing_seq, std::memory_order_acq_rel)) {
            // Linear memory copy bounded by seqlock fences
            std::memcpy(&slot.record, &event, sizeof(EtwEventRecord));

            // Publish completed write by advancing odd writing_seq to even ready_seq
            slot.sequence.store(ready_seq, std::memory_order_release);
            return;
        }
    }

    // Fallthrough occurs if:
    // 1. seq_val > writing_seq: Another producer claimed a newer generation for this slot (lapped the ring),
    //    meaning this writer was overtaken. Dropping the stale event is intentional for lock-free bounded rings.
    // 2. Timeout / spin limit exceeded while waiting for an older write to finish.
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<EtwEventRecord> FlightRecorder::snapshot(
    uint64_t from_qpc, 
    uint64_t to_qpc, 
    uint64_t* out_dropped_count
) const {
    std::vector<EtwEventRecord> results;
    results.reserve(4096);

    const uint64_t current_head = head_.load(std::memory_order_acquire);
    const uint64_t start_seq = (current_head > capacity_) ? (current_head - capacity_) : 0;

    uint64_t local_dropped = 0;
    uint64_t seq = current_head;
    size_t out_of_window_count = 0;
    constexpr size_t OUT_OF_WINDOW_CUSHION = 4096;
    constexpr size_t MAX_READ_SPINS = 64;

    // Scan backwards from newest ticket (current_head - 1) down to start_seq
    // 'while (seq > start_seq)' prevents unsigned 64-bit underflow when start_seq == 0
    while (seq > start_seq) {
        --seq;
        const size_t slot_idx = static_cast<size_t>(seq & mask_);
        const Slot& slot = slots_[slot_idx];

        // Seqlock read protocol with bounded spin-wait for in-flight producer writes
        uint64_t seq1 = slot.sequence.load(std::memory_order_acquire);
        size_t read_spins = 0;
        while ((seq1 % 2) != 0 && ++read_spins <= MAX_READ_SPINS) {
            cpu_pause();
            seq1 = slot.sequence.load(std::memory_order_acquire);
        }
        
        // If still in-flight after bounded spin-wait, safely skip
        if (seq1 % 2 != 0) {
            continue; // In-flight write; skip without artificially inflating drop statistics
        }

        // If overwritten by a newer generation sequence
        if (seq1 != (seq * 2 + 2)) {
            ++local_dropped;
            continue;
        }

        // Copy record using memcpy
        EtwEventRecord temp{};
        std::memcpy(&temp, &slot.record, sizeof(EtwEventRecord));

        // Verify sequence did not change during copy
        const uint64_t seq2 = slot.sequence.load(std::memory_order_acquire);
        if (seq1 != seq2) {
            ++local_dropped;
            continue;
        }

        // Check if event falls within timestamp window
        if (temp.qpc_timestamp >= from_qpc && temp.qpc_timestamp <= to_qpc) {
            results.push_back(temp);
            out_of_window_count = 0;
        } else if (temp.qpc_timestamp < from_qpc) {
            // Event is older than analysis window; allow cushion for multi-core out-of-order ticket arrivals
            if (++out_of_window_count >= OUT_OF_WINDOW_CUSHION) {
                break;
            }
        }
    }

    if (out_dropped_count) {
        *out_dropped_count = local_dropped;
    }

    // Stable sort to guarantee strict monotonic chronological ordering in case of concurrent core interleaving
    std::stable_sort(results.begin(), results.end(), [](const EtwEventRecord& a, const EtwEventRecord& b) {
        return a.qpc_timestamp < b.qpc_timestamp;
    });

    return results;
}

} // namespace stuttometer
