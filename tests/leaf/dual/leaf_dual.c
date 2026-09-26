/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The run-time backend switch of the test-only `microtel_leaf_dual`
 * (leaf_dual.h). leaf_core.c is compiled there with
 * MICROTEL_LEAF_BACKEND_ENCODE=microtel_leaf_internal_encode_dual.
 */

#include "leaf_dual.h"

#include "leaf_internal.h"

static microtel_leaf_dual_backend s_backend = MICROTEL_LEAF_DUAL_UPB;

void microtel_leaf_dual_select(microtel_leaf_dual_backend backend)
{
    s_backend = backend;
}

microtel_leaf_status_t microtel_leaf_internal_encode_dual(const microtel_leaf_internal_batch* batch,
                                                          const microtel_leaf_internal_sink* sink,
                                                          void* scratch,
                                                          size_t scratch_size,
                                                          size_t* written)
{
    if (s_backend == MICROTEL_LEAF_DUAL_NANOPB)
    {
        return microtel_leaf_internal_encode_nanopb(batch, sink, scratch, scratch_size, written);
    }
    return microtel_leaf_internal_encode_upb(batch, sink, scratch, scratch_size, written);
}
