# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Bare-metal Arm Cortex-M, for the standalone leaf build (cmake -S leaf) only;
# the C++ runtime is not built for a target without an OS. Used by the
# leaf-footprint CI job (docs/leaf-concentrator-design.md §7.6):
#
#   cmake -S leaf -B build-m4 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake \
#       -DMICROTEL_LEAF_CPU=cortex-m4
#
# MICROTEL_LEAF_CPU is any -mcpu the compiler accepts (default cortex-m0plus).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(MICROTEL_LEAF_CPU "cortex-m0plus" CACHE STRING "Cortex-M core passed as -mcpu")

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)

# No OS to run a test executable on: compiler checks build a static library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT
    "-mcpu=${MICROTEL_LEAF_CPU} -mthumb -ffunction-sections -fdata-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
