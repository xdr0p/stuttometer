#include "stuttometer/flight_recorder.hpp"
#include <cstring>

namespace stuttometer {

FlightRecorder::FlightRecorder(size_t capacity)
    : capacity_([capacity]() {
          // Ensure capacity is a power of 2
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

    // Odd sequence indicates write in progress
    const uint64_t writing_seq = (seq * 2) + 1;
    const uint64_t ready_seq   = (seq * 2) + 2;

    slot.sequence.store(writing_seq, std::memory_order_release);

    // Copy event record and stamp sequence number
    slot.record = event;
    slot.record.sequence_num = seq;

    // Even sequence indicates payload is ready and valid
    slot.sequence.store(ready_seq, std::memory_order_release);
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

    for (uint64_t seq = start_seq; seq < current_head; ++seq) {
        const size_t slot_idx = static_cast<size_t>(seq & mask_);
        const Slot& slot = slots_[slot_idx];

        // Seqlock read protocol
        const uint64_t seq1 = slot.sequence.load(std::memory_order_acquire);
        
        // If writing in progress (odd) or overwritten by a different sequence
        if ((seq1 % 2 != 0) || (seq1 != (seq * 2 + 2))) {
            ++local_dropped;
            continue;
        }

        // Copy record
        EtwEventRecord temp = slot.record;

        // Verify sequence did not change during copy
        const uint64_t seq2 = slot.sequence.load(std::memory_order_acquire);
        if (seq1 != seq2) {
            ++local_dropped;
            continue;
        }

        // Check if event falls within timestamp window
        if (temp.qpc_timestamp >= from_qpc && temp.qpc_timestamp <= to_qpc) {
            results.push_back(temp);
        }
    }

    if (out_dropped_count) {
        *out_dropped_count = local_dropped;
    }
    if (local_dropped > 0) {
        dropped_events_.fetch_add(local_dropped, std::memory_order_relaxed);
    }

    // Sort snapshot chronologically by QPC timestamp
    std::sort(results.begin(), results.end(), [](const EtwEventRecord& a, const EtwEventRecord& b) {
        return a.qpc_timestamp < b.qpc_timestamp;
    });

    return results;
}

} // namespace stuttometer
