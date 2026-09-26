# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Leaf unit and golden-vector tests (docs/leaf-concentrator-design.md §7.1,
# §7.2). One binary per backend, from the same sources:
#
#   microtel_leaf_upb_test      against the upb backend
#   microtel_leaf_nanopb_test   against the nanopb backend
#
# The configured backend (MICROTEL_LEAF_ENCODER) is tested through the shipped
# `microtel_leaf` archive; the other through the test-only
# `microtel_leaf_<backend>` that leaf/CMakeLists.txt builds beside it. The
# sources see MICROTEL_LEAF_TEST_BACKEND_UPB or _NANOPB for the few tests that
# belong to one backend.
#
# microtel_leaf_backend_diff_test links `microtel_leaf_dual` (both backends in
# one binary) and compares their bytes for every golden vector and for a fixed
# set of random builder programs, the deterministic half of the
# leaf_backend_diff_fuzz target (tests/fuzz/).
#
# The golden-vector scenarios (tests/leaf/vectors/) are C11 and are compiled as
# such, with the same no-extensions posture as the leaf itself.
#
# microtel_leaf_target_<backend>_test is the C runner of tests/leaf/target/ (no
# gtest, no heap), which ci/scripts/leaf-target.sh also runs on bare-metal
# Cortex-M under QEMU; here it runs on the host, like the rest of the suite.
#
# Paths are relative to this directory, not CMAKE_SOURCE_DIR, because
# tests/leaf/target/CMakeLists.txt also adds this directory to a project of its
# own when it cross-builds the suite for aarch64 and i686.

get_filename_component(MICROTEL_LEAF_TEST_REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(MICROTEL_LEAF_VECTORS_SOURCE ${MICROTEL_LEAF_TEST_REPO}/tests/leaf/vectors/leaf_vectors.c)

# The tests decode each payload with upb; they reach gen/ and the upb runtime
# headers, with the rename header forced in exactly as the encoder's tests do.
set(MICROTEL_LEAF_TEST_CXX_SOURCES
    leaf_test.cpp
    leaf_golden_test.cpp
    leaf_backend_diff_test.cpp
    ${MICROTEL_LEAF_TEST_REPO}/tests/leaf/diff/leaf_diff_program.cpp)
set_source_files_properties(${MICROTEL_LEAF_TEST_CXX_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${MICROTEL_UPB_RENAME_HEADER};-Wno-missing-field-initializers")

set_source_files_properties(${MICROTEL_LEAF_VECTORS_SOURCE} PROPERTIES
    COMPILE_OPTIONS "-std=c11;-pedantic-errors;-Wvla")

function(microtel_leaf_test name)
    cmake_parse_arguments(LT "" "" "SOURCES;LIBS;DEFINES" ${ARGN})
    microtel_add_test(
        NAME    ${name}
        SOURCES ${LT_SOURCES}
        LIBS    ${LT_LIBS} microtel_upb_gen)
    target_include_directories(${name}
        PRIVATE
            ${MICROTEL_LEAF_TEST_REPO}/gen
            ${MICROTEL_LEAF_TEST_REPO}/third_party/upb)
    target_compile_definitions(${name} PRIVATE
        MICROTEL_LEAF_VECTORS_DIR="${MICROTEL_LEAF_TEST_REPO}/tests/leaf/vectors"
        ${LT_DEFINES})
endfunction()

foreach(backend upb nanopb)
    if(backend STREQUAL MICROTEL_LEAF_ENCODER)
        set(backend_lib microtel_leaf)
    else()
        set(backend_lib microtel_leaf_${backend})
    endif()
    string(TOUPPER ${backend} backend_upper)
    microtel_leaf_test(microtel_leaf_${backend}_test
        SOURCES leaf_test.cpp leaf_golden_test.cpp ${MICROTEL_LEAF_VECTORS_SOURCE}
        LIBS    ${backend_lib}
        DEFINES MICROTEL_LEAF_TEST_BACKEND_${backend_upper}=1)
endforeach()

microtel_leaf_test(microtel_leaf_backend_diff_test
    SOURCES leaf_backend_diff_test.cpp
            ${MICROTEL_LEAF_TEST_REPO}/tests/leaf/diff/leaf_diff_program.cpp
            ${MICROTEL_LEAF_VECTORS_SOURCE}
    LIBS    microtel_leaf_dual)

include(${MICROTEL_LEAF_TEST_REPO}/tests/leaf/target/leaf_target.cmake)
foreach(backend upb nanopb)
    if(backend STREQUAL MICROTEL_LEAF_ENCODER)
        set(backend_lib microtel_leaf)
    else()
        set(backend_lib microtel_leaf_${backend})
    endif()
    microtel_leaf_target_runner(microtel_leaf_target_${backend}_test
        BACKEND ${backend} LIB ${backend_lib} PLATFORM hosted)
    add_test(NAME microtel_leaf_target_${backend}_test COMMAND microtel_leaf_target_${backend}_test)
    set_tests_properties(microtel_leaf_target_${backend}_test PROPERTIES LABELS "unit")
endforeach()
