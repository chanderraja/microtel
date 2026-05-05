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
if(MICROTEL_USE_SPDLOG)
    target_link_libraries(microtel_common PRIVATE spdlog::spdlog)
    target_compile_definitions(microtel_common PRIVATE MICROTEL_USE_SPDLOG=1)
endif()
