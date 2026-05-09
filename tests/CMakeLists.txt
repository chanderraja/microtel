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
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${MT_NAME} COMMAND ${MT_NAME})
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
