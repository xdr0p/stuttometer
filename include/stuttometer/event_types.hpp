#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace stuttometer {

// Event category identifiers for normalized records
enum class EventCategory : uint16_t {
    UNKNOWN          = 0,
    DXGI             = 1,
    AUDIO            = 2,
    DPC              = 3,
    ISR              = 4,
    DISK             = 5,
    CSWITCH          = 6,
    PROFILE          = 7,
    DXGKRNL_MMIOFLIP = 8,
    DXGKRNL_VSYNCDPC = 9,
    DWM_GLITCH       = 10,
    PAGE_FAULT       = 11,
    THERMAL_THROTTLE = 12,
    ANTIMALWARE_SCAN = 13,
    D3D12_PSO_CREATE = 14,
    DXGKRNL_VRAM_PAGING = 15,
    MEM_VIRTUAL_ALLOC = 16,
    MEM_WORKING_SET_TRIM = 17,
    MEM_PHYSICAL_ALLOC = 18
};

inline std::string_view category_to_string(EventCategory cat) noexcept {
    switch (cat) {
        case EventCategory::DXGI:                 return "DXGI";
        case EventCategory::AUDIO:                return "AUDIO";
        case EventCategory::DPC:                  return "DPC";
        case EventCategory::ISR:                  return "ISR";
        case EventCategory::DISK:                 return "DISK";
        case EventCategory::CSWITCH:              return "CSWITCH";
        case EventCategory::PROFILE:              return "PROFILE";
        case EventCategory::DXGKRNL_MMIOFLIP:     return "DXGKRNL_MMIOFLIP";
        case EventCategory::DXGKRNL_VSYNCDPC:     return "DXGKRNL_VSYNCDPC";
        case EventCategory::DWM_GLITCH:           return "DWM_GLITCH";
        case EventCategory::PAGE_FAULT:           return "PAGE_FAULT";
        case EventCategory::THERMAL_THROTTLE:     return "THERMAL_THROTTLE";
        case EventCategory::ANTIMALWARE_SCAN:     return "ANTIMALWARE_SCAN";
        case EventCategory::D3D12_PSO_CREATE:     return "D3D12_PSO_CREATE";
        case EventCategory::DXGKRNL_VRAM_PAGING:  return "DXGKRNL_VRAM_PAGING";
        case EventCategory::MEM_VIRTUAL_ALLOC:    return "MEM_VIRTUAL_ALLOC";
        case EventCategory::MEM_WORKING_SET_TRIM: return "MEM_WORKING_SET_TRIM";
        case EventCategory::MEM_PHYSICAL_ALLOC:   return "MEM_PHYSICAL_ALLOC";
        default:                                  return "UNKNOWN";
    }
}

// Category-specific flag bits
namespace EventFlags {
    inline constexpr uint16_t NONE                    = 0x0000;
    inline constexpr uint16_t DISK_IS_WRITE           = 0x0001;
    inline constexpr uint16_t CSWITCH_VOLUNTARY       = 0x0002;
    inline constexpr uint16_t CSWITCH_HIGH_PRIO       = 0x0004;
    inline constexpr uint16_t AUDIO_BUFFER_UNDERRUN   = 0x0008;
    inline constexpr uint16_t DXGI_VSYNC_WAIT         = 0x0010;
    inline constexpr uint16_t CSWITCH_OUT_VOLUNTARY   = 0x0020;
    inline constexpr uint16_t D3D12_GRAPHICS_PSO      = 0x0040;
    inline constexpr uint16_t D3D12_COMPUTE_PSO       = 0x0080;
    inline constexpr uint16_t VRAM_DEMOTED_COMMITMENT = 0x0100;
    inline constexpr uint16_t VRAM_USAGE_OVER_BUDGET  = 0x0200;
    inline constexpr uint16_t VRAM_PAGING_TRANSFER    = 0x0400;
    inline constexpr uint16_t MEM_ALLOC_COMMIT        = 0x1000;
    inline constexpr uint16_t MEM_WS_TRIM_OUTSWAP      = 0x2000;
    inline constexpr uint16_t MEM_PHYSICAL_CONTIGUOUS  = 0x4000;
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

    // --- 2-byte and 1-byte members (8 bytes) ---
    uint16_t category;          // Offset 36: EventCategory enum
    uint16_t event_id;          // Offset 38: ETW Event ID
    uint8_t  cpu_index;         // Offset 40: CPU Core index
    uint8_t  _pad_flags;        // Offset 41: Explicit padding for uint16_t alignment
    uint16_t flags;             // Offset 42: EventFlags bitmask (16 bits)

    // --- Explicit Padding (12 bytes) ---
    uint8_t  reserved[12];      // Offset 44: Brings total struct size strictly to 56 bytes
};

static_assert(sizeof(EtwEventRecord) == 56, "EtwEventRecord must be exactly 56 bytes");
static_assert(std::is_trivially_copyable_v<EtwEventRecord>, "EtwEventRecord must be trivially copyable");

} // namespace stuttometer
