# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/ — gtest unit tests, mirroring src/. See tests/unit/README.md.

# M2 chunk 2: smoke test that puts the public + internal header tree
# through gtest's translation unit. Catches name collisions between
# microtel and gtest (e.g., `internal::Test` vs gtest's `Test`) early.
microtel_add_test(
    NAME    headers_smoke_test
    SOURCES headers_smoke_test.cpp)

# M2 chunk 3: compile + link verification for the dumb mocks under
# tests/mocks/. Real behavioural tests using these mocks land in M3.
microtel_add_test(
    NAME    mocks_smoke_test
    SOURCES mocks_smoke_test.cpp)

# M2 chunk 4: compile + link verification for the logic-bearing fakes
# under tests/fakes/. Exercises the scripting/recording machinery that
# distinguishes a fake from a mock; real behavioural use lands in M3.
microtel_add_test(
    NAME    fakes_smoke_test
    SOURCES fakes_smoke_test.cpp)

# M2 chunk 6: behavioural test of the SetLogSink / ResetLogSink public
# API + the internal LogImpl entry point. Links the real production code
# from src/common/.
microtel_add_test(
    NAME    log_sink_test
    SOURCES log_sink_test.cpp
    LIBS    microtel_common)
target_include_directories(log_sink_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M3 — Track A unit tests. Mirrors src/sdk/.
add_subdirectory(sdk)

# M3 — Track F unit tests. Mirrors src/wire/encoder/.
add_subdirectory(wire)

# M3 — Track D unit tests. Mirrors src/transport/.
add_subdirectory(transport)

# M3 — Track D RAII wrappers. Mirrors src/common/raii/.
add_subdirectory(common)

# M3 close — export pipeline. Mirrors src/exporter/.
add_subdirectory(exporter)

# M6-D — preflight CLI logic.
add_subdirectory(preflight)

# M12 — public API contract tests (Meter, instruments via Provider interface).
add_subdirectory(api)

# M14 L6 — spdlog bridge adapter (only when spdlog is available).
if(MICROTEL_USE_SPDLOG)
    add_subdirectory(adapters)
endif()
