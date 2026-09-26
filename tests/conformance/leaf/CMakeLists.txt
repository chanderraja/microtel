# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The leaf / concentrator ship gate against a real collector (ICP 0031 gates 3
# and 5, docs/leaf-concentrator-design.md §7.5). One source, one binary per
# encoder backend; each binary runs every test over OTLP/HTTP and OTLP/gRPC.
#
# The configured backend (MICROTEL_LEAF_ENCODER) is linked through the shipped
# `microtel_leaf`, the other through the test-only `microtel_leaf_<backend>`
# that leaf/CMakeLists.txt builds when tests are on, as tests/unit/leaf/ does.
# Public headers only: leaf.h and the C++ API.

foreach(backend upb nanopb)
    if(backend STREQUAL MICROTEL_LEAF_ENCODER)
        set(backend_lib microtel_leaf)
    else()
        set(backend_lib microtel_leaf_${backend})
    endif()
    string(TOUPPER ${backend} backend_upper)
    microtel_add_conformance_test(
        NAME    conformance_leaf_${backend}_test
        SOURCES leaf_e2e_test.cpp
        LIBS    microtel_sdk ${backend_lib})
    target_compile_definitions(conformance_leaf_${backend}_test PRIVATE
        MICROTEL_LEAF_E2E_BACKEND_${backend_upper}=1)
endforeach()
