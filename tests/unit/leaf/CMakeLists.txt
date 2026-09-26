# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Leaf unit and golden-vector tests (docs/leaf-concentrator-design.md §7.1,
# §7.2). One binary per backend, from the same sources; the backend is the one
# microtel_leaf was built with (MICROTEL_LEAF_ENCODER).
#
# The golden-vector scenarios (tests/leaf/vectors/) are C11 and are compiled as
# such, with the same no-extensions posture as the leaf itself.

set(MICROTEL_LEAF_TEST_NAME microtel_leaf_${MICROTEL_LEAF_ENCODER}_test)

microtel_add_test(
    NAME    ${MICROTEL_LEAF_TEST_NAME}
    SOURCES leaf_test.cpp
            leaf_golden_test.cpp
            ${CMAKE_SOURCE_DIR}/tests/leaf/vectors/leaf_vectors.c
    LIBS    microtel_leaf microtel_upb_gen)

# The tests decode each payload with upb; they reach gen/ and the upb runtime
# headers, with the rename header forced in exactly as the encoder's tests do.
target_include_directories(${MICROTEL_LEAF_TEST_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/gen
        ${CMAKE_SOURCE_DIR}/third_party/upb)

set_source_files_properties(leaf_test.cpp leaf_golden_test.cpp PROPERTIES
    COMPILE_OPTIONS "-include;${MICROTEL_UPB_RENAME_HEADER};-Wno-missing-field-initializers")

set_source_files_properties(${CMAKE_SOURCE_DIR}/tests/leaf/vectors/leaf_vectors.c PROPERTIES
    COMPILE_OPTIONS "-std=c11;-pedantic-errors;-Wvla")

target_compile_definitions(${MICROTEL_LEAF_TEST_NAME} PRIVATE
    MICROTEL_LEAF_VECTORS_DIR="${CMAKE_SOURCE_DIR}/tests/leaf/vectors")
