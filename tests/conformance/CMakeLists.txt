# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Conformance test tree.  Exercises a real OpenTelemetry Collector container
# (see ci/scripts/conformance.sh); without one the tests skip rather than fail,
# so a plain `ctest` on a developer box stays green.
#
# Mirrors tests/integration/CMakeLists.txt with one deliberate difference:
# ${CMAKE_SOURCE_DIR}/src is NOT on the include path.  Conformance tests assert
# what microtel *ships* — if a test needs an internal header to make its point,
# it belongs in the integration tier instead.

function(microtel_add_conformance_test)
    cmake_parse_arguments(MT
        ""
        "NAME"
        "SOURCES;LIBS"
        ${ARGN})

    if(NOT MT_NAME)
        message(FATAL_ERROR "microtel_add_conformance_test: NAME is required")
    endif()
    if(NOT MT_SOURCES)
        message(FATAL_ERROR "microtel_add_conformance_test: SOURCES is required")
    endif()

    add_executable(${MT_NAME} ${MT_SOURCES})
    target_link_libraries(${MT_NAME} PRIVATE
        microtel_headers
        GTest::gtest
        GTest::gtest_main
        ${MT_LIBS})
    # Public headers only, plus tests/ for the shared support headers.
    target_include_directories(${MT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(${MT_NAME} PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${MT_NAME} COMMAND ${MT_NAME})
    # The timeout is per-binary and covers a flush plus a 15 s poll of the
    # collector's output file; a hung export must fail the job, not stall it.
    set_tests_properties(${MT_NAME} PROPERTIES LABELS "conformance" TIMEOUT 120)
endfunction()

add_subdirectory(http)
add_subdirectory(grpc)
# The leaf / concentrator end-to-end gate needs both halves compiled in.
if(TARGET microtel_leaf AND MICROTEL_WITH_CONCENTRATOR)
    add_subdirectory(leaf)
endif()
