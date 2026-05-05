# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/ — gtest unit tests, mirroring src/. See tests/unit/README.md.

# M2 chunk 2: a single smoke test that puts the public + internal header
# tree through gtest's translation unit. Catches name collisions between
# microtel and gtest (e.g., `internal::Test` vs gtest's `Test`) early.
microtel_add_test(
    NAME    headers_smoke_test
    SOURCES headers_smoke_test.cpp)
