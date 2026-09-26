# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# 32-bit x86 Linux (size_t and pointers are 32 bits), cross-compiled with
# Debian / Ubuntu's gcc-i686-linux-gnu. An x86-64 host runs the result
# directly. Used by the leaf-target CI job (ci/scripts/leaf-target.sh i686):
#
#   cmake -S tests/leaf/target -B build-i686 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/i686-linux-gnu.cmake \
#       -DMICROTEL_LEAF_TARGET_GTEST=ON

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER i686-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER i686-linux-gnu-g++)
set(CMAKE_AR i686-linux-gnu-ar)
set(CMAKE_RANLIB i686-linux-gnu-ranlib)

set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")

set(CMAKE_FIND_ROOT_PATH /usr/i686-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
