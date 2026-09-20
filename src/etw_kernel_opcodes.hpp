#pragma once
#include <cstdint>

namespace stuttometer {

inline constexpr uint8_t KERNEL_OPCODE_DPC_CLASSIC       = 66;
inline constexpr uint8_t KERNEL_OPCODE_ISR_CLASSIC       = 67;
inline constexpr uint8_t KERNEL_OPCODE_DPC               = 68;
inline constexpr uint8_t KERNEL_OPCODE_TIMER             = 69;
inline constexpr uint8_t KERNEL_OPCODE_CSWITCH           = 36;
inline constexpr uint8_t KERNEL_OPCODE_DISK_READ_INIT    = 12;
inline constexpr uint8_t KERNEL_OPCODE_DISK_WRITE_INIT   = 13;
inline constexpr uint8_t KERNEL_OPCODE_DISK_READ         = 10;
inline constexpr uint8_t KERNEL_OPCODE_DISK_WRITE        = 11;
inline constexpr uint8_t KERNEL_OPCODE_HARDFAULT         = 32;
inline constexpr uint8_t KERNEL_OPCODE_VIRTUAL_ALLOC     = 98;

} // namespace stuttometer
