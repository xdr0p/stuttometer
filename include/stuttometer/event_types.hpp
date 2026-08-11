#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace stuttometer {

// Event category identifiers for normalized records
enum class EventCategory : uint16_t {
    UNKNOWN   = 0,
    DXGI      = 1,
    AUDIO     = 2,
    DPC       = 3,
    ISR       = 4,
    DISK      = 5,
    CSWITCH   = 6,
    PROFILE   = 7
};

inline std::string_view category_to_string(EventCategory cat) noexcept {
    switch (cat) {
        case EventCategory::DXGI:    return "DXGI";
        case EventCategory::AUDIO:   return "AUDIO";
        case EventCategory::DPC:     return "DPC";
        case EventCategory::ISR:     return "ISR";
        case EventCategory::DISK:    return "DISK";
        case EventCategory::CSWITCH: return "CSWITCH";
        case EventCategory::PROFILE: return "PROFILE";
        default:                     return "UNKNOWN";
    }
}

// Category-specific flag bits
namespace EventFlags {
    inline constexpr uint8_t NONE                 = 0x00;
    inline constexpr uint8_t DISK_IS_WRITE        = 0x01;
    inline constexpr uint8_t CSWITCH_VOLUNTARY    = 0x02;
    inline constexpr uint8_t CSWITCH_HIGH_PRIO    = 0x04;
    inline constexpr uint8_t AUDIO_BUFFER_UNDERRUN= 0x08;
    inline constexpr uint8_t DXGI_VSYNC_WAIT      = 0x10;
}

// Strictly 56-byte POD normalized event record.
// When placed into a Slot with an 8-byte atomic sequence, total Slot size is exactly 64 bytes.
struct EtwEventRecord {
    // --- 8-byte aligned members (24 bytes) ---
    uint64_t qpc_timestamp;     // Offset 0: High-resolution timestamp
    uint64_t auxiliary_data;    // Offset 8: Secondary hash, byte count, or caller IP

    // --- 8-byte union (8 bytes) ---
    union {                     // Offset 16
        uint64_t routine_addr;  // DPC/ISR driver function pointer
        uint64_t file_key;      // Disk FileObject pointer or hashed path
        struct { uint32_t prev_pid; uint32_t prev_tid; } cswitch;
        struct { uint32_t frame_index; uint32_t present_flags; } dxgi;
        struct { uint32_t glitch_count; int32_t  error_code; } audio;
        uint64_t payload_u64;
    } payload;

    // --- 4-byte aligned members (12 bytes) ---
    uint32_t pid;               // Offset 24: Process ID
    uint32_t tid;               // Offset 28: Thread ID
    uint32_t duration_us;       // Offset 32: Duration in microseconds (DPC, ISR, Disk, Present, CSwitch deschedule)

    // --- 2-byte and 1-byte members (6 bytes) ---
    uint16_t category;          // Offset 36: EventCategory enum
    uint16_t event_id;          // Offset 38: ETW Event ID
    uint8_t  cpu_index;         // Offset 40: CPU Core index
    uint8_t  flags;             // Offset 41: EventFlags bitmask

    // --- Explicit Padding (14 bytes) ---
    uint8_t  reserved[14];      // Offset 42: Brings total struct size strictly to 56 bytes
};

static_assert(sizeof(EtwEventRecord) == 56, "EtwEventRecord must be exactly 56 bytes");
static_assert(std::is_trivially_copyable_v<EtwEventRecord>, "EtwEventRecord must be trivially copyable");

} // namespace stuttometer
