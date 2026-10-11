#include "stuttometer/flight_recorder.hpp"
#include "stuttometer/fixed_table.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <cstring>
#include <algorithm>

namespace stuttometer {

FlightRecorder::FlightRecorder(size_t capacity, uint64_t qpc_freq)
    : capacity_([capacity]() {
          size_t cap = 1;
          while (cap < capacity) {
              cap <<= 1;
          }
          return cap;
      }())
    , mask_(capacity_ - 1)
    , slots_(std::make_unique<Slot[]>(capacity_))
    , qpc_freq_((qpc_freq != 0) ? qpc_freq : get_qpc_frequency())
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

    // Wraparound protection: best-effort fast-path check if producer was preempted/delayed
    // and the ring wrapped past its ticket before writing could begin.
    // Note: head_ can advance further concurrently; full linearization and stale overwrite
    // protection are guaranteed below by the slot.sequence CAS.
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

void FlightRecorder::snapshot(
    uint64_t from_qpc,
    uint64_t to_qpc,
    std::vector<EtwEventRecord>& out_records,
    uint64_t* out_dropped_count
) const {
    out_records.clear();
    if (out_records.capacity() < 4096) {
        out_records.reserve(4096);
    }

    if (from_qpc > to_qpc) {
        if (out_dropped_count) *out_dropped_count = 0;
        last_snapshot_dropped_.store(0, std::memory_order_relaxed);
        last_snapshot_scanned_count_.store(0, std::memory_order_relaxed);
        return;
    }

    const uint64_t current_head = head_.load(std::memory_order_acquire);
    const uint64_t start_seq = (current_head > capacity_) ? (current_head - capacity_) : 0;

    uint64_t local_dropped = 0;
    uint64_t seq = current_head;
    size_t out_of_window_count = 0;
    size_t scanned_slots = 0;

    // Retry budget constraints
    constexpr size_t MAX_SLOT_SPINS = 16;
    constexpr size_t GLOBAL_SNAPSHOT_RETRY_BUDGET = 256;
    size_t global_retries = 0;

    // Cushion constraints
    constexpr size_t MIN_OUT_OF_WINDOW_SLOTS = 64;
    constexpr size_t MAX_OUT_OF_WINDOW_SLOTS = 2048;

    // Cross-session skew threshold: default 150.0 ms (1.5x active flush interval of 100 ms).
    // Note: While ETW_FLUSH_TIMER_SEC is 1s, active tracing issues ControlTraceW flushes every 100 ms,
    // and trigger processing synchronously calls session_mgr.flush_buffers() and waits for the drain
    // before snapshot() is invoked. Residual skew is bounded purely by cross-session buffer reordering.
    const uint64_t max_skew_qpc = (qpc_freq_ > 0) ? ms_to_qpc_delta(150.0, qpc_freq_) : 0;

    while (seq > start_seq) {
        --seq;
        ++scanned_slots;
        const size_t slot_idx = static_cast<size_t>(seq & mask_);
        const Slot& slot = slots_[slot_idx];
        const uint64_t expected_seq = (seq * 2) + 2;

        size_t slot_spins = 0;

        while (true) {
            uint64_t seq1 = slot.sequence.load(std::memory_order_acquire);

            // 1. In-flight writer spin wait
            while ((seq1 % 2) != 0 && slot_spins < MAX_SLOT_SPINS && global_retries < GLOBAL_SNAPSHOT_RETRY_BUDGET) {
                cpu_pause();
                ++slot_spins;
                ++global_retries;
                seq1 = slot.sequence.load(std::memory_order_acquire);
            }

            if ((seq1 % 2) != 0) {
                // In-flight writer did not finish within spin budget; skip without inflating drop stats
                break;
            }

            // 2. Generation check
            if (seq1 < expected_seq) {
                // Writer has not yet published this ticket; skip silently
                break;
            }
            if (seq1 > expected_seq) {
                // Slot was lapped by a newer generation before we could read it
                ++local_dropped;
                break;
            }

            // 3. Checkpoint 1: Fast timestamp pre-check
            // Plain 8-byte scalar read hitting the same 64-byte L1 cache line resident from seq1 load
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t ts = slot.record.qpc_timestamp;
            std::atomic_thread_fence(std::memory_order_acquire);

            const uint64_t seq2 = slot.sequence.load(std::memory_order_acquire);
            if (seq1 != seq2) {
                // Pre-check was torn by concurrent writer
                if (slot_spins < MAX_SLOT_SPINS && global_retries < GLOBAL_SNAPSHOT_RETRY_BUDGET) {
                    cpu_pause();
                    ++slot_spins;
                    ++global_retries;
                    continue; // Retry same slot
                }
                // Budget exhausted on pre-check: skip without inflating drops
                break;
            }

            // 4. Timestamp triage
            if (ts > to_qpc) {
                // Event is newer than target window; skip memcpy, out_of_window_count untouched
                break;
            }

            if (ts >= from_qpc && ts <= to_qpc) {
                // In-window event: Checkpoint 2 full 56-byte record copy
                out_records.emplace_back();
                std::memcpy(&out_records.back(), &slot.record, sizeof(EtwEventRecord));
                std::atomic_thread_fence(std::memory_order_acquire);

                const uint64_t seq3 = slot.sequence.load(std::memory_order_acquire);
                if (seq1 != seq3) {
                    // Copy was torn by concurrent writer: roll back
                    out_records.pop_back();
                    if (slot_spins < MAX_SLOT_SPINS && global_retries < GLOBAL_SNAPSHOT_RETRY_BUDGET) {
                        cpu_pause();
                        ++slot_spins;
                        ++global_retries;
                        continue; // Retry same slot
                    }
                    // Budget exhausted on attempted in-window copy: legitimate drop
                    ++local_dropped;
                    break;
                }

                // Successful in-window copy: reset out_of_window_count
                out_of_window_count = 0;
                break;
            }

            if (ts > 0 && ts < from_qpc) {
                // Event is older than target window: evaluate hybrid cushion
                ++out_of_window_count;
                if (out_of_window_count >= MIN_OUT_OF_WINDOW_SLOTS) {
                    if (max_skew_qpc > 0 && (from_qpc - ts) > max_skew_qpc) {
                        // Hardware clock confirmed past maximum cross-session skew
                        seq = start_seq; // Terminate outer loop
                        break;
                    }
                }
                if (out_of_window_count >= MAX_OUT_OF_WINDOW_SLOTS) {
                    seq = start_seq; // Terminate outer loop
                    break;
                }
                break;
            }

            // ts == 0 or other uninitialized state: skip without touching counters
            break;
        }
    }

    if (out_dropped_count) {
        *out_dropped_count = local_dropped;
    }
    last_snapshot_dropped_.store(local_dropped, std::memory_order_relaxed);
    last_snapshot_scanned_count_.store(scanned_slots, std::memory_order_relaxed);

    // Reverse descending buffer into predominantly ascending order
    std::reverse(out_records.begin(), out_records.end());

    // Near O(N) stable sort: preserves arrival ticket order for events with identical timestamps
    std::stable_sort(out_records.begin(), out_records.end(), [](const EtwEventRecord& a, const EtwEventRecord& b) {
        return a.qpc_timestamp < b.qpc_timestamp;
    });
}

std::vector<EtwEventRecord> FlightRecorder::snapshot(
    uint64_t from_qpc, 
    uint64_t to_qpc, 
    uint64_t* out_dropped_count
) const {
    std::vector<EtwEventRecord> results;
    results.reserve(4096);
    snapshot(from_qpc, to_qpc, results, out_dropped_count);
    return results;
}

} // namespace stuttometer
