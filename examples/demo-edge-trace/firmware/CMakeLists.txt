# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The demo firmware: a project of its own (like tests/leaf/target), so the
# cross build needs no nghttp2, OpenSSL, zlib or C++ for the target.
#
#   cmake -S examples/demo-edge-trace/firmware -B build-fw -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake \
#       -DMICROTEL_LEAF_CPU=cortex-m4 -DCMAKE_BUILD_TYPE=MinSizeRel
#   cmake --build build-fw          # -> build-fw/edge_sensor.elf

cmake_minimum_required(VERSION 3.20)
project(microtel_demo_edge_firmware LANGUAGES C)

get_filename_component(MICROTEL_REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

# The vendored upb encoder and the leaf, as tests/leaf/target builds them.
set(MICROTEL_UPB_RENAME_HEADER ${MICROTEL_REPO}/third_party/upb/microtel_upb_rename.h)
set(MICROTEL_LEAF_ENCODER "upb" CACHE STRING "" FORCE)
include(${MICROTEL_REPO}/cmake/MicrotelWarnings.cmake)
add_subdirectory(${MICROTEL_REPO}/third_party/utf8_range utf8_range)
add_subdirectory(${MICROTEL_REPO}/third_party/upb upb)
add_subdirectory(${MICROTEL_REPO}/gen gen)
add_subdirectory(${MICROTEL_REPO}/leaf leaf)

add_executable(edge_sensor.elf edge_sensor.c board_mps2.c)
target_link_libraries(edge_sensor.elf PRIVATE microtel_leaf)
set_target_properties(edge_sensor.elf PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(edge_sensor.elf PRIVATE -Wall -Wextra)
target_link_options(edge_sensor.elf PRIVATE
    --specs=nano.specs --specs=nosys.specs -nostartfiles
    -T${CMAKE_CURRENT_SOURCE_DIR}/mps2_an386.ld -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/edge_sensor.map)
set_property(TARGET edge_sensor.elf APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/mps2_an386.ld)
