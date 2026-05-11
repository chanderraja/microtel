# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-D unit tests: preflight CLI argument parsing and config-error paths.

microtel_add_test(
    NAME    preflight_test
    SOURCES preflight_test.cpp
    LIBS    microtel_preflight_lib)
