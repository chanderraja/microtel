# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/sugar/ — the v1.1 sugar layer (ICP 0028).
#
# Sugar mirrors no src/ directory: it is header-only, adds no file under
# src/, and calls nothing but the public API. These tests run over
# tests/fakes/fake_tracer.hpp and tests/fakes/fake_span.hpp, so they need
# nothing on the link line beyond the header target microtel_add_test
# already provides.

microtel_add_test(
    NAME    sugar_span_test
    SOURCES span_test.cpp)

microtel_add_test(
    NAME    sugar_attr_key_test
    SOURCES attr_key_test.cpp)

microtel_add_test(
    NAME    sugar_exception_test
    SOURCES exception_test.cpp)
