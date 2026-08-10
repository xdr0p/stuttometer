cmake_minimum_required(VERSION 3.25)

project(
  stuttometer
  VERSION 0.1.0
  DESCRIPTION "Lightweight Real-Time Windows ETW Stutter & Glitch Diagnostic Utility"
  LANGUAGES CXX
)

if(NOT WIN32)
  message(FATAL_ERROR "Stuttometer requires Windows ETW APIs.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
)

FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
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
)

target_include_directories(stuttometer_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(stuttometer_core
  PUBLIC
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    advapi32
    tdh
    psapi
)

target_compile_features(stuttometer_core PUBLIC cxx_std_20)

# ------------------------------------------------------------------------------
# Main Executable: stuttometer
# ------------------------------------------------------------------------------
add_executable(stuttometer
  src/main.cpp
)

target_link_libraries(stuttometer
  PRIVATE
    stuttometer_core
)

target_compile_features(stuttometer PRIVATE cxx_std_20)

# ------------------------------------------------------------------------------
# Testing & Verification Targets
# ------------------------------------------------------------------------------
enable_testing()

add_executable(test_flight_recorder
  tests/test_flight_recorder.cpp
)
target_link_libraries(test_flight_recorder PRIVATE stuttometer_core)
add_test(NAME test_flight_recorder COMMAND test_flight_recorder)

add_executable(test_correlator
  tests/test_correlator.cpp
)
target_link_libraries(test_correlator PRIVATE stuttometer_core)
add_test(NAME test_correlator COMMAND test_correlator)

add_executable(test_json_schema
  tests/test_json_schema.cpp
)
target_link_libraries(test_json_schema PRIVATE stuttometer_core)
add_test(NAME test_json_schema COMMAND test_json_schema)
