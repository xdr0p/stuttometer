#pragma once

// =============================================================================
// Stuttometer Single Source of Truth Version Definitions
// =============================================================================
// This header is consumed by:
//   1. All C++ source files (via stuttometer::TOOL_VERSION and constants)
//   2. Win32 resource scripts (resources.rc via STUTTOMETER_VERSION_* macros)
//   3. CMake build configuration (extracted via regex in CMakeLists.txt)
// =============================================================================

#define STUTTOMETER_VERSION_MAJOR 0
#define STUTTOMETER_VERSION_MINOR 5
#define STUTTOMETER_VERSION_PATCH 3
#define STUTTOMETER_VERSION_STRING "0.5.3"
#define STUTTOMETER_VERSION_QUAD 0,5,3,0
#define STUTTOMETER_VERSION_QUAD_STRING "0.5.3.0"

#ifdef __cplusplus
#include <string_view>

namespace stuttometer {

inline constexpr std::string_view TOOL_VERSION = STUTTOMETER_VERSION_STRING;
inline constexpr std::string_view TOOL_NAME = "Stuttometer";

inline constexpr int VERSION_MAJOR = STUTTOMETER_VERSION_MAJOR;
inline constexpr int VERSION_MINOR = STUTTOMETER_VERSION_MINOR;
inline constexpr int VERSION_PATCH = STUTTOMETER_VERSION_PATCH;

} // namespace stuttometer
#endif
