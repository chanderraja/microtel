# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Linux on 64-bit Arm (Cortex-A), cross-compiled with Debian / Ubuntu's
# gcc-aarch64-linux-gnu (and g++-aarch64-linux-gnu for the C++ tests). Used by
# the leaf-footprint CI job (docs/leaf-concentrator-design.md §7.6) and by the
# leaf-target job, which runs the leaf tests under qemu-aarch64
# (ci/scripts/leaf-target.sh):
#
#   cmake -S leaf -B build-a64 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#       -DMICROTEL_LEAF_ENCODER=upb

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_AR aarch64-linux-gnu-ar)
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib)

set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
