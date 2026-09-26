# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# leaf: a C leaf and a C++ concentrator over UDP (docs/leaf-concentrator-design.md
# §7.7). Needs -DMICROTEL_BUILD_LEAF=ON for the leaf half and, for the
# concentrator half, -DMICROTEL_WITH_CONCENTRATOR=ON; each half is skipped
# without its option.
#
# The two C programs are not microtel_add_example() targets, which are C++
# linked against microtel_sdk: they are C11 linked against the leaf library
# alone, the way firmware links it.

if(TARGET microtel_leaf)
    foreach(program udp_leaf size_probe)
        set(target microtel_example_leaf_${program})
        add_executable(${target} ${program}.c)
        target_link_libraries(${target} PRIVATE microtel_leaf)
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endforeach()
    # The probe's upb build takes caller-owned encode scratch (size_probe.c).
    if(MICROTEL_LEAF_ENCODER STREQUAL "upb")
        target_compile_definitions(microtel_example_leaf_size_probe PRIVATE PROBE_WITH_SCRATCH)
    endif()
else()
    message(STATUS "examples/leaf: leaf half skipped (needs -DMICROTEL_BUILD_LEAF=ON)")
endif()

if(MICROTEL_WITH_CONCENTRATOR)
    microtel_add_example(leaf_concentrator SOURCES udp_concentrator.cpp)
    target_compile_definitions(microtel_example_leaf_concentrator PRIVATE
        MICROTEL_EXAMPLE_LEAF_TOML="${CMAKE_CURRENT_SOURCE_DIR}/microtel.toml")
else()
    message(STATUS
        "examples/leaf: concentrator half skipped (needs -DMICROTEL_WITH_CONCENTRATOR=ON)")
endif()
