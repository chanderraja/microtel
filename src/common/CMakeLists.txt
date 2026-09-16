# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/common/ — first real source code in microtel.
#
# Holds the shared, layer-independent services: logging sink injection,
# diagnostics counters, error helpers, byte-budget constants. M2 lands the
# logging plumbing; M3+ adds the rest as production tracks unblock.

add_library(microtel_common STATIC
    log_sink.cpp)

target_link_libraries(microtel_common
    PUBLIC microtel_headers)

target_include_directories(microtel_common
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_common PUBLIC cxx_std_20)

target_compile_options(microtel_common PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

# microtel_common does NOT link spdlog — issue #190, resolved by ICP 0026 §6
# with option (1).
#
# It used to, under MICROTEL_USE_SPDLOG, against the day the internal log
# emitter would call into it. That day is now cancelled: log_sink.cpp routes
# through the public LogSink hook and falls back to stderr, in every build
# configuration, and an application that wants spdlog installs
# microtel_spdlog_bridge (src/adapters/spdlog) as its sink, inside its own
# build. So the link contributed zero symbols and could never contribute any,
# and the same went for the MICROTEL_USE_SPDLOG compile definition, which no
# source in this directory ever tested.
#
# Dropping both makes the claim checkable rather than merely intended:
# libmicrotel_common.a cannot acquire an undefined spdlog reference, which is
# the defect #190 exists to prevent. The $<BUILD_INTERFACE:> wrapping in the
# install rules and the absent find_dependency(spdlog) in
# cmake/microtelConfig.cmake.in therefore stay correct as written, and the
# package keeps recording the option as `microtel_WITH_SPDLOG` for consumers
# that want to know whether the bridge target was built.
#
# MICROTEL_USE_SPDLOG survives as an option because it still gates something
# real: the bridge adapter and its tests.

# M6-A: configuration layer (TOML parsing, env-var overlay, validation).
add_subdirectory(config)
