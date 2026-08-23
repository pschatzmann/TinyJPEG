# When this file is processed by the ESP-IDF build system (i.e. TinyJPEG is
# used as an ESP-IDF component - either dropped into a project's components/
# dir or referenced via EXTRA_COMPONENT_DIRS), register it the IDF way and
# stop: a component CMakeLists.txt must not call project()/
# cmake_minimum_required() itself (that's the enclosing IDF project's job).
# ESP_PLATFORM is the variable ESP-IDF's build system sets for exactly this
# kind of branch - see the IDF docs, "Behind the Scenes" section:
# "The CMake variable ESP_PLATFORM is set to 1 whenever the ESP-IDF build
# system is being used. Tests such as if (ESP_PLATFORM) can be used in
# generic CMake code if special IDF-specific logic is required."
if(ESP_PLATFORM)
    idf_component_register(
        INCLUDE_DIRS "src"
    )
    return()
endif()

cmake_minimum_required(VERSION 3.14)
project(TinyJPEG CXX)

# TinyJPEG is header-only (see src/) - the primary distribution mechanism is
# the Arduino Library Manager (library.properties), not CMake. This file
# exists for three things CMake is actually useful for: (1) letting a
# non-Arduino CMake-based project consume the library via
# add_subdirectory() + target_link_libraries(), (2) building/running the
# desktop native test suite (test/native/) - this project's correctness
# oracle, and (3) - handled by the ESP_PLATFORM branch above - being
# consumed as an ESP-IDF component. None of this is needed for, or affects,
# the Arduino/arduino-cli workflow.
#
# Structure mirrors the sibling TinyMPG/TinyH264 projects (same author,
# same constrained-microcontroller target, same header-only/no-hot-path-
# allocation approach), adapted for a decode-only, single-directory
# (src/) library instead of decoder/encoder subdirectories.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Header-only library target: no .cpp files of its own, so this is an
# INTERFACE target - it only contributes an include path and a C++17
# requirement to whatever links against it.
add_library(TinyJPEG INTERFACE)
add_library(TinyJPEG::TinyJPEG ALIAS TinyJPEG)
target_include_directories(TinyJPEG INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Propagated to consumers too: the codebase actually requires C++17 (not
# just this build of it - e.g. TinyJPEGDecoder.h's context objects use
# alignas, and the FileT/InputCallback templates rely on C++17 class
# template argument deduction friendliness), so a project linking against
# TinyJPEG without C++17 enabled should fail to configure rather than fail
# with confusing compiler errors deep inside a header.
target_compile_features(TinyJPEG INTERFACE cxx_std_17)

# Only build the test suite when this is the top-level CMake project (i.e.
# not when some other project add_subdirectory()s this one just to get the
# TinyJPEG target) - portable equivalent of CMake 3.21's
# PROJECT_IS_TOP_LEVEL, kept for compatibility with the wider range of
# CMake versions this cmake_minimum_required allows.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TINYJPEG_IS_TOP_LEVEL TRUE)
else()
    set(TINYJPEG_IS_TOP_LEVEL FALSE)
endif()
option(TINYJPEG_BUILD_TESTS "Build the desktop native test suite (test/native/)" ${TINYJPEG_IS_TOP_LEVEL})

if(TINYJPEG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/native)
endif()
