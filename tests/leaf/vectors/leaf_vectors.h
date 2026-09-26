/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Golden-vector scenarios for the leaf (docs/leaf-concentrator-design.md
 * §7.2). Each vector builds one scenario with a fixed clock and a fixed random
 * source and encodes it. Every backend's test binary encodes every vector and
 * compares the bytes with the committed `<name>.bin` beside this file, so two
 * backends that both match the files match each other.
 *
 * Written in C so that the scenarios exercise the public header as C11 and
 * can be reused unchanged by a C fuzz target.
 */

#ifndef MICROTEL_LEAF_TEST_VECTORS_H
#define MICROTEL_LEAF_TEST_VECTORS_H

#include "microtel/leaf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Number of vectors. */
size_t microtel_leaf_test_vector_count(void);

/** Name of vector `index`; also the stem of its golden file. */
const char* microtel_leaf_test_vector_name(size_t index);

/**
 * Builds vector `index` and encodes it into `out`.
 * @return the status of microtel_leaf_encode, or of the first call that failed
 */
microtel_leaf_status_t microtel_leaf_test_vector_encode(size_t index,
                                                        uint8_t* out,
                                                        size_t out_size,
                                                        size_t* written);

#ifdef __cplusplus
}
#endif

#endif /* MICROTEL_LEAF_TEST_VECTORS_H */
