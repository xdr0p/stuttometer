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

// Strictly 64-byte POD normalized event record.
// Reordered by member alignment to eliminate all compiler padding.
struct alignas(64) EtwEventRecord {
    // --- 8-byte aligned members (24 bytes) ---
    uint64_t sequence_num;      // Offset 0: Monotonic sequence counter
    uint64_t qpc_timestamp;     // Offset 8: High-resolution timestamp
    uint64_t auxiliary_data;    // Offset 16: Secondary hash, byte count, or caller IP

    // --- 8-byte union (8 bytes) ---
    union {                     // Offset 24
        uint64_t routine_addr;  // DPC/ISR driver function pointer
        uint64_t file_key;      // Disk FileObject pointer or hashed path
        struct { uint32_t prev_pid; uint32_t prev_tid; } cswitch;
        struct { uint32_t frame_index; uint32_t present_flags; } dxgi;
        struct { uint32_t glitch_count; int32_t  error_code; } audio;
        uint64_t payload_u64;
    } payload;

    // --- 4-byte aligned members (12 bytes) ---
    uint32_t pid;               // Offset 32: Process ID
    uint32_t tid;               // Offset 36: Thread ID
    uint32_t duration_us;       // Offset 40: Duration in microseconds (DPC, ISR, Disk, Present)

    // --- 2-byte and 1-byte members (8 bytes) ---
    uint16_t category;          // Offset 44: EventCategory enum
    uint16_t event_id;          // Offset 46: ETW Event ID
    uint8_t  cpu_index;         // Offset 48: CPU Core index
    uint8_t  flags;             // Offset 49: EventFlags bitmask
    uint8_t  aux_u8[2];         // Offset 50: [0]=wait_reason / old_state, [1]=new_priority

    // --- Explicit Padding (12 bytes) ---
    uint8_t  reserved[12];      // Offset 52: Brings total strictly to 64 bytes
};

static_assert(sizeof(EtwEventRecord) == 64, "EtwEventRecord must be exactly 64 bytes");
static_assert(std::is_trivially_copyable_v<EtwEventRecord>, "EtwEventRecord must be trivially copyable");

} // namespace stuttometer
