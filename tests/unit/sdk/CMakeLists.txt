# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/sdk/ (Track A). Mirrors src/sdk/ one-to-one per
# tests/unit/README.md.

# M3 — first production-code-driven test in microtel. Drives AlwaysOn
# sampler behaviour against the real production code in src/sdk/.
microtel_add_test(
    NAME    always_on_sampler_test
    SOURCES always_on_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    always_off_sampler_test
    SOURCES always_off_sampler_test.cpp
    LIBS    microtel_sdk)
