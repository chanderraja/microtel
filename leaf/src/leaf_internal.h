/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file leaf_internal.h
 * @brief The contract between the leaf core and an encoder backend
 *        (docs/leaf-concentrator-design.md §2.2). Not installed.
 *
 * The core decides **what** is emitted: which spans, which fields are present,
 * attribute order and timestamp values. A backend decides only **how** the
 * bytes are produced, and owns no state between calls. Every presence decision
 * is therefore made once, here, which is what lets two backends produce the
 * same bytes (§2.3). A backend must follow these rules:
 *
 *  1. Fields in ascending field-number order.
 *  2. proto3 scalars and strings equal to their default (0, false, empty) are
 *     omitted, except the `AnyValue` member, which is always written (it is a
 *     `oneof`). `ResourceSpans.resource` and `ScopeSpans.scope` are always
 *     present, even when empty; `Span.status` only when `has_status` is set;
 *     `parent_span_id` only when `parent_span_id` is non-NULL.
 *  3. Spans, attributes and events in the order the cursors return them.
 *  4. Timestamps as `fixed64`; doubles from their bit pattern; `int64`
 *     negatives as ten-byte varints.
 *  5. Exactly one `ResourceSpans` and one `ScopeSpans`; no `schema_url`.
 *  6. Only the four scalar `AnyValue` alternatives.
 *
 * The views returned here point into the record buffer and are valid until the
 * core's next mutating call. A backend may call the cursor functions as many
 * times as it likes (nanopb runs callbacks twice); they are pure functions of
 * the batch.
 */

#ifndef MICROTEL_LEAF_INTERNAL_H
#define MICROTEL_LEAF_INTERNAL_H

#include "microtel/leaf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Most reserved Resource attributes one payload carries (design §3.8). */
#define MICROTEL_LEAF_INTERNAL_MAX_RESERVED 7u

/** @brief Iteration position. Start every sequence with a zeroed cursor, `{0u, 0u}`. */
typedef struct microtel_leaf_internal_cursor
{
    uint32_t pos;
    uint32_t index;
} microtel_leaf_internal_cursor;

/** @brief One span, with every value final: timestamps already converted. */
typedef struct microtel_leaf_internal_span
{
    const uint8_t* trace_id;       /**< 16 bytes, non-zero */
    const uint8_t* span_id;        /**< 8 bytes, non-zero */
    const uint8_t* parent_span_id; /**< 8 bytes, or NULL for no parent */
    const char* name;
    size_t name_len;
    uint32_t kind;
    uint64_t start_time_unix_nano;
    uint64_t end_time_unix_nano;
    int has_status; /**< emit Span.status */
    uint32_t status_code;
    const char* status_message;
    size_t status_message_len;
    size_t attr_count;
    size_t event_count;
    uint32_t chain; /**< private to the core */
} microtel_leaf_internal_span;

/** @brief One span event. */
typedef struct microtel_leaf_internal_event
{
    uint64_t time_unix_nano;
    const char* name;
    size_t name_len;
    size_t attr_count;
    const uint8_t* attrs; /**< private to the core */
} microtel_leaf_internal_event;

/** @brief The read-only, backend-neutral view of one payload. */
typedef struct microtel_leaf_internal_batch
{
    const void* core;      /**< private to the core */
    size_t resource_count; /**< configured plus reserved attributes */
    const char* scope_name;
    size_t scope_name_len;
    const char* scope_version;
    size_t scope_version_len;
    size_t span_count; /**< ended spans */
    microtel_leaf_kv_t reserved[MICROTEL_LEAF_INTERNAL_MAX_RESERVED];
    size_t reserved_count;
    uint64_t time_offset; /**< private to the core */
} microtel_leaf_internal_batch;

/** @brief Where a backend writes the payload: exactly one of `buf` and `write` is set. */
typedef struct microtel_leaf_internal_sink
{
    uint8_t* buf; /**< buffer mode (may be NULL with cap 0 in buffer mode) */
    size_t cap;
    int (*write)(void* ctx, const uint8_t* bytes, size_t len); /**< streaming mode */
    void* write_ctx;
} microtel_leaf_internal_sink;

/** @brief Next Resource attribute: the configured ones, then the reserved ones. */
int microtel_leaf_internal_next_resource_attr(const microtel_leaf_internal_batch* batch,
                                              microtel_leaf_internal_cursor* cursor,
                                              microtel_leaf_kv_t* out);

/** @brief Next ended span, in end order. Returns 0 at the end. */
int microtel_leaf_internal_next_span(const microtel_leaf_internal_batch* batch,
                                     microtel_leaf_internal_cursor* cursor,
                                     microtel_leaf_internal_span* out);

/** @brief Next attribute of `span`, in first-set order. Returns 0 at the end. */
int microtel_leaf_internal_next_attr(const microtel_leaf_internal_batch* batch,
                                     const microtel_leaf_internal_span* span,
                                     microtel_leaf_internal_cursor* cursor,
                                     microtel_leaf_kv_t* out);

/** @brief Next event of `span`, in add order. Returns 0 at the end. */
int microtel_leaf_internal_next_event(const microtel_leaf_internal_batch* batch,
                                      const microtel_leaf_internal_span* span,
                                      microtel_leaf_internal_cursor* cursor,
                                      microtel_leaf_internal_event* out);

/** @brief Next attribute of `event`. Returns 0 at the end. */
int microtel_leaf_internal_next_event_attr(const microtel_leaf_internal_event* event,
                                           microtel_leaf_internal_cursor* cursor,
                                           microtel_leaf_kv_t* out);

/**
 * @brief The upb backend (backend_upb.c).
 *
 * Both backends share this signature so the core names its backend through one
 * macro, MICROTEL_LEAF_BACKEND_ENCODE, and a test-only build can link both. The
 * nanopb backend ignores `scratch`.
 *
 * @param written on MICROTEL_LEAF_OK the payload size; on
 *                MICROTEL_LEAF_ERR_BUFFER_SMALL the size needed
 */
microtel_leaf_status_t microtel_leaf_internal_encode_upb(const microtel_leaf_internal_batch* batch,
                                                         const microtel_leaf_internal_sink* sink,
                                                         void* scratch,
                                                         size_t scratch_size,
                                                         size_t* written);

/** @brief The nanopb backend (backend_nanopb.c); same contract, `scratch` unused. */
microtel_leaf_status_t microtel_leaf_internal_encode_nanopb(
    const microtel_leaf_internal_batch* batch,
    const microtel_leaf_internal_sink* sink,
    void* scratch,
    size_t scratch_size,
    size_t* written);

/**
 * @brief Test-only: forwards to the backend selected at run time
 *        (tests/leaf/dual/). Defined only in `microtel_leaf_dual`, which links
 *        both backends for the byte-identity comparison (§7.2); no shipped
 *        build contains it.
 */
microtel_leaf_status_t microtel_leaf_internal_encode_dual(const microtel_leaf_internal_batch* batch,
                                                          const microtel_leaf_internal_sink* sink,
                                                          void* scratch,
                                                          size_t scratch_size,
                                                          size_t* written);

#ifdef __cplusplus
}
#endif

#endif /* MICROTEL_LEAF_INTERNAL_H */
