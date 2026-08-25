cmake_minimum_required(VERSION 3.25)

project(
  stuttometer
  VERSION 0.1.0
  DESCRIPTION "Lightweight Real-Time Windows ETW Stutter & Glitch Diagnostic Utility"
  LANGUAGES CXX RC
)

if(NOT WIN32)
  message(FATAL_ERROR "Stuttometer requires Windows ETW APIs.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Stuttometer requires a 64-bit target architecture (x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor /wd4324)
endif()

add_compile_definitions(
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

include(FetchContent)

# ------------------------------------------------------------------------------
# Dependencies: nlohmann_json & CLI11
# ------------------------------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)

FetchContent_Declare(
  CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
  URL_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a
)

FetchContent_MakeAvailable(nlohmann_json CLI11)

# ------------------------------------------------------------------------------
# Core Library: stuttometer_core
# ------------------------------------------------------------------------------
add_library(stuttometer_core STATIC
  src/flight_recorder.cpp
  src/etw_session.cpp
  src/trigger_engine.cpp
  src/correlator.cpp
  src/json_reporter.cpp
  src/privilege_utils.cpp
  src/mock_simulation.cpp
)

target_include_directories(stuttometer_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(stuttometer_core
  PUBLIC
    nlohmann_json::nlohmann_json
    advapi32
    psapi
)

target_compile_features(stuttometer_core PUBLIC cxx_std_20)

# ------------------------------------------------------------------------------
# Main Executable (CLI): stuttometer
# ------------------------------------------------------------------------------
add_executable(stuttometer
  src/main.cpp
)

target_link_libraries(stuttometer
  PRIVATE
    stuttometer_core
    CLI11::CLI11
)

target_compile_features(stuttometer PRIVATE cxx_std_20)

# ------------------------------------------------------------------------------
# GUI Executable: stuttometer_gui (Native Win32 Subsystem)
# ------------------------------------------------------------------------------
add_executable(stuttometer_gui WIN32
  src/gui/main_gui.cpp
  src/gui/gui_controller.cpp
  src/gui/resources.rc
  src/gui/stuttometer_gui.manifest
)

target_include_directories(stuttometer_gui
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
)

target_link_libraries(stuttometer_gui
  PRIVATE
    stuttometer_core
    user32
    gdi32
    comctl32
    comdlg32
    dwmapi
    uxtheme
    shell32
    winmm
    advapi32
    psapi
    ole32
)

target_compile_features(stuttometer_gui PRIVATE cxx_std_20)

# Administrator elevation (level='requireAdministrator'), PerMonitorV2 DPI awareness,
# and Common Controls v6 are embedded directly via src/gui/stuttometer_gui.manifest.
# /MANIFESTUAC:NO prevents MSVC from injecting a conflicting default asInvoker UAC snippet.
if(MSVC)
  target_link_options(stuttometer_gui PRIVATE "/MANIFESTUAC:NO")
endif()

option(STUTTO_KILL_BEFORE_BUILD "Kill running stuttometer_gui before link" OFF)

if(WIN32 AND STUTTO_KILL_BEFORE_BUILD)
  add_custom_command(
    TARGET stuttometer_gui PRE_LINK
    COMMAND cmd /c "taskkill /F /IM $<TARGET_FILE_NAME:stuttometer_gui> >nul 2>&1 || exit 0"
    COMMENT "Ensuring stuttometer_gui is not locked before linking"
  )
endif()

# ------------------------------------------------------------------------------
# Testing & Verification Targets
# ------------------------------------------------------------------------------
enable_testing()

add_executable(test_flight_recorder
  tests/test_flight_recorder.cpp
)
target_link_libraries(test_flight_recorder PRIVATE stuttometer_core)
add_test(NAME test_flight_recorder COMMAND test_flight_recorder)
set_tests_properties(test_flight_recorder PROPERTIES TIMEOUT 30)

add_executable(test_correlator
  tests/test_correlator.cpp
)
target_link_libraries(test_correlator PRIVATE stuttometer_core)
add_test(NAME test_correlator COMMAND test_correlator)
set_tests_properties(test_correlator PROPERTIES TIMEOUT 30)

add_executable(test_json_schema
  tests/test_json_schema.cpp
)
target_link_libraries(test_json_schema PRIVATE stuttometer_core)
add_test(NAME test_json_schema COMMAND test_json_schema)
set_tests_properties(test_json_schema PROPERTIES TIMEOUT 30)

add_executable(test_etw_constants
  tests/test_etw_constants.cpp
)
target_link_libraries(test_etw_constants PRIVATE stuttometer_core)
add_test(NAME test_etw_constants COMMAND test_etw_constants)
set_tests_properties(test_etw_constants PROPERTIES TIMEOUT 30)

add_executable(test_etw_session
  tests/test_etw_session.cpp
)
target_link_libraries(test_etw_session PRIVATE stuttometer_core)
add_test(NAME test_etw_session COMMAND test_etw_session)
set_tests_properties(test_etw_session PROPERTIES TIMEOUT 30)

add_executable(test_kernel_present_tracking
  tests/test_kernel_present_tracking.cpp
)
target_link_libraries(test_kernel_present_tracking PRIVATE stuttometer_core)
add_test(NAME test_kernel_present_tracking COMMAND test_kernel_present_tracking)
set_tests_properties(test_kernel_present_tracking PROPERTIES TIMEOUT 30)

add_executable(test_frame_pacing
  tests/test_frame_pacing.cpp
)
target_link_libraries(test_frame_pacing PRIVATE stuttometer_core)
add_test(NAME test_frame_pacing COMMAND test_frame_pacing)
set_tests_properties(test_frame_pacing PROPERTIES TIMEOUT 30)


