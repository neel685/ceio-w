cmake_minimum_required(VERSION 3.24)

project(ceio_sim
  VERSION 0.1.0
  DESCRIPTION "Discrete-event simulator for cache-efficient NIC-CPU I/O (CEIO, Toasty, CEIO-W)"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(CEIO_BUILD_TESTS "Build unit tests" ON)
option(CEIO_WERROR "Treat warnings as errors" ON)
set(CEIO_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined'")

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------
add_library(ceio_options INTERFACE)
target_compile_options(ceio_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast>
  $<$<AND:$<BOOL:${CEIO_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>)
if(CEIO_SANITIZE)
  target_compile_options(ceio_options INTERFACE -fsanitize=${CEIO_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(ceio_options INTERFACE -fsanitize=${CEIO_SANITIZE})
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(ceio_core
  src/sim/simulator.cpp
  src/sim/rng.cpp
  src/sim/stats.cpp
  src/hw/cache.cpp
  src/hw/dram.cpp
  src/hw/memory_system.cpp
  src/hw/pcie.cpp
  src/net/traffic.cpp
  src/net/link.cpp
  src/host/address_space.cpp
  src/host/buffer_pool.cpp
  src/host/app.cpp
  src/host/core.cpp
  src/nic/nic.cpp
  src/policy/credit_manager.cpp
  src/policy/ceio.cpp
  src/policy/hostcc.cpp
  src/net/congestion.cpp
  src/exp/scenario.cpp
  src/exp/experiment.cpp
  src/exp/presets.cpp)
add_library(ceio::core ALIAS ceio_core)
target_include_directories(ceio_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ceio_core PRIVATE ceio_options)
set_target_properties(ceio_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(ceio_run tools/ceio_run.cpp)
target_link_libraries(ceio_run PRIVATE ceio::core ceio_options)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CEIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
