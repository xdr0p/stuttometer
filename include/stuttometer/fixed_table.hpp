#pragma once

#include <cstdint>
#include <atomic>
#include <cstring>
#include <type_traits>

namespace stuttometer {

// Lock-free, zero-allocation, pre-allocated open-addressing table for in-flight tracking
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

    struct alignas(32) Slot {
        std::atomic<uint64_t> key{0};
        std::atomic<uint8_t> state{STATE_EMPTY};
        Value value{};
    };

    FixedInFlightTable() {
        clear();
    }

    ~FixedInFlightTable() = default;

    // Non-copyable, non-movable
    FixedInFlightTable(const FixedInFlightTable&) = delete;
    FixedInFlightTable& operator=(const FixedInFlightTable&) = delete;

    void clear() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].key.store(0, std::memory_order_relaxed);
            slots_[i].state.store(STATE_EMPTY, std::memory_order_relaxed);
            std::memset(&slots_[i].value, 0, sizeof(Value));
        }
    }

    // Insert or update an in-flight key-value pair (lock-free CAS)
    bool insert(uint64_t key, const Value& val) noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 64;

        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            const size_t idx = (start_idx + probe) & MASK;
            Slot& slot = slots_[idx];

            uint8_t current_state = slot.state.load(std::memory_order_acquire);

            // If slot already holds this key and is valid, update value
            if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                    std::memcpy(&slot.value, &val, sizeof(Value));
                    slot.state.store(STATE_VALID, std::memory_order_release);
                    return true;
                }
            }

            // If slot is empty or tombstone, claim it
            if (current_state == STATE_EMPTY || current_state == STATE_TOMBSTONE) {
                if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                    slot.key.store(key, std::memory_order_relaxed);
                    std::memcpy(&slot.value, &val, sizeof(Value));
                    slot.state.store(STATE_VALID, std::memory_order_release);
                    return true;
                }
            }
        }
        return false; // Table at capacity or high collision
    }

    // Lookup and erase the key in a single atomic transaction
    bool find_and_erase(uint64_t key, Value& out_val) noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 64;

        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            const size_t idx = (start_idx + probe) & MASK;
            Slot& slot = slots_[idx];

            uint8_t current_state = slot.state.load(std::memory_order_acquire);

            if (current_state == STATE_EMPTY) {
                return false; // Key not present
            }

            if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                if (slot.state.compare_exchange_strong(current_state, STATE_WRITING, std::memory_order_acquire)) {
                    std::memcpy(&out_val, &slot.value, sizeof(Value));
                    slot.key.store(0, std::memory_order_relaxed);
                    slot.state.store(STATE_TOMBSTONE, std::memory_order_release);
                    return true;
                }
            }
        }
        return false;
    }

    // Lookup without erasing
    bool lookup(uint64_t key, Value& out_val) const noexcept {
        if (key == 0) return false;

        const size_t start_idx = hash_key(key) & MASK;
        constexpr size_t MAX_PROBES = 64;

        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            const size_t idx = (start_idx + probe) & MASK;
            const Slot& slot = slots_[idx];

            const uint8_t current_state = slot.state.load(std::memory_order_acquire);
            if (current_state == STATE_EMPTY) {
                return false;
            }

            if (current_state == STATE_VALID && slot.key.load(std::memory_order_relaxed) == key) {
                std::memcpy(&out_val, &slot.value, sizeof(Value));
                return true;
            }
        }
        return false;
    }

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

    Slot slots_[Capacity];
};

} // namespace stuttometer
