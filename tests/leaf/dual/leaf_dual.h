/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Test-only: `microtel_leaf_dual` is the leaf core linked with both encoder
 * backends (docs/leaf-concentrator-design.md §2.1, §7.2). Every encode goes to
 * the backend selected here, so one process can build the same spans twice and
 * compare the upb and nanopb bytes. No shipped build contains this.
 */

#ifndef MICROTEL_LEAF_DUAL_H
#define MICROTEL_LEAF_DUAL_H

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief The backend microtel_leaf_encode and microtel_leaf_encode_to use. */
typedef enum microtel_leaf_dual_backend
{
    MICROTEL_LEAF_DUAL_UPB = 0,
    MICROTEL_LEAF_DUAL_NANOPB = 1
} microtel_leaf_dual_backend;

/** @brief Selects the backend for every following encode. Not thread-safe. */
void microtel_leaf_dual_select(microtel_leaf_dual_backend backend);

#ifdef __cplusplus
}
#endif

#endif /* MICROTEL_LEAF_DUAL_H */
