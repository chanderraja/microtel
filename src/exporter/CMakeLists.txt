# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/exporter/ — protocol-agnostic OTLP export pipeline (Track A, M3 close).

add_library(microtel_exporter STATIC
    otlp_exporter.cpp
    otlp_metric_exporter.cpp
    otlp_log_exporter.cpp
    retry_policy.cpp)

target_link_libraries(microtel_exporter
    PUBLIC microtel_headers)

target_include_directories(microtel_exporter
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_exporter PUBLIC cxx_std_20)

target_compile_options(microtel_exporter PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
