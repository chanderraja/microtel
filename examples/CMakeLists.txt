# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# examples/ — standalone, runnable demonstrations of the public microtel API.
# Built only when MICROTEL_BUILD_EXAMPLES=ON (see the top-level CMakeLists.txt
# and examples/README.md).
#
# One example is one directory: a main.cpp, a README.md, a CMakeLists.txt with
# a single microtel_add_example() call, and one add_subdirectory() line here.
# Nothing else in the tree changes when an example is added, so two examples
# landing in parallel touch only this file's list and cannot collide anywhere
# else.

# microtel_add_example(<name> [SOURCES <src>...])
#
# Defines the executable target `microtel_example_<name>` from the sources in
# the calling directory, linked against microtel_sdk — which transitively
# provides the public headers (via microtel_headers) and every runtime link
# dependency (nghttp2, OpenSSL, zlib).
#
# SOURCES defaults to main.cpp. Pass it when an example needs extra
# translation units, or when one directory builds more than one binary (a
# two-process example, say):
#
#   microtel_add_example(handoff_sender   SOURCES sender.cpp)
#   microtel_add_example(handoff_receiver SOURCES receiver.cpp)
#
# Every binary lands in ${CMAKE_BINARY_DIR}/examples regardless of which
# subdirectory defined it, so the documented run path stays
# ./build/examples/microtel_example_<name> as the tree grows.
#
# The warning flags match the project's own: an example that would not survive
# -Werror in a consumer's build is not an example worth shipping.
function(microtel_add_example name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")

    if(NOT arg_SOURCES)
        set(arg_SOURCES main.cpp)
    endif()

    set(target "microtel_example_${name}")

    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE microtel_sdk)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
endfunction()

add_subdirectory(auth_bearer)
add_subdirectory(basic_trace)
add_subdirectory(context_propagation)
add_subdirectory(distributed_handoff)
add_subdirectory(health_and_backpressure)
add_subdirectory(hot_reload)
add_subdirectory(leaf)
add_subdirectory(leaf_mqtt)
add_subdirectory(multi_profile)
add_subdirectory(resource_detectors)
add_subdirectory(sampler_chains)
add_subdirectory(sugar_tour)
add_subdirectory(tls)
