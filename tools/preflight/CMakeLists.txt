# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-D — preflight CLI binary.
# microtel-preflight --preflight={connect|export} [config.toml]

# Shared library so the test target can link against the logic without main().
add_library(microtel_preflight_lib STATIC preflight.cpp)

target_link_libraries(microtel_preflight_lib
    PUBLIC  microtel_sdk
    PRIVATE microtel_headers)

target_include_directories(microtel_preflight_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_preflight_lib PUBLIC cxx_std_20)

target_compile_options(microtel_preflight_lib PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

# The runnable binary.
add_executable(microtel-preflight main.cpp)

target_link_libraries(microtel-preflight PRIVATE microtel_preflight_lib)

target_include_directories(microtel-preflight
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_features(microtel-preflight PRIVATE cxx_std_20)

target_compile_options(microtel-preflight PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
