cmake_minimum_required(VERSION 3.15)
project(beamcommander3-backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Used by the Windows release job (.github/workflows/release.yml) to build
# laser_daemon.exe via CMake + MSVC, since macOS keeps using build.sh's raw
# clang++ command (untouched, still the source of truth for macOS/dev
# builds - see DEVELOPER.md). Point this at a checkout of
# https://github.com/sebleedelisle/libera-laser with submodules initialized:
#
#   cmake -S backend -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIBERA_DIR=/path/to/libera-laser
#   cmake --build build
#
if (NOT DEFINED LIBERA_DIR)
  message(FATAL_ERROR "Set -DLIBERA_DIR=/path/to/libera-laser (a checkout with submodules initialized)")
endif()

set(LIBERA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBERA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBERA_BUILD_LASER_TOOL OFF CACHE BOOL "" FORCE)
if (WIN32)
  # No system libusb/pkg-config on Windows - use the prebuilt libusb that
  # ships with libera-laser's vendored helios_dac submodule instead.
  set(LIBERA_USE_BUNDLED_LIBUSB ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${LIBERA_DIR}" "${CMAKE_BINARY_DIR}/libera-laser")

add_executable(laser_daemon laser_daemon.cpp)
target_link_libraries(laser_daemon PRIVATE libera-core)

if (WIN32)
  target_link_libraries(laser_daemon PRIVATE ws2_32)
endif()

# Backend unit tests (tests/test_*.cpp): the safety-critical hardware
# output pipeline (safety masks, mask outlines, blackout/arm gate, zone -
# see output_pipeline.h), show parameter smoothing (param_smoothing.h) and
# show effects like flicker (show_fx.h). Not part of the normal build - they
# run before every push (.githooks/pre-push). To build and run them with
# CMake/CTest:  -DBEAMCOMMANDER_TESTS=ON, then  ctest --test-dir build
option(BEAMCOMMANDER_TESTS "Build the backend unit tests" OFF)
if (BEAMCOMMANDER_TESTS)
  enable_testing()
  foreach(test_name output_pipeline param_smoothing show_fx)
    add_executable(${test_name}_tests tests/test_${test_name}.cpp)
    target_include_directories(${test_name}_tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}" "${LIBERA_DIR}/include")
    add_test(NAME ${test_name}_tests COMMAND ${test_name}_tests)
  endforeach()
endif()
