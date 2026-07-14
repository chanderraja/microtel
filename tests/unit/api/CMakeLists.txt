# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/api/ — public API contract tests exercised through the abstract
# microtel::Provider / microtel::Meter interfaces (not SDK internals directly).

microtel_add_test(
    NAME    meter_api_test
    SOURCES meter_api_test.cpp
    LIBS    microtel_sdk)
target_include_directories(meter_api_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# SdkProviderArgs uses designated initializers; new fields added in M13 are
# intentionally omitted here (defaults are sufficient for these API tests).
target_compile_options(meter_api_test PRIVATE -Wno-missing-field-initializers)
