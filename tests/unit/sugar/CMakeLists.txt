# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/sugar/ — the v1.1 sugar layer (ICP 0028).
#
# Sugar mirrors no src/ directory: it is header-only, adds no file under
# src/, and calls nothing but the public API, so these tests run over
# tests/fakes/fake_tracer.hpp and tests/fakes/fake_span.hpp with no production
# span pipeline behind them.
#
# microtel_sugar is named here because ICP 0028 §4 deliberately does not
# export it, and an in-tree target nothing links is a target nothing checks.
# It carries headers only (per the ICP), so the tests that hold a ScopedSpan
# add microtel_sdk for the two out-of-line symbols the handle needs:
# ScopedContext::~ScopedContext, and internal::SpanDeleter::operator() beside
# the no-op singleton it dispatches for. A consumer gets both from
# microtel::microtel.

microtel_add_test(
    NAME    sugar_span_test
    SOURCES span_test.cpp
    LIBS    microtel_sugar microtel_sdk)

microtel_add_test(
    NAME    sugar_attr_key_test
    SOURCES attr_key_test.cpp
    LIBS    microtel_sugar)

microtel_add_test(
    NAME    sugar_exception_test
    SOURCES exception_test.cpp
    LIBS    microtel_sugar)
