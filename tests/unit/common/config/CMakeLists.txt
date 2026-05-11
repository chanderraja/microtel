# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-A unit tests: config layer (TOML parsing, env-var overlay, validation).

microtel_add_test(
    NAME    config_test
    SOURCES config_test.cpp
    LIBS    microtel_config)

target_include_directories(config_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
