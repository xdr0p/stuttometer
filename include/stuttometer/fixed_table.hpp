#pragma once

#include <cstdint>
#include <atomic>
#include <cstring>
#include <type_traits>
#include <thread>
#include <memory>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace stuttometer {

inline void cpu_pause() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(_M_ARM64) || defined(__aarch64__)
    __yield();
#else
    std::this_thread::yield();
#endif
}

// Lock-free, zero-allocation, pre-allocated open-addressing table with spin-wait & eviction
template <typename Value, size_t Capacity = 2048>
class FixedInFlightTable {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(std::is_trivially_copyable_v<Value>, "Value must be trivially copyable");

public:
    enum SlotState : uint8_t {
        STATE_EMPTY     = 0,
        STATE_WRITING   = 1,
        STATE_VALID     = 2,
        STATE_TOMBSTONE = 3
    };

    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> key{0};
        std::atomic<uint8_t> state{STATE_EMPTY};
        Value value{};
    };

    FixedInFlightTable()
        : slots_(std::make_unique<Slot[]>(Capacity))
    {
        clear();
    }

    ~FixedInFlightTable() = default;

    // Non-copyable, non-movable
    FixedInFlightTable(const FixedInFlightTable&) = delete;
    FixedInFlightTable& operator=(const FixedInFlightTable&) = delete;

    void clear() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(0, std::memory_order_relaxed);
            slots_[i].key.store(0, std::memory_order_relaxed);
            slots_[i].state.store(STATE_EMPTY, std::memory_order_relaxed);
            std::memset(&slots_[i].value, 0, sizeof(Value));
        }
        insertion_failures_.store(0, std::memory_order_relaxed);
        unpaired_evictions_.store(0, std::memory_order_relaxed);
    }

    // Insert or update an in-flight key-value pair with spin-wait on STATE_WRITING
    bool insert(uint64_t key, const Value& val) noexcept {
        if (key == 0) {
            insertion_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 128;
        constexpr size_t MAX_SLOT_SPINS = 64;
        constexpr size_t MAX_INSERT_RETRIES = 8;

        for (size_t retry = 0; retry < MAX_INSERT_RETRIES; ++retry) {
            int first_tombstone_idx = -1;
            bool retry_needed = false;
            bool saw_writing = false;

            for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
                const size_t idx = (start_idx + probe) & MASK;
                Slot& slot = slots_[idx];

                while (true) {
                    uint8_t current_state = slot.state.load(std::memory_order_acquire);

                    // Bounded spin-wait if slot is currently in an active write transaction
                    size_t spins = 0;
                    while (current_state == STATE_WRITING) {
                        if (++spins > MAX_SLOT_SPINS) {
                            saw_writing = true;
                            std::this_thread::yield();
                            break;
                        }
                        cpu_pause();
                        current_state = slot.state.load(std::memory_order_acquire);
                    }

                    if (current_state == STATE_WRITING) {
                        saw_writing = true;
                        break; // Advance probe along collision chain, but remember writer was active
                    }

                    // 1. If slot already holds this key and is valid, update value in-place
                    if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                        if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                            if (slot.key.load(std::memory_order_relaxed) != key) {
                                slot.state.store(STATE_VALID, std::memory_order_release);
                                break; // Slot was recycled for a different key, advance probe
                            }

                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            std::memcpy(&slot.value, &val, sizeof(Value));

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            return true;
                        }
                        continue; // CAS failed, retry this slot
                    }

                    // 2. If slot is a tombstone, remember the first tombstone and continue probing to check for existing key
                    if (current_state == STATE_TOMBSTONE) {
                        if (first_tombstone_idx == -1) {
                            first_tombstone_idx = static_cast<int>(idx);
                        }
                        break; // Advance probe along collision chain
                    }

                    // 3. If slot is empty:
                    if (current_state == STATE_EMPTY) {
                        // If we saw a concurrent writer earlier in the chain, we cannot assume our key is absent.
                        // Re-probe from start_idx after a yield.
                        if (saw_writing) {
                            retry_needed = true;
                            std::this_thread::yield();
                            break;
                        }

                        if (first_tombstone_idx != -1) {
                            Slot& target_slot = slots_[static_cast<size_t>(first_tombstone_idx)];
                            uint8_t target_state = target_slot.state.load(std::memory_order_acquire);
                            if (target_state == STATE_EMPTY || target_state == STATE_TOMBSTONE) {
                                if (target_slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                                    const uint64_t seq = target_slot.sequence.load(std::memory_order_relaxed);
                                    target_slot.sequence.store(seq + 1, std::memory_order_release);
                                    std::atomic_thread_fence(std::memory_order_release);

                                    target_slot.key.store(key, std::memory_order_relaxed);
                                    std::memcpy(&target_slot.value, &val, sizeof(Value));

                                    std::atomic_thread_fence(std::memory_order_release);
                                    target_slot.sequence.store(seq + 2, std::memory_order_release);
                                    target_slot.state.store(STATE_VALID, std::memory_order_release);
                                    return true;
                                }
                            }
                            // Lost CAS on remembered tombstone: another thread claimed it.
                            // Re-probe from start_idx to prevent duplicate insertion.
                            retry_needed = true;
                            break;
                        }

                        // No tombstone seen; claim this empty slot
                        uint8_t target_state = STATE_EMPTY;
                        if (slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            slot.key.store(key, std::memory_order_relaxed);
                            std::memcpy(&slot.value, &val, sizeof(Value));

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            return true;
                        }
                        continue; // Lost CAS on empty slot, retry this slot
                    }

                    // Slot is valid for a different key; advance probe
                    break;
                }

                if (retry_needed) {
                    break;
                }
            }

            if (retry_needed) {
                const size_t pause_cycles = 1ULL << std::min(retry, static_cast<size_t>(6));
                for (size_t p = 0; p < pause_cycles; ++p) {
                    cpu_pause();
                }
                continue; // Re-probe from start_idx with backoff
            }

            // If probe chain ended without finding EMPTY, but we found a tombstone, claim it
            if (first_tombstone_idx != -1 && !saw_writing) {
                Slot& target_slot = slots_[static_cast<size_t>(first_tombstone_idx)];
                uint8_t target_state = target_slot.state.load(std::memory_order_acquire);
                if (target_state == STATE_EMPTY || target_state == STATE_TOMBSTONE) {
                    if (target_slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                        const uint64_t seq = target_slot.sequence.load(std::memory_order_relaxed);
                        target_slot.sequence.store(seq + 1, std::memory_order_release);
                        std::atomic_thread_fence(std::memory_order_release);

                        target_slot.key.store(key, std::memory_order_relaxed);
                        std::memcpy(&target_slot.value, &val, sizeof(Value));

                        std::atomic_thread_fence(std::memory_order_release);
                        target_slot.sequence.store(seq + 2, std::memory_order_release);
                        target_slot.state.store(STATE_VALID, std::memory_order_release);
                        return true;
                    }
                }
                // Lost CAS on tombstone, retry whole probe
                continue;
            }
        }

        insertion_failures_.fetch_add(1, std::memory_order_relaxed);
        return false; // Table at capacity or high collision
    }

    // Atomically mutate the value associated with key in-place using updater_fn(Value&)
    template <typename UpdaterFn>
    bool update(uint64_t key, UpdaterFn&& updater) noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 128;
        constexpr size_t MAX_SLOT_SPINS = 64;
        constexpr size_t MAX_RETRIES = 4;

        for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
            bool saw_writing = false;

            for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
                const size_t idx = (start_idx + probe) & MASK;
                Slot& slot = slots_[idx];

                while (true) {
                    uint8_t current_state = slot.state.load(std::memory_order_acquire);

                    size_t spins = 0;
                    while (current_state == STATE_WRITING) {
                        if (++spins > MAX_SLOT_SPINS) {
                            saw_writing = true;
                            std::this_thread::yield();
                            break;
                        }
                        cpu_pause();
                        current_state = slot.state.load(std::memory_order_acquire);
                    }

                    if (current_state == STATE_WRITING) {
                        saw_writing = true;
                        break;
                    }

                    if (current_state == STATE_EMPTY) {
                        if (saw_writing) {
                            break; // Re-probe if writer was seen
                        }
                        return false; // Key not present in table
                    }

                    if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                        if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                            if (slot.key.load(std::memory_order_relaxed) != key) {
                                slot.state.store(STATE_VALID, std::memory_order_release);
                                break; // Slot was recycled for a different key, advance probe
                            }

                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            updater(slot.value);

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            return true;
                        }
                        continue; // CAS failed, retry this slot
                    }

                    // Slot is tombstone or different key, advance probe
                    break;
                }
            }

            if (!saw_writing) {
                break;
            }
        }
        return false;
    }

    // Atomically mutate if present, or initialize with default_val and apply updater_fn in a single atomic transaction
    template <typename UpdaterFn>
    bool upsert(uint64_t key, const Value& default_val, UpdaterFn&& updater) noexcept {
        if (key == 0) {
            insertion_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 128;
        constexpr size_t MAX_SLOT_SPINS = 64;
        constexpr size_t MAX_INSERT_RETRIES = 8;

        for (size_t retry = 0; retry < MAX_INSERT_RETRIES; ++retry) {
            int first_tombstone_idx = -1;
            bool retry_needed = false;
            bool saw_writing = false;

            for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
                const size_t idx = (start_idx + probe) & MASK;
                Slot& slot = slots_[idx];

                while (true) {
                    uint8_t current_state = slot.state.load(std::memory_order_acquire);

                    size_t spins = 0;
                    while (current_state == STATE_WRITING) {
                        if (++spins > MAX_SLOT_SPINS) {
                            saw_writing = true;
                            std::this_thread::yield();
                            break;
                        }
                        cpu_pause();
                        current_state = slot.state.load(std::memory_order_acquire);
                    }

                    if (current_state == STATE_WRITING) {
                        saw_writing = true;
                        break;
                    }

                    // 1. If slot already holds this key and is valid, update value in-place
                    if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                        if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                            if (slot.key.load(std::memory_order_relaxed) != key) {
                                slot.state.store(STATE_VALID, std::memory_order_release);
                                break;
                            }

                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            updater(slot.value);

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            return true;
                        }
                        continue;
                    }

                    // 2. If slot is a tombstone
                    if (current_state == STATE_TOMBSTONE) {
                        if (first_tombstone_idx == -1) {
                            first_tombstone_idx = static_cast<int>(idx);
                        }
                        break;
                    }

                    // 3. If slot is empty
                    if (current_state == STATE_EMPTY) {
                        if (saw_writing) {
                            retry_needed = true;
                            std::this_thread::yield();
                            break;
                        }

                        if (first_tombstone_idx != -1) {
                            Slot& target_slot = slots_[static_cast<size_t>(first_tombstone_idx)];
                            uint8_t target_state = target_slot.state.load(std::memory_order_acquire);
                            if (target_state == STATE_EMPTY || target_state == STATE_TOMBSTONE) {
                                if (target_slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                                    const uint64_t seq = target_slot.sequence.load(std::memory_order_relaxed);
                                    target_slot.sequence.store(seq + 1, std::memory_order_release);
                                    std::atomic_thread_fence(std::memory_order_release);

                                    target_slot.key.store(key, std::memory_order_relaxed);
                                    target_slot.value = default_val;
                                    updater(target_slot.value);

                                    std::atomic_thread_fence(std::memory_order_release);
                                    target_slot.sequence.store(seq + 2, std::memory_order_release);
                                    target_slot.state.store(STATE_VALID, std::memory_order_release);
                                    return true;
                                }
                            }
                            retry_needed = true;
                            break;
                        }

                        uint8_t target_state = STATE_EMPTY;
                        if (slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            slot.key.store(key, std::memory_order_relaxed);
                            slot.value = default_val;
                            updater(slot.value);

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            return true;
                        }
                        continue;
                    }

                    break;
                }

                if (retry_needed) {
                    break;
                }
            }

            if (retry_needed) {
                const size_t pause_cycles = 1ULL << std::min(retry, static_cast<size_t>(6));
                for (size_t p = 0; p < pause_cycles; ++p) {
                    cpu_pause();
                }
                continue; // Re-probe from start_idx with backoff
            }

            if (first_tombstone_idx != -1 && !saw_writing) {
                Slot& target_slot = slots_[static_cast<size_t>(first_tombstone_idx)];
                uint8_t target_state = target_slot.state.load(std::memory_order_acquire);
                if (target_state == STATE_EMPTY || target_state == STATE_TOMBSTONE) {
                    if (target_slot.state.compare_exchange_strong(target_state, STATE_WRITING, std::memory_order_acquire)) {
                        const uint64_t seq = target_slot.sequence.load(std::memory_order_relaxed);
                        target_slot.sequence.store(seq + 1, std::memory_order_release);
                        std::atomic_thread_fence(std::memory_order_release);

                        target_slot.key.store(key, std::memory_order_relaxed);
                        target_slot.value = default_val;
                        updater(target_slot.value);

                        std::atomic_thread_fence(std::memory_order_release);
                        target_slot.sequence.store(seq + 2, std::memory_order_release);
                        target_slot.state.store(STATE_VALID, std::memory_order_release);
                        return true;
                    }
                }
                continue;
            }
        }

        insertion_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Lookup and erase the key in a single atomic transaction
    bool find_and_erase(uint64_t key, Value& out_val) noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 128;
        constexpr size_t MAX_SLOT_SPINS = 64;
        constexpr size_t MAX_RETRIES = 4;

        for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
            bool saw_writing = false;

            for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
                const size_t idx = (start_idx + probe) & MASK;
                Slot& slot = slots_[idx];

                while (true) {
                    uint8_t current_state = slot.state.load(std::memory_order_acquire);

                    // Bounded spin-wait if slot is currently being written
                    size_t spins = 0;
                    while (current_state == STATE_WRITING) {
                        if (++spins > MAX_SLOT_SPINS) {
                            saw_writing = true;
                            std::this_thread::yield();
                            break;
                        }
                        cpu_pause();
                        current_state = slot.state.load(std::memory_order_acquire);
                    }

                    if (current_state == STATE_WRITING) {
                        saw_writing = true;
                        break;
                    }

                    if (current_state == STATE_EMPTY) {
                        if (saw_writing) {
                            break; // Re-probe if writer was seen
                        }
                        return false; // Key not present in table
                    }

                    if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                        if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                            if (slot.key.load(std::memory_order_relaxed) != key) {
                                slot.state.store(STATE_VALID, std::memory_order_release);
                                break; // Slot was recycled for a different key, advance probe
                            }

                            const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                            slot.sequence.store(seq + 1, std::memory_order_release);
                            std::atomic_thread_fence(std::memory_order_release);

                            std::memcpy(&out_val, &slot.value, sizeof(Value));
                            slot.key.store(0, std::memory_order_relaxed);

                            std::atomic_thread_fence(std::memory_order_release);
                            slot.sequence.store(seq + 2, std::memory_order_release);
                            slot.state.store(STATE_TOMBSTONE, std::memory_order_release);
                            return true;
                        }
                        continue; // CAS failed, retry this slot
                    }

                    // Slot is tombstone or different key, advance probe
                    break;
                }
            }

            if (!saw_writing) {
                break;
            }
        }
        return false;
    }

    // Lookup without erasing (with torn read validation)
    bool lookup(uint64_t key, Value& out_val) const noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 128;
        constexpr size_t MAX_SLOT_SPINS = 64;
        constexpr size_t MAX_RETRIES = 4;

        for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
            bool saw_writing = false;

            for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
                const size_t idx = (start_idx + probe) & MASK;
                const Slot& slot = slots_[idx];

                while (true) {
                    uint8_t current_state = slot.state.load(std::memory_order_acquire);
                    size_t spins = 0;
                    while (current_state == STATE_WRITING) {
                        if (++spins > MAX_SLOT_SPINS) {
                            saw_writing = true;
                            std::this_thread::yield();
                            break;
                        }
                        cpu_pause();
                        current_state = slot.state.load(std::memory_order_acquire);
                    }

                    if (current_state == STATE_WRITING) {
                        saw_writing = true;
                        break;
                    }

                    if (current_state == STATE_EMPTY) {
                        if (saw_writing) {
                            break; // Re-probe if writer was seen
                        }
                        return false;
                    }

                    if (current_state == STATE_VALID && slot.key.load(std::memory_order_acquire) == key) {
                        const uint64_t seq1 = slot.sequence.load(std::memory_order_acquire);
                        if (seq1 % 2 != 0) {
                            cpu_pause();
                            continue; // Writing in progress
                        }

                        std::atomic_thread_fence(std::memory_order_acquire);
                        std::memcpy(&out_val, &slot.value, sizeof(Value));
                        std::atomic_thread_fence(std::memory_order_acquire);

                        const uint64_t seq2 = slot.sequence.load(std::memory_order_acquire);
                        if (seq1 == seq2 &&
                            slot.state.load(std::memory_order_acquire) == STATE_VALID &&
                            slot.key.load(std::memory_order_acquire) == key) {
                            return true;
                        }
                        continue; // Torn read or slot modified during copy, retry
                    }

                    // Slot is tombstone or different key, advance probe
                    break;
                }
            }

            if (!saw_writing) {
                break;
            }
        }
        return false;
    }

    // Background eviction for reclaiming stale entries (called strictly from background flush worker)
    template <typename TimestampExtractor>
    size_t evict_stale(uint64_t current_qpc, uint64_t max_age_qpc, TimestampExtractor extractor) noexcept {
        size_t evicted = 0;
        for (size_t i = 0; i < Capacity; ++i) {
            Slot& slot = slots_[i];
            const uint8_t current_state = slot.state.load(std::memory_order_acquire);
            if (current_state == STATE_VALID) {
                const uint64_t seq1 = slot.sequence.load(std::memory_order_acquire);
                if ((seq1 % 2) != 0) {
                    continue; // Writing in progress by a producer, skip slot
                }

                std::atomic_thread_fence(std::memory_order_acquire);
                Value temp_val{};
                std::memcpy(&temp_val, &slot.value, sizeof(Value));
                std::atomic_thread_fence(std::memory_order_acquire);

                const uint64_t seq2 = slot.sequence.load(std::memory_order_acquire);
                if (seq1 != seq2 || slot.state.load(std::memory_order_acquire) != STATE_VALID) {
                    continue; // Torn read or slot modified during copy, skip slot
                }

                const uint64_t ts = extractor(temp_val);
                if (current_qpc > ts && (current_qpc - ts) > max_age_qpc) {
                    uint8_t expected_state = STATE_VALID;
                    if (slot.state.compare_exchange_strong(expected_state, STATE_WRITING, std::memory_order_acquire)) {
                        // Verify sequence did not change right before CAS
                        if (slot.sequence.load(std::memory_order_acquire) != seq2) {
                            // Producer updated slot value in between, abort eviction
                            slot.state.store(STATE_VALID, std::memory_order_release);
                            continue;
                        }

                        const uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
                        slot.sequence.store(seq + 1, std::memory_order_release);
                        std::atomic_thread_fence(std::memory_order_release);

                        slot.key.store(0, std::memory_order_relaxed);

                        std::atomic_thread_fence(std::memory_order_release);
                        slot.sequence.store(seq + 2, std::memory_order_release);
                        slot.state.store(STATE_TOMBSTONE, std::memory_order_release);
                        ++evicted;
                    }
                }
            }
        }
        if (evicted > 0) {
            unpaired_evictions_.fetch_add(evicted, std::memory_order_relaxed);
        }
        return evicted;
    }

    uint64_t insertion_failures() const noexcept { return insertion_failures_.load(std::memory_order_relaxed); }
    uint64_t unpaired_evictions() const noexcept { return unpaired_evictions_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t MASK = Capacity - 1;

    // Fast 64-bit splitmix hash
    static constexpr size_t hash_key(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<size_t>(x);
    }

    std::unique_ptr<Slot[]> slots_;
    alignas(64) std::atomic<uint64_t> insertion_failures_{0};
    alignas(64) std::atomic<uint64_t> unpaired_evictions_{0};
};

} // namespace stuttometer
