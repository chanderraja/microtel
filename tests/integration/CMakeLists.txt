# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration test tree.  Uses real OS resources (sockets, timers).
# Mirrors tests/CMakeLists.txt helper but without the unit-test label.

function(microtel_add_integration_test)
    cmake_parse_arguments(MT
        ""
        "NAME"
        "SOURCES;LIBS"
        ${ARGN})

    if(NOT MT_NAME)
        message(FATAL_ERROR "microtel_add_integration_test: NAME is required")
    endif()
    if(NOT MT_SOURCES)
        message(FATAL_ERROR "microtel_add_integration_test: SOURCES is required")
    endif()

    add_executable(${MT_NAME} ${MT_SOURCES})
    target_link_libraries(${MT_NAME} PRIVATE
        microtel_headers
        GTest::gtest
        GTest::gtest_main
        ${MT_LIBS})
    target_include_directories(${MT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${MT_NAME} PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${MT_NAME} COMMAND ${MT_NAME})
endfunction()

add_subdirectory(transport)
add_subdirectory(otelcpp_shim)
