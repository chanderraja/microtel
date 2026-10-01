# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Test tree entry point. See tests/README.md for the taxonomy.
#
# `microtel_add_test(NAME <name> SOURCES <list>)` is the helper every leaf
# CMakeLists uses. It adds a gtest binary, wires gtest_main + microtel_headers,
# applies the standard warning posture, and registers the executable with
# ctest. M2 chunk 5 will fold sanitizer flags in here.

function(microtel_add_test)
    cmake_parse_arguments(MT
        ""                      # options
        "NAME"                  # one-value
        "SOURCES;LIBS"          # multi-value
        ${ARGN})

    if(NOT MT_NAME)
        message(FATAL_ERROR "microtel_add_test: NAME is required")
    endif()
    if(NOT MT_SOURCES)
        message(FATAL_ERROR "microtel_add_test: SOURCES is required")
    endif()

    add_executable(${MT_NAME} ${MT_SOURCES})
    target_link_libraries(${MT_NAME} PRIVATE
        microtel_headers
        GTest::gtest
        GTest::gtest_main
        ${MT_LIBS})
    # Tests include shared mocks/fakes via "mocks/..." and "fakes/...".
    target_include_directories(${MT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(${MT_NAME} PRIVATE
        -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})
    add_test(NAME ${MT_NAME} COMMAND ${MT_NAME})
    set_tests_properties(${MT_NAME} PROPERTIES LABELS "unit")
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(conformance)

# The dependency-closure gate, run against this build tree (CLAUDE.md rule 13,
# ICP 0020 Decision 4). The CI symbol-scan job scans one configuration only —
# effectively Release, concentrator OFF — and at -O2 upb's `UPB_INLINE`
# helpers are inlined away. At -O0 they are emitted as weak globals, so a
# helper missing from third_party/upb/microtel_upb_rename.h ships unprefixed
# in a Debug build and nowhere else. Registering the scan here runs it in
# every configuration that runs ctest, including the Debug coverage build.
# The build-dir form also sweeps test-only archives; they get the same rules.
# Coverage and sanitizer instrumentation define globals of their own, so those
# trees skip the leaf pass (--instrumented); the rename passes still run.
# GNU nm, as the CI job uses: the script parses its `nm -A` output format.
find_program(MICROTEL_BASH bash)
find_program(MICROTEL_GNU_NM nm)
if(MICROTEL_BASH AND MICROTEL_GNU_NM)
    set(MICROTEL_SYMBOL_SCAN_ARGS "")
    if(MICROTEL_COVERAGE OR NOT MICROTEL_SANITIZER STREQUAL "")
        set(MICROTEL_SYMBOL_SCAN_ARGS --instrumented)
    endif()
    add_test(NAME symbol_scan_build_tree
        COMMAND ${MICROTEL_BASH} ${CMAKE_SOURCE_DIR}/ci/scripts/symbol-scan.sh
                ${MICROTEL_SYMBOL_SCAN_ARGS} ${CMAKE_BINARY_DIR})
    set_tests_properties(symbol_scan_build_tree PROPERTIES
        LABELS "integration"
        ENVIRONMENT "NM=${MICROTEL_GNU_NM}")
endif()
