# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The leaf's tests on other targets (issue #351): a project of its own, so a
# cross toolchain needs neither nghttp2, OpenSSL nor zlib for the target, and
# a bare-metal one no C++ at all. ci/scripts/leaf-target.sh drives it.
#
#   cmake -S tests/leaf/target -B build-target \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/<toolchain>.cmake \
#       -DMICROTEL_LEAF_TARGET_BOARD=<hosted|microbit|mps2-an386> \
#       [-DMICROTEL_LEAF_TARGET_GTEST=ON]
#
# It builds the leaf with both backends (the test-only archives of
# leaf/CMakeLists.txt) and, for each backend the board has room for, the C
# runner leaf_target_<backend> with stack measurement on. On a hosted board
# (a Linux process: aarch64 under qemu-aarch64, i686) MICROTEL_LEAF_TARGET_GTEST
# adds the full gtest leaf suite of tests/unit/leaf/. ctest runs everything,
# through CMAKE_CROSSCOMPILING_EMULATOR when that is set.

cmake_minimum_required(VERSION 3.20)

project(microtel_leaf_target LANGUAGES C)

set(MICROTEL_LEAF_TARGET_BOARD "hosted" CACHE STRING
    "Where the runner runs: hosted, microbit (Cortex-M0) or mps2-an386 (Cortex-M4)")
set_property(CACHE MICROTEL_LEAF_TARGET_BOARD PROPERTY STRINGS hosted microbit mps2-an386)
option(MICROTEL_LEAF_TARGET_GTEST "Also build the gtest leaf suite (hosted boards only)" OFF)

get_filename_component(MICROTEL_REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

if(MICROTEL_LEAF_TARGET_GTEST)
    if(NOT MICROTEL_LEAF_TARGET_BOARD STREQUAL "hosted")
        message(FATAL_ERROR "MICROTEL_LEAF_TARGET_GTEST needs MICROTEL_LEAF_TARGET_BOARD=hosted")
    endif()
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

enable_testing()

# The vendored encoders and the leaf with both backends, as the main build
# makes them when its tests are on: microtel_leaf (upb) and the test-only
# microtel_leaf_nanopb and microtel_leaf_dual.
set(MICROTEL_UPB_RENAME_HEADER ${MICROTEL_REPO}/third_party/upb/microtel_upb_rename.h)
set(MICROTEL_BUILD_TESTS ON)
set(MICROTEL_LEAF_ENCODER "upb" CACHE STRING "" FORCE)
add_subdirectory(${MICROTEL_REPO}/third_party/utf8_range utf8_range)
add_subdirectory(${MICROTEL_REPO}/third_party/upb upb)
add_subdirectory(${MICROTEL_REPO}/gen gen)
add_subdirectory(${MICROTEL_REPO}/third_party/nanopb nanopb)
add_subdirectory(${MICROTEL_REPO}/gen/nanopb gen_nanopb)
add_subdirectory(${MICROTEL_REPO}/leaf leaf)

include(${CMAKE_CURRENT_SOURCE_DIR}/leaf_target.cmake)

if(MICROTEL_LEAF_TARGET_BOARD STREQUAL "microbit")
    # 16 KiB of RAM: the nanopb runner only, with the vectors' record buffer
    # cut from 16 KiB to what the largest vector needs.
    microtel_leaf_target_runner(leaf_target_nanopb
        BACKEND nanopb LIB microtel_leaf_nanopb PLATFORM cortex-m MEASURE_STACK
        LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/microbit.ld
        DEFINES MICROTEL_LEAF_VECTORS_RECORD_BYTES=6144)
    # The control: an unaligned load that must fault, so the runner's buffers
    # at odd offsets are known to run on a core that would catch a bad access.
    microtel_leaf_target_runner(leaf_target_alignment_control
        BACKEND nanopb LIB microtel_leaf_nanopb PLATFORM cortex-m
        LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/microbit.ld
        DEFINES MICROTEL_LEAF_VECTORS_RECORD_BYTES=6144 LEAF_TARGET_EXPECT_ALIGNMENT_FAULT=1)
elseif(MICROTEL_LEAF_TARGET_BOARD STREQUAL "mps2-an386")
    foreach(backend upb nanopb)
        set(lib microtel_leaf_${backend})
        if(backend STREQUAL "upb")
            set(lib microtel_leaf)
        endif()
        microtel_leaf_target_runner(leaf_target_${backend}
            BACKEND ${backend} LIB ${lib} PLATFORM cortex-m MEASURE_STACK
            LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/mps2_an386.ld)
    endforeach()
elseif(MICROTEL_LEAF_TARGET_BOARD STREQUAL "hosted")
    microtel_leaf_target_runner(leaf_target_upb
        BACKEND upb LIB microtel_leaf PLATFORM hosted MEASURE_STACK)
    microtel_leaf_target_runner(leaf_target_nanopb
        BACKEND nanopb LIB microtel_leaf_nanopb PLATFORM hosted MEASURE_STACK)
    add_test(NAME leaf_target_upb COMMAND leaf_target_upb)
    add_test(NAME leaf_target_nanopb COMMAND leaf_target_nanopb)
else()
    message(FATAL_ERROR "unknown MICROTEL_LEAF_TARGET_BOARD '${MICROTEL_LEAF_TARGET_BOARD}'")
endif()

if(MICROTEL_LEAF_TARGET_GTEST)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # What tests/CMakeLists.txt gives the unit tests, for tests/unit/leaf/.
    add_library(microtel_headers INTERFACE)
    target_include_directories(microtel_headers INTERFACE ${MICROTEL_REPO}/include)
    function(microtel_add_test)
        cmake_parse_arguments(MT "" "NAME" "SOURCES;LIBS" ${ARGN})
        add_executable(${MT_NAME} ${MT_SOURCES})
        target_link_libraries(${MT_NAME} PRIVATE
            microtel_headers GTest::gtest GTest::gtest_main ${MT_LIBS})
        target_include_directories(${MT_NAME} PRIVATE ${MICROTEL_REPO}/tests)
        target_compile_options(${MT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME ${MT_NAME} COMMAND ${MT_NAME})
    endfunction()
    add_subdirectory(${MICROTEL_REPO}/tests/unit/leaf unit_leaf)
endif()
