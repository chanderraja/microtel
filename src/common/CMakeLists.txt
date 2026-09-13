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

# When MICROTEL_USE_SPDLOG=ON the FetchContent target `spdlog::spdlog` is
# available; link it into common. M3+ uses it from the internal log
# emitter; M2 wires the dependency only.
#
# $<BUILD_INTERFACE:> keeps it out of the installed export set, and today that
# is accurate rather than convenient: the spdlog route has not landed (see
# log_sink.cpp — stderr in both modes), so libmicrotel_common.a references no
# spdlog symbol at all and a consumer of the installed package needs none.
# The generated microtelConfig.cmake records the option as
# `microtel_WITH_SPDLOG` but issues no find_dependency, because spdlog is
# FetchContent'd with SPDLOG_INSTALL=OFF and is therefore not in the package.
#
# When the M3+ route starts calling into spdlog, this stops being true and the
# installed package needs a real answer — issue #189.
if(MICROTEL_USE_SPDLOG)
    target_link_libraries(microtel_common PRIVATE $<BUILD_INTERFACE:spdlog::spdlog>)
    target_compile_definitions(microtel_common PRIVATE MICROTEL_USE_SPDLOG=1)
endif()

# M6-A: configuration layer (TOML parsing, env-var overlay, validation).
add_subdirectory(config)
