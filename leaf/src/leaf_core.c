/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The leaf core (docs/leaf-concentrator-design.md §1): config, the record
 * buffer, span building, ids, clocks, and the backend-neutral batch view the
 * encoder backends read (§2.2). Includes no encoder header.
 *
 * Record buffer layout
 * --------------------
 *
 *   [ span table -> ...free... <- chunks | Resource + scope ]
 *   0              slot_count   data_low   persist_off      buf_size
 *
 * The span table grows from the front, one fixed-size record per slot. A
 * slot keeps its index for the life of its span, so a handle stays valid
 * across encodes; a handle also carries a generation, so one whose span has
 * been encoded is rejected even after the slot is reused.
 *
 * Everything variable-sized (names, attributes, events, status messages) is a
 * chunk in the region below the Resource, allocated downward. Each span links
 * its chunks in a list; the list order is the emit order, so an attribute
 * overwrite that no longer fits is spliced into the old one's place. A chunk
 * that is overwritten, or whose span has been encoded, is dead; compaction
 * (mark, fix references, slide up) reclaims dead chunks when an allocation
 * fails and after every successful encode. The Resource and scope are written
 * once at init, above the chunks, and never move.
 *
 * All access to records in the caller's buffer goes through memcpy, so the
 * buffer needs no particular alignment and no object is accessed through an
 * incompatible type.
 */

#include "leaf_internal.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

#define LEAF_MAGIC 0x4c454146u /* "LEAF" */
#define NONE32 0xffffffffu
#define NONE16 0xffffu
#define MAX_BUFFER 0xfffffff0u

#define DEFAULT_MAX_ATTRS 16u
#define DEFAULT_MAX_EVENTS 4u
#define DEFAULT_MAX_EVENT_ATTRS 4u

#define TRACE_ID_BYTES 16u
#define SPAN_ID_BYTES 8u
#define WORD_BYTES 8u
#define BITS_PER_BYTE 8u
#define BOOT_ID_BYTES 4u
#define BYTE_MASK 0xffu
#define GEN_SHIFT 16u
#define SLOT_MASK 0xffffu

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define SPLITMIX_GAMMA 0x9e3779b97f4a7c15ULL
#define SPLITMIX_MUL1 0xbf58476d1ce4e5b9ULL
#define SPLITMIX_MUL2 0x94d049bb133111ebULL
#define SPLITMIX_SHIFT1 30u
#define SPLITMIX_SHIFT2 27u
#define SPLITMIX_SHIFT3 31u

#define WIRE_PROTO_VERSION 1

/* Upper-bound sizes for microtel_leaf_encoded_size (§1.8). */
#define BOUND_TAG 2u     /* every field number here is < 2048 */
#define BOUND_LEN 5u     /* a length below 2^32 */
#define BOUND_VARINT 10u /* the longest varint */
#define BOUND_FIXED64 8u

static const char k_reserved_prefix[] = "microtel.leaf.";

enum span_state
{
    SLOT_FREE = 0,
    SLOT_OPEN = 1,
    SLOT_ENDED = 2
};

enum chunk_kind
{
    CHUNK_DEAD = 0,
    CHUNK_NAME = 1,
    CHUNK_ATTR = 2,
    CHUNK_EVENT = 3,
    CHUNK_STATUS = 4
};

/* ------------------------------------------------------------------------ */
/* Records kept in the caller's buffer                                      */
/* ------------------------------------------------------------------------ */

typedef struct span_rec
{
    uint64_t start;
    uint64_t end;
    uint8_t trace_id[TRACE_ID_BYTES];
    uint8_t span_id[SPAN_ID_BYTES];
    uint8_t parent_span_id[SPAN_ID_BYTES];
    uint32_t head; /* first chunk, in emit order */
    uint32_t tail;
    uint16_t gen;
    uint16_t next_ended; /* next slot in end order */
    uint16_t attr_count;
    uint16_t event_count;
    uint8_t state;
    uint8_t kind;
    uint8_t status_code;
    uint8_t has_parent;
} span_rec;

typedef struct chunk_hdr
{
    uint32_t next;  /* next chunk of the owner's list */
    uint32_t size;  /* bytes including this header */
    uint32_t used;  /* payload bytes in use */
    uint32_t fwd;   /* compaction: new offset */
    uint32_t below; /* compaction: the chunk just below this one */
    uint16_t owner; /* slot index */
    uint8_t kind;
    uint8_t pad;
} chunk_hdr;

/* A packed attribute: this header, the key bytes, then the string bytes. */
typedef struct kv_hdr
{
    uint64_t scalar; /* int64, double bits, or bool */
    uint32_t key_len;
    uint32_t str_len;
    uint32_t type;
    uint32_t pad;
} kv_hdr;

/* An event chunk's payload: this header, the name, then packed attributes. */
typedef struct event_hdr
{
    uint64_t time;
    uint32_t name_len;
    uint32_t attr_count;
} event_hdr;

/* ------------------------------------------------------------------------ */
/* The leaf state, in the caller's microtel_leaf_t                          */
/* ------------------------------------------------------------------------ */

typedef struct leaf_state
{
    uint32_t magic;
    uint32_t time_mode;
    uint64_t (*now_ns)(void* ctx);
    void* clock_ctx;
    void (*random_bytes)(void* ctx, uint8_t* out, size_t len);
    void* random_ctx;
    void* scratch;
    size_t scratch_size;
    uint8_t* buf;
    uint32_t buf_size;
    uint32_t persist_off;
    uint32_t data_low;
    uint32_t resource_off;
    uint32_t resource_count;
    uint32_t scope_name_off;
    uint32_t scope_name_len;
    uint32_t scope_version_off;
    uint32_t scope_version_len;
    uint32_t boot_id;
    uint16_t slot_count;
    uint16_t next_gen;
    uint16_t ended_head;
    uint16_t ended_tail;
    uint32_t ended_count;
    uint16_t max_attrs;
    uint16_t max_events;
    uint16_t max_event_attrs;
    uint16_t synced;
    uint64_t device_key;
    uint64_t id_counter;
    uint64_t sync_unix;
    uint64_t sync_leaf;
    microtel_leaf_counters_t counters;
} leaf_state;

/* §1.4: the real state fits the caller's opaque storage, with its alignment. */
_Static_assert(sizeof(leaf_state) <= sizeof(microtel_leaf_t),
               "leaf_state outgrew MICROTEL_LEAF_STATE_WORDS");
_Static_assert(alignof(leaf_state) <= alignof(microtel_leaf_t),
               "leaf_state needs more alignment than microtel_leaf_t");
/* §1.4: the attribute layout is frozen for 1.x; its union holds a 64-bit
 * scalar or a pointer-and-length pair. */
_Static_assert(sizeof(((microtel_leaf_kv_t*)0)->value) >= sizeof(int64_t),
               "microtel_leaf_kv_t value union shrank");

static leaf_state* state_of(microtel_leaf_t* leaf)
{
    return (leaf_state*)leaf->microtel_private;
}

static const leaf_state* cstate_of(const microtel_leaf_t* leaf)
{
    return (const leaf_state*)leaf->microtel_private;
}

/* NULL leaf: ERR_ARG. Not initialised or freed: ERR_STATE. */
static microtel_leaf_status_t check_leaf(const microtel_leaf_t* leaf)
{
    if (leaf == NULL)
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    if (cstate_of(leaf)->magic != LEAF_MAGIC)
    {
        return MICROTEL_LEAF_ERR_STATE;
    }
    return MICROTEL_LEAF_OK;
}

/* ------------------------------------------------------------------------ */
/* Buffer access                                                            */
/* ------------------------------------------------------------------------ */

static uint32_t slot_off(uint16_t slot)
{
    return (uint32_t)slot * (uint32_t)sizeof(span_rec);
}

static span_rec get_slot(const leaf_state* st, uint16_t slot)
{
    span_rec rec;
    memcpy(&rec, st->buf + slot_off(slot), sizeof(rec));
    return rec;
}

static void put_slot(leaf_state* st, uint16_t slot, const span_rec* rec)
{
    memcpy(st->buf + slot_off(slot), rec, sizeof(*rec));
}

static chunk_hdr get_chunk(const leaf_state* st, uint32_t off)
{
    chunk_hdr hdr;
    memcpy(&hdr, st->buf + off, sizeof(hdr));
    return hdr;
}

static void put_chunk(leaf_state* st, uint32_t off, const chunk_hdr* hdr)
{
    memcpy(st->buf + off, hdr, sizeof(*hdr));
}

static uint8_t* chunk_payload(const leaf_state* st, uint32_t off)
{
    return st->buf + off + sizeof(chunk_hdr);
}

static uint32_t table_end(const leaf_state* st)
{
    return slot_off(st->slot_count);
}

/* ------------------------------------------------------------------------ */
/* Attributes: validation and packing                                       */
/* ------------------------------------------------------------------------ */

static int kv_valid(const microtel_leaf_kv_t* kv)
{
    if (kv->key == NULL || kv->key_len == 0u || kv->type > MICROTEL_LEAF_VALUE_STRING)
    {
        return 0;
    }
    return kv->type != MICROTEL_LEAF_VALUE_STRING || kv->value.s.ptr != NULL ||
           kv->value.s.len == 0u;
}

static int kvs_valid(const microtel_leaf_kv_t* kvs, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
    {
        if (!kv_valid(&kvs[i]))
        {
            return 0;
        }
    }
    return 1;
}

static size_t kv_str_len(const microtel_leaf_kv_t* kv)
{
    return kv->type == MICROTEL_LEAF_VALUE_STRING ? kv->value.s.len : 0u;
}

/* Packed size, in 64 bits so that no length can overflow it. */
static uint64_t kv_size(const microtel_leaf_kv_t* kv)
{
    return (uint64_t)sizeof(kv_hdr) + (uint64_t)kv->key_len + (uint64_t)kv_str_len(kv);
}

static uint64_t kv_scalar(const microtel_leaf_kv_t* kv)
{
    uint64_t bits = 0;
    switch (kv->type)
    {
        case MICROTEL_LEAF_VALUE_BOOL:
            bits = kv->value.b != 0 ? 1u : 0u;
            break;
        case MICROTEL_LEAF_VALUE_INT64:
            bits = (uint64_t)kv->value.i;
            break;
        case MICROTEL_LEAF_VALUE_DOUBLE:
            memcpy(&bits, &kv->value.d, sizeof(bits));
            break;
        default:
            break;
    }
    return bits;
}

/* Writes a packed attribute at `dst`; returns the bytes written. */
static uint32_t pack_kv(uint8_t* dst, const microtel_leaf_kv_t* kv)
{
    kv_hdr hdr;
    const size_t str_len = kv_str_len(kv);
    memset(&hdr, 0, sizeof(hdr));
    hdr.scalar = kv_scalar(kv);
    hdr.key_len = (uint32_t)kv->key_len;
    hdr.str_len = (uint32_t)str_len;
    hdr.type = (uint32_t)kv->type;
    memcpy(dst, &hdr, sizeof(hdr));
    memcpy(dst + sizeof(hdr), kv->key, kv->key_len);
    if (str_len > 0u)
    {
        memcpy(dst + sizeof(hdr) + kv->key_len, kv->value.s.ptr, str_len);
    }
    return (uint32_t)kv_size(kv);
}

/* Reads a packed attribute at `src` into a view; returns its size. */
static uint32_t unpack_kv(const uint8_t* src, microtel_leaf_kv_t* out)
{
    kv_hdr hdr;
    memcpy(&hdr, src, sizeof(hdr));
    memset(out, 0, sizeof(*out));
    out->key = (const char*)(src + sizeof(hdr));
    out->key_len = hdr.key_len;
    out->type = (microtel_leaf_value_type_t)hdr.type;
    switch (out->type)
    {
        case MICROTEL_LEAF_VALUE_BOOL:
            out->value.b = hdr.scalar != 0u ? 1 : 0;
            break;
        case MICROTEL_LEAF_VALUE_INT64:
            out->value.i = (int64_t)hdr.scalar;
            break;
        case MICROTEL_LEAF_VALUE_DOUBLE:
            memcpy(&out->value.d, &hdr.scalar, sizeof(out->value.d));
            break;
        default:
            out->value.s.ptr = out->key + hdr.key_len;
            out->value.s.len = hdr.str_len;
            break;
    }
    return (uint32_t)sizeof(hdr) + hdr.key_len + hdr.str_len;
}

static int has_reserved_prefix(const microtel_leaf_kv_t* kv)
{
    const size_t n = sizeof(k_reserved_prefix) - 1u;
    return kv->key_len >= n && memcmp(kv->key, k_reserved_prefix, n) == 0;
}

/* ------------------------------------------------------------------------ */
/* Ids (§1.6.1)                                                             */
/* ------------------------------------------------------------------------ */

static uint64_t fnv1a(uint64_t h, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    for (i = 0; i < len; ++i)
    {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t splitmix64(uint64_t x)
{
    uint64_t z = x + SPLITMIX_GAMMA;
    z = (z ^ (z >> SPLITMIX_SHIFT1)) * SPLITMIX_MUL1;
    z = (z ^ (z >> SPLITMIX_SHIFT2)) * SPLITMIX_MUL2;
    return z ^ (z >> SPLITMIX_SHIFT3);
}

static void store_le(uint8_t* out, uint64_t v, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
    {
        out[i] = (uint8_t)((v >> (BITS_PER_BYTE * i)) & BYTE_MASK);
    }
}

static uint64_t load_le64(const uint8_t* in)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < WORD_BYTES; ++i)
    {
        v |= (uint64_t)in[i] << (BITS_PER_BYTE * i);
    }
    return v;
}

static void store_be64(uint8_t* out, uint64_t v)
{
    size_t i;
    for (i = 0; i < WORD_BYTES; ++i)
    {
        out[i] = (uint8_t)((v >> (BITS_PER_BYTE * (WORD_BYTES - 1u - i))) & BYTE_MASK);
    }
}

/* FNV-1a over each attribute's key, type and value bytes, then boot_id, finished
 * with a splitmix64 mix. Not cryptographic: it only has to separate devices. */
static uint64_t device_key(const microtel_leaf_kv_t* resource, size_t count, uint32_t boot_id)
{
    uint64_t h = FNV_OFFSET;
    uint8_t scalar[WORD_BYTES];
    uint8_t boot[BOOT_ID_BYTES];
    size_t i;
    for (i = 0; i < count; ++i)
    {
        const microtel_leaf_kv_t* kv = &resource[i];
        const uint8_t type = (uint8_t)kv->type;
        h = fnv1a(h, kv->key, kv->key_len);
        h = fnv1a(h, &type, 1u);
        if (kv->type == MICROTEL_LEAF_VALUE_STRING)
        {
            h = fnv1a(h, kv->value.s.ptr, kv->value.s.len);
            continue;
        }
        store_le(scalar, kv_scalar(kv), WORD_BYTES);
        h = fnv1a(h, scalar, kv->type == MICROTEL_LEAF_VALUE_BOOL ? 1u : WORD_BYTES);
    }
    store_le(boot, boot_id, BOOT_ID_BYTES);
    h = fnv1a(h, boot, BOOT_ID_BYTES);
    return splitmix64(h);
}

/* id_word = splitmix64(device_key + counter++) XOR random_word. */
static uint64_t next_id_word(leaf_state* st)
{
    uint8_t r[WORD_BYTES];
    memset(r, 0, sizeof(r));
    st->random_bytes(st->random_ctx, r, sizeof(r));
    return splitmix64(st->device_key + st->id_counter++) ^ load_le64(r);
}

static void new_trace_id(leaf_state* st, uint8_t* out)
{
    uint64_t hi = 0;
    uint64_t lo = 0;
    while (hi == 0u && lo == 0u)
    {
        hi = next_id_word(st);
        lo = next_id_word(st);
    }
    store_be64(out, hi);
    store_be64(out + WORD_BYTES, lo);
}

static void new_span_id(leaf_state* st, uint8_t* out)
{
    uint64_t w = 0;
    while (w == 0u)
    {
        w = next_id_word(st);
    }
    store_be64(out, w);
}

static int all_zero(const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
    {
        if (p[i] != 0u)
        {
            return 0;
        }
    }
    return 1;
}

static uint64_t read_clock(const leaf_state* st)
{
    return st->now_ns != NULL ? st->now_ns(st->clock_ctx) : 0u;
}

/* ------------------------------------------------------------------------ */
/* Compaction                                                               */
/* ------------------------------------------------------------------------ */

static int chunk_live(const leaf_state* st, const chunk_hdr* hdr)
{
    if (hdr->kind == CHUNK_DEAD || hdr->owner >= st->slot_count)
    {
        return 0;
    }
    return get_slot(st, hdr->owner).state != SLOT_FREE;
}

/* Pass 1: forwarding offsets that pack live chunks against the Resource,
 * in their current order, and `below` links for the top-down move. Returns
 * the offset of the topmost chunk, or NONE32. */
static uint32_t compact_plan(leaf_state* st, uint32_t* new_low)
{
    uint32_t live = 0;
    uint32_t off = st->data_low;
    uint32_t below = NONE32;
    uint32_t pos;
    while (off < st->persist_off)
    {
        const chunk_hdr hdr = get_chunk(st, off);
        live += chunk_live(st, &hdr) ? hdr.size : 0u;
        off += hdr.size;
    }
    *new_low = st->persist_off - live;
    pos = *new_low;
    off = st->data_low;
    while (off < st->persist_off)
    {
        chunk_hdr hdr = get_chunk(st, off);
        hdr.below = below;
        hdr.fwd = NONE32;
        if (chunk_live(st, &hdr))
        {
            hdr.fwd = pos;
            pos += hdr.size;
        }
        put_chunk(st, off, &hdr);
        below = off;
        off += hdr.size;
    }
    return below;
}

static uint32_t forward(const leaf_state* st, uint32_t off)
{
    return off == NONE32 ? NONE32 : get_chunk(st, off).fwd;
}

/* Pass 2: rewrite every reference to its forwarding offset. */
static void compact_fix(leaf_state* st)
{
    uint32_t off = st->data_low;
    uint16_t slot;
    for (slot = 0; slot < st->slot_count; ++slot)
    {
        span_rec rec = get_slot(st, slot);
        if (rec.state != SLOT_FREE)
        {
            rec.head = forward(st, rec.head);
            rec.tail = forward(st, rec.tail);
            put_slot(st, slot, &rec);
        }
    }
    while (off < st->persist_off)
    {
        chunk_hdr hdr = get_chunk(st, off);
        if (hdr.fwd != NONE32)
        {
            hdr.next = forward(st, hdr.next);
            put_chunk(st, off, &hdr);
        }
        off += hdr.size;
    }
}

/* Pass 3: slide live chunks up, topmost first, so no move overwrites a chunk
 * that has not moved yet. */
static void compact_move(leaf_state* st, uint32_t top)
{
    uint32_t off = top;
    while (off != NONE32)
    {
        const chunk_hdr hdr = get_chunk(st, off);
        if (hdr.fwd != NONE32 && hdr.fwd != off)
        {
            memmove(st->buf + hdr.fwd, st->buf + off, hdr.size);
        }
        off = hdr.below;
    }
}

static void compact(leaf_state* st)
{
    uint32_t new_low = st->persist_off;
    const uint32_t top = compact_plan(st, &new_low);
    compact_fix(st);
    compact_move(st, top);
    st->data_low = new_low;
}

/* ------------------------------------------------------------------------ */
/* Allocation                                                               */
/* ------------------------------------------------------------------------ */

static int fits_below(const leaf_state* st, uint64_t size)
{
    return (uint64_t)table_end(st) + size <= (uint64_t)st->data_low;
}

/* Allocates a chunk with `payload` bytes for `owner`, compacting once if it
 * does not fit. Returns its offset, or NONE32. The chunk is not linked. */
static uint32_t alloc_chunk(leaf_state* st, uint16_t owner, uint8_t kind, uint64_t payload)
{
    const uint64_t size = (uint64_t)sizeof(chunk_hdr) + payload;
    chunk_hdr hdr;
    if (!fits_below(st, size))
    {
        compact(st);
        if (!fits_below(st, size))
        {
            return NONE32;
        }
    }
    st->data_low -= (uint32_t)size;
    memset(&hdr, 0, sizeof(hdr));
    hdr.next = NONE32;
    hdr.size = (uint32_t)size;
    hdr.used = (uint32_t)payload;
    hdr.owner = owner;
    hdr.kind = kind;
    put_chunk(st, st->data_low, &hdr);
    return st->data_low;
}

/* Appends chunk `off` to the end of `slot`'s list. */
static void link_chunk(leaf_state* st, uint16_t slot, uint32_t off)
{
    span_rec rec = get_slot(st, slot);
    if (rec.head == NONE32)
    {
        rec.head = off;
    }
    else
    {
        chunk_hdr tail = get_chunk(st, rec.tail);
        tail.next = off;
        put_chunk(st, rec.tail, &tail);
    }
    rec.tail = off;
    put_slot(st, slot, &rec);
}

static void kill_chunk(leaf_state* st, uint32_t off)
{
    chunk_hdr hdr = get_chunk(st, off);
    hdr.kind = CHUNK_DEAD;
    put_chunk(st, off, &hdr);
}

/* A chunk with a string payload: a name or a status message. */
static uint32_t alloc_string(leaf_state* st, uint16_t owner, uint8_t kind, const char* s, size_t n)
{
    const uint32_t off = alloc_chunk(st, owner, kind, n);
    if (off != NONE32 && n > 0u)
    {
        memcpy(chunk_payload(st, off), s, n);
    }
    return off;
}

/* ------------------------------------------------------------------------ */
/* Handles and lists                                                        */
/* ------------------------------------------------------------------------ */

/* Resolves a handle to an open or ended span. Returns 0 if it is stale. */
static int resolve(const leaf_state* st, microtel_leaf_span_t handle, uint16_t* slot)
{
    const uint16_t index = (uint16_t)(handle & SLOT_MASK);
    const uint16_t gen = (uint16_t)(handle >> GEN_SHIFT);
    span_rec rec;
    if (index >= st->slot_count || gen == 0u)
    {
        return 0;
    }
    rec = get_slot(st, index);
    if (rec.state == SLOT_FREE || rec.gen != gen)
    {
        return 0;
    }
    *slot = index;
    return 1;
}

/* Resolves a handle to an open span, the only kind a mutation accepts. */
static int resolve_open(const leaf_state* st, microtel_leaf_span_t handle, uint16_t* slot)
{
    return resolve(st, handle, slot) && get_slot(st, *slot).state == SLOT_OPEN;
}

/* Whether chunk `off` is of `kind` and, when `key` is given, an attribute
 * with that key. */
static int chunk_matches(const leaf_state* st,
                         uint32_t off,
                         uint8_t kind,
                         const microtel_leaf_kv_t* key)
{
    microtel_leaf_kv_t found;
    if (get_chunk(st, off).kind != kind)
    {
        return 0;
    }
    if (key == NULL)
    {
        return 1;
    }
    (void)unpack_kv(chunk_payload(st, off), &found);
    return found.key_len == key->key_len && memcmp(found.key, key->key, key->key_len) == 0;
}

/* Finds the first chunk of `kind` in `slot`'s list (and, for attributes, with
 * `key`). Returns its offset or NONE32; `*prev` is the chunk before it. */
static uint32_t find_chunk(const leaf_state* st,
                           uint16_t slot,
                           uint8_t kind,
                           const microtel_leaf_kv_t* key,
                           uint32_t* prev)
{
    uint32_t off = get_slot(st, slot).head;
    *prev = NONE32;
    while (off != NONE32 && !chunk_matches(st, off, kind, key))
    {
        *prev = off;
        off = get_chunk(st, off).next;
    }
    return off;
}

/* Puts chunk `repl` where `old` is in `slot`'s list, and kills `old`. */
static void splice_chunk(leaf_state* st, uint16_t slot, uint32_t prev, uint32_t old, uint32_t repl)
{
    span_rec rec = get_slot(st, slot);
    chunk_hdr hdr = get_chunk(st, repl);
    hdr.next = get_chunk(st, old).next;
    put_chunk(st, repl, &hdr);
    if (prev == NONE32)
    {
        rec.head = repl;
    }
    else
    {
        chunk_hdr p = get_chunk(st, prev);
        p.next = repl;
        put_chunk(st, prev, &p);
    }
    if (rec.tail == old)
    {
        rec.tail = repl;
    }
    put_slot(st, slot, &rec);
    kill_chunk(st, old);
}

/* Removes chunk `off` (preceded by `prev`) from `slot`'s list, and kills it. */
static void unlink_chunk(leaf_state* st, uint16_t slot, uint32_t prev, uint32_t off)
{
    span_rec rec = get_slot(st, slot);
    const uint32_t next = get_chunk(st, off).next;
    if (prev == NONE32)
    {
        rec.head = next;
    }
    else
    {
        chunk_hdr p = get_chunk(st, prev);
        p.next = next;
        put_chunk(st, prev, &p);
    }
    if (rec.tail == off)
    {
        rec.tail = prev;
    }
    put_slot(st, slot, &rec);
    kill_chunk(st, off);
}

static uint16_t next_generation(leaf_state* st)
{
    const uint16_t gen = st->next_gen;
    st->next_gen = (uint16_t)(gen == NONE16 ? 1u : gen + 1u);
    return gen;
}

/* A free slot, growing the table if needed. Returns NONE16 if there is no room. */
static uint16_t acquire_slot(leaf_state* st)
{
    uint16_t slot;
    span_rec rec;
    for (slot = 0; slot < st->slot_count; ++slot)
    {
        if (get_slot(st, slot).state == SLOT_FREE)
        {
            return slot;
        }
    }
    if (st->slot_count == NONE16)
    {
        return NONE16;
    }
    if (!fits_below(st, sizeof(span_rec)))
    {
        compact(st);
        if (!fits_below(st, sizeof(span_rec)))
        {
            return NONE16;
        }
    }
    slot = st->slot_count++;
    memset(&rec, 0, sizeof(rec));
    rec.state = SLOT_FREE;
    put_slot(st, slot, &rec);
    return slot;
}

/* Drops trailing free slots so the table gives their space back. */
static void trim_slots(leaf_state* st)
{
    while (st->slot_count > 0u && get_slot(st, (uint16_t)(st->slot_count - 1u)).state == SLOT_FREE)
    {
        --st->slot_count;
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

uint32_t microtel_leaf_version(void)
{
    return MICROTEL_LEAF_VERSION;
}

static int config_scalars_valid(const microtel_leaf_config_t* c)
{
    if (c->random_bytes == NULL || c->time_mode > MICROTEL_LEAF_TIME_BOOT_RELATIVE)
    {
        return 0;
    }
    if (c->now_ns == NULL && c->time_mode != MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED)
    {
        return 0;
    }
    return (c->scratch == NULL) == (c->scratch_size == 0u);
}

static int config_valid(const microtel_leaf_config_t* c)
{
    size_t i;
    if (!config_scalars_valid(c) || (c->resource == NULL && c->resource_count > 0u))
    {
        return 0;
    }
    if ((c->scope_name == NULL && c->scope_name_len > 0u) ||
        (c->scope_version == NULL && c->scope_version_len > 0u))
    {
        return 0;
    }
    for (i = 0; i < c->resource_count; ++i)
    {
        if (!kv_valid(&c->resource[i]) || has_reserved_prefix(&c->resource[i]))
        {
            return 0;
        }
    }
    return 1;
}

static uint64_t persistent_size(const microtel_leaf_config_t* c)
{
    uint64_t total = (uint64_t)c->scope_name_len + (uint64_t)c->scope_version_len;
    size_t i;
    for (i = 0; i < c->resource_count; ++i)
    {
        total += kv_size(&c->resource[i]);
    }
    return total;
}

static uint16_t cap_or(uint16_t value, uint16_t fallback)
{
    return value != 0u ? value : fallback;
}

/* Writes the Resource and scope at the top of the buffer. */
static void write_persistent(leaf_state* st, const microtel_leaf_config_t* c, uint32_t size)
{
    uint32_t off = st->buf_size - size;
    size_t i;
    st->persist_off = off;
    st->data_low = off;
    st->resource_off = off;
    st->resource_count = (uint32_t)c->resource_count;
    for (i = 0; i < c->resource_count; ++i)
    {
        off += pack_kv(st->buf + off, &c->resource[i]);
    }
    st->scope_name_off = off;
    st->scope_name_len = (uint32_t)c->scope_name_len;
    if (c->scope_name_len > 0u)
    {
        memcpy(st->buf + off, c->scope_name, c->scope_name_len);
    }
    off += st->scope_name_len;
    st->scope_version_off = off;
    st->scope_version_len = (uint32_t)c->scope_version_len;
    if (c->scope_version_len > 0u)
    {
        memcpy(st->buf + off, c->scope_version, c->scope_version_len);
    }
}

static void init_state(leaf_state* st, const microtel_leaf_config_t* c, void* buf, uint32_t size)
{
    memset(st, 0, sizeof(*st));
    st->time_mode = (uint32_t)c->time_mode;
    st->now_ns = c->now_ns;
    st->clock_ctx = c->clock_ctx;
    st->random_bytes = c->random_bytes;
    st->random_ctx = c->random_ctx;
    st->scratch = c->scratch;
    st->scratch_size = c->scratch_size;
    st->buf = (uint8_t*)buf;
    st->buf_size = size;
    st->boot_id = c->boot_id;
    st->next_gen = 1u;
    st->ended_head = NONE16;
    st->ended_tail = NONE16;
    st->max_attrs = cap_or(c->max_attributes_per_span, DEFAULT_MAX_ATTRS);
    st->max_events = cap_or(c->max_events_per_span, DEFAULT_MAX_EVENTS);
    st->max_event_attrs = cap_or(c->max_attributes_per_event, DEFAULT_MAX_EVENT_ATTRS);
    st->device_key = device_key(c->resource, c->resource_count, c->boot_id);
}

microtel_leaf_status_t microtel_leaf_init(microtel_leaf_t* leaf,
                                          size_t leaf_size,
                                          const microtel_leaf_config_t* config,
                                          void* record_buffer,
                                          size_t record_buffer_size)
{
    microtel_leaf_config_t c;
    uint32_t usable;
    uint64_t persistent;
    if (leaf == NULL || leaf_size < sizeof(microtel_leaf_t) || config == NULL ||
        record_buffer == NULL || config->struct_size < sizeof(microtel_leaf_config_t))
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    memcpy(&c, config, sizeof(c));
    if (!config_valid(&c))
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    usable = record_buffer_size > MAX_BUFFER ? MAX_BUFFER : (uint32_t)record_buffer_size;
    persistent = persistent_size(&c);
    if (persistent > usable)
    {
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    init_state(state_of(leaf), &c, record_buffer, usable);
    write_persistent(state_of(leaf), &c, (uint32_t)persistent);
    state_of(leaf)->magic = LEAF_MAGIC;
    return MICROTEL_LEAF_OK;
}

void microtel_leaf_free(microtel_leaf_t* leaf)
{
    if (leaf != NULL)
    {
        memset(leaf, 0, sizeof(*leaf));
    }
}

/* ------------------------------------------------------------------------ */
/* Building spans                                                           */
/* ------------------------------------------------------------------------ */

typedef struct span_ids
{
    const uint8_t* trace_id;  /* NULL: generate */
    const uint8_t* parent_id; /* NULL: root */
} span_ids;

static microtel_leaf_status_t start_span(leaf_state* st,
                                         microtel_leaf_span_t* out_span,
                                         const char* name,
                                         size_t name_len,
                                         microtel_leaf_span_kind_t kind,
                                         const span_ids* ids)
{
    span_rec rec;
    const uint16_t slot = acquire_slot(st);
    uint32_t name_off = NONE32;
    if (slot != NONE16 && name_len > 0u)
    {
        name_off = alloc_string(st, slot, CHUNK_NAME, name, name_len);
    }
    if (slot == NONE16 || (name_len > 0u && name_off == NONE32))
    {
        trim_slots(st);
        ++st->counters.dropped_spans;
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    memset(&rec, 0, sizeof(rec));
    if (ids->trace_id != NULL)
    {
        memcpy(rec.trace_id, ids->trace_id, TRACE_ID_BYTES);
    }
    else
    {
        new_trace_id(st, rec.trace_id);
    }
    new_span_id(st, rec.span_id);
    rec.has_parent = ids->parent_id != NULL ? 1u : 0u;
    if (ids->parent_id != NULL)
    {
        memcpy(rec.parent_span_id, ids->parent_id, SPAN_ID_BYTES);
    }
    rec.head = NONE32;
    rec.tail = NONE32;
    rec.gen = next_generation(st);
    rec.next_ended = NONE16;
    rec.state = SLOT_OPEN;
    rec.kind = (uint8_t)kind;
    rec.start = read_clock(st);
    put_slot(st, slot, &rec);
    if (name_off != NONE32)
    {
        link_chunk(st, slot, name_off);
    }
    *out_span = ((uint32_t)rec.gen << GEN_SHIFT) | slot;
    return MICROTEL_LEAF_OK;
}

static int start_args_valid(const microtel_leaf_span_t* out_span,
                            const char* name,
                            size_t name_len,
                            microtel_leaf_span_kind_t kind)
{
    return out_span != NULL && (name != NULL || name_len == 0u) &&
           kind >= MICROTEL_LEAF_SPAN_KIND_INTERNAL && kind <= MICROTEL_LEAF_SPAN_KIND_CONSUMER;
}

microtel_leaf_status_t microtel_leaf_span_start(microtel_leaf_t* leaf,
                                                microtel_leaf_span_t* out_span,
                                                const char* name,
                                                size_t name_len,
                                                microtel_leaf_span_kind_t kind,
                                                const microtel_leaf_span_t* parent)
{
    leaf_state* st;
    span_rec parent_rec;
    span_ids ids = {NULL, NULL};
    uint16_t parent_slot = 0;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    if (!start_args_valid(out_span, name, name_len, kind))
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    st = state_of(leaf);
    if (parent != NULL)
    {
        if (!resolve(st, *parent, &parent_slot))
        {
            return MICROTEL_LEAF_ERR_STATE;
        }
        parent_rec = get_slot(st, parent_slot);
        ids.trace_id = parent_rec.trace_id;
        ids.parent_id = parent_rec.span_id;
    }
    return start_span(st, out_span, name, name_len, kind, &ids);
}

microtel_leaf_status_t microtel_leaf_span_start_remote(microtel_leaf_t* leaf,
                                                       microtel_leaf_span_t* out_span,
                                                       const char* name,
                                                       size_t name_len,
                                                       microtel_leaf_span_kind_t kind,
                                                       const uint8_t trace_id[16],
                                                       const uint8_t parent_span_id[8])
{
    span_ids ids;
    const microtel_leaf_status_t status = check_leaf(leaf);
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    if (!start_args_valid(out_span, name, name_len, kind) || trace_id == NULL ||
        parent_span_id == NULL || all_zero(trace_id, TRACE_ID_BYTES) ||
        all_zero(parent_span_id, SPAN_ID_BYTES))
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    ids.trace_id = trace_id;
    ids.parent_id = parent_span_id;
    return start_span(state_of(leaf), out_span, name, name_len, kind, &ids);
}

/* Common prologue of the span mutators: a live leaf and an open span. */
static microtel_leaf_status_t open_span(microtel_leaf_t* leaf,
                                        microtel_leaf_span_t span,
                                        uint16_t* slot)
{
    const microtel_leaf_status_t status = check_leaf(leaf);
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    return resolve_open(state_of(leaf), span, slot) ? MICROTEL_LEAF_OK : MICROTEL_LEAF_ERR_STATE;
}

/* Replaces the value of attribute chunk `old`, in place if it fits. */
static microtel_leaf_status_t overwrite_attr(leaf_state* st,
                                             uint16_t slot,
                                             uint32_t old,
                                             const microtel_leaf_kv_t* attr)
{
    const uint64_t size = kv_size(attr);
    chunk_hdr hdr = get_chunk(st, old);
    uint32_t repl;
    uint32_t prev;
    if (size <= (uint64_t)hdr.size - sizeof(chunk_hdr))
    {
        hdr.used = pack_kv(chunk_payload(st, old), attr);
        put_chunk(st, old, &hdr);
        return MICROTEL_LEAF_OK;
    }
    repl = alloc_chunk(st, slot, CHUNK_ATTR, size);
    if (repl == NONE32)
    {
        ++st->counters.dropped_attributes;
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    (void)pack_kv(chunk_payload(st, repl), attr);
    /* The allocation may have compacted: look the old chunk up again. */
    old = find_chunk(st, slot, CHUNK_ATTR, attr, &prev);
    splice_chunk(st, slot, prev, old, repl);
    return MICROTEL_LEAF_OK;
}

static microtel_leaf_status_t add_attr(leaf_state* st,
                                       uint16_t slot,
                                       const microtel_leaf_kv_t* attr)
{
    span_rec rec = get_slot(st, slot);
    uint32_t off;
    if (rec.attr_count >= st->max_attrs)
    {
        ++st->counters.dropped_attributes;
        return MICROTEL_LEAF_ERR_LIMIT;
    }
    off = alloc_chunk(st, slot, CHUNK_ATTR, kv_size(attr));
    if (off == NONE32)
    {
        ++st->counters.dropped_attributes;
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    (void)pack_kv(chunk_payload(st, off), attr);
    link_chunk(st, slot, off);
    rec = get_slot(st, slot);
    ++rec.attr_count;
    put_slot(st, slot, &rec);
    return MICROTEL_LEAF_OK;
}

microtel_leaf_status_t microtel_leaf_span_set_attribute(microtel_leaf_t* leaf,
                                                        microtel_leaf_span_t span,
                                                        const microtel_leaf_kv_t* attr)
{
    uint16_t slot = 0;
    uint32_t prev;
    uint32_t existing;
    leaf_state* st;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status == MICROTEL_LEAF_OK && (attr == NULL || !kv_valid(attr)))
    {
        status = MICROTEL_LEAF_ERR_ARG;
    }
    if (status == MICROTEL_LEAF_OK)
    {
        status = open_span(leaf, span, &slot);
    }
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    st = state_of(leaf);
    existing = find_chunk(st, slot, CHUNK_ATTR, attr, &prev);
    if (existing != NONE32)
    {
        return overwrite_attr(st, slot, existing, attr);
    }
    return add_attr(st, slot, attr);
}

/* Writes an event's payload into chunk `off`. */
static void write_event(leaf_state* st,
                        uint32_t off,
                        const char* name,
                        size_t name_len,
                        const microtel_leaf_kv_t* attrs,
                        size_t kept)
{
    uint8_t* p = chunk_payload(st, off);
    event_hdr hdr;
    size_t i;
    hdr.time = read_clock(st);
    hdr.name_len = (uint32_t)name_len;
    hdr.attr_count = (uint32_t)kept;
    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);
    if (name_len > 0u)
    {
        memcpy(p, name, name_len);
    }
    p += name_len;
    for (i = 0; i < kept; ++i)
    {
        p += pack_kv(p, &attrs[i]);
    }
}

microtel_leaf_status_t microtel_leaf_span_add_event(microtel_leaf_t* leaf,
                                                    microtel_leaf_span_t span,
                                                    const char* name,
                                                    size_t name_len,
                                                    const microtel_leaf_kv_t* attrs,
                                                    size_t attr_count)
{
    uint16_t slot = 0;
    leaf_state* st;
    span_rec rec;
    size_t kept;
    size_t i;
    uint64_t size;
    uint32_t off;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status == MICROTEL_LEAF_OK &&
        ((name == NULL && name_len > 0u) || (attrs == NULL && attr_count > 0u) ||
         (attrs != NULL && !kvs_valid(attrs, attr_count))))
    {
        status = MICROTEL_LEAF_ERR_ARG;
    }
    if (status == MICROTEL_LEAF_OK)
    {
        status = open_span(leaf, span, &slot);
    }
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    st = state_of(leaf);
    rec = get_slot(st, slot);
    if (rec.event_count >= st->max_events)
    {
        ++st->counters.dropped_events;
        return MICROTEL_LEAF_ERR_LIMIT;
    }
    kept = attr_count < st->max_event_attrs ? attr_count : st->max_event_attrs;
    size = (uint64_t)sizeof(event_hdr) + (uint64_t)name_len;
    for (i = 0; i < kept; ++i)
    {
        size += kv_size(&attrs[i]);
    }
    off = alloc_chunk(st, slot, CHUNK_EVENT, size);
    if (off == NONE32)
    {
        ++st->counters.dropped_events;
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    write_event(st, off, name, name_len, attrs, kept);
    link_chunk(st, slot, off);
    rec = get_slot(st, slot);
    ++rec.event_count;
    put_slot(st, slot, &rec);
    st->counters.dropped_attributes += (uint32_t)(attr_count - kept);
    return MICROTEL_LEAF_OK;
}

/* Replaces (or, with an empty message, removes) the status message chunk. */
static microtel_leaf_status_t set_status_message(leaf_state* st,
                                                 uint16_t slot,
                                                 const char* message,
                                                 size_t message_len)
{
    uint32_t prev;
    uint32_t old = find_chunk(st, slot, CHUNK_STATUS, NULL, &prev);
    uint32_t repl;
    if (old != NONE32)
    {
        chunk_hdr hdr = get_chunk(st, old);
        if (message_len > 0u && (uint64_t)message_len <= (uint64_t)hdr.size - sizeof(chunk_hdr))
        {
            memcpy(chunk_payload(st, old), message, message_len);
            hdr.used = (uint32_t)message_len;
            put_chunk(st, old, &hdr);
            return MICROTEL_LEAF_OK;
        }
    }
    if (message_len == 0u)
    {
        if (old != NONE32)
        {
            unlink_chunk(st, slot, prev, old);
        }
        return MICROTEL_LEAF_OK;
    }
    repl = alloc_string(st, slot, CHUNK_STATUS, message, message_len);
    if (repl == NONE32)
    {
        return MICROTEL_LEAF_ERR_NO_SPACE;
    }
    /* The allocation may have compacted: look the old chunk up again. */
    old = find_chunk(st, slot, CHUNK_STATUS, NULL, &prev);
    if (old != NONE32)
    {
        unlink_chunk(st, slot, prev, old);
    }
    link_chunk(st, slot, repl);
    return MICROTEL_LEAF_OK;
}

microtel_leaf_status_t microtel_leaf_span_set_status(microtel_leaf_t* leaf,
                                                     microtel_leaf_span_t span,
                                                     microtel_leaf_status_code_t code,
                                                     const char* message,
                                                     size_t message_len)
{
    uint16_t slot = 0;
    leaf_state* st;
    span_rec rec;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status == MICROTEL_LEAF_OK &&
        (code > MICROTEL_LEAF_STATUS_ERROR || (message == NULL && message_len > 0u)))
    {
        status = MICROTEL_LEAF_ERR_ARG;
    }
    if (status == MICROTEL_LEAF_OK)
    {
        status = open_span(leaf, span, &slot);
    }
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    st = state_of(leaf);
    status = set_status_message(st, slot, message, message_len);
    if (status == MICROTEL_LEAF_OK)
    {
        rec = get_slot(st, slot);
        rec.status_code = (uint8_t)code;
        put_slot(st, slot, &rec);
    }
    return status;
}

microtel_leaf_status_t microtel_leaf_span_end(microtel_leaf_t* leaf, microtel_leaf_span_t span)
{
    uint16_t slot = 0;
    leaf_state* st;
    span_rec rec;
    const microtel_leaf_status_t status = open_span(leaf, span, &slot);
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    st = state_of(leaf);
    rec = get_slot(st, slot);
    rec.state = SLOT_ENDED;
    rec.end = read_clock(st);
    rec.next_ended = NONE16;
    put_slot(st, slot, &rec);
    if (st->ended_tail == NONE16)
    {
        st->ended_head = slot;
    }
    else
    {
        span_rec tail = get_slot(st, st->ended_tail);
        tail.next_ended = slot;
        put_slot(st, st->ended_tail, &tail);
    }
    st->ended_tail = slot;
    ++st->ended_count;
    return MICROTEL_LEAF_OK;
}

microtel_leaf_status_t microtel_leaf_clock_sync(microtel_leaf_t* leaf,
                                                uint64_t unix_ns,
                                                uint64_t leaf_now_ns)
{
    leaf_state* st;
    const microtel_leaf_status_t status = check_leaf(leaf);
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    st = state_of(leaf);
    if (st->now_ns == NULL || unix_ns == 0u || leaf_now_ns > read_clock(st))
    {
        return MICROTEL_LEAF_ERR_CLOCK;
    }
    st->sync_unix = unix_ns;
    st->sync_leaf = leaf_now_ns;
    st->synced = 1u;
    return MICROTEL_LEAF_OK;
}

void microtel_leaf_get_counters(const microtel_leaf_t* leaf,
                                microtel_leaf_counters_t* out,
                                size_t out_size)
{
    microtel_leaf_counters_t c;
    const size_t n = out_size < sizeof(c) ? out_size : sizeof(c);
    if (leaf == NULL || out == NULL)
    {
        return;
    }
    memset(&c, 0, sizeof(c));
    if (check_leaf(leaf) == MICROTEL_LEAF_OK)
    {
        c = cstate_of(leaf)->counters;
    }
    memcpy(out, &c, n);
}

/* ------------------------------------------------------------------------ */
/* The batch view (§2.2)                                                    */
/* ------------------------------------------------------------------------ */

static const leaf_state* batch_state(const microtel_leaf_internal_batch* batch)
{
    return (const leaf_state*)batch->core;
}

static const char k_key_proto[] = "microtel.leaf.proto";
static const char k_key_time_mode[] = "microtel.leaf.time_mode";
static const char k_key_encode_time[] = "microtel.leaf.encode_time";
static const char k_key_sync_age[] = "microtel.leaf.sync_age";
static const char k_key_boot_id[] = "microtel.leaf.boot_id";
static const char k_key_dropped_spans[] = "microtel.leaf.dropped_spans";
static const char k_key_dropped_items[] = "microtel.leaf.dropped_items";

static void add_reserved(microtel_leaf_internal_batch* b,
                         const char* key,
                         size_t key_len,
                         int64_t v)
{
    microtel_leaf_kv_t* kv = &b->reserved[b->reserved_count++];
    memset(kv, 0, sizeof(*kv));
    kv->key = key;
    kv->key_len = key_len;
    kv->type = MICROTEL_LEAF_VALUE_INT64;
    kv->value.i = v;
}

#define ADD_RESERVED(batch, key, value) add_reserved((batch), (key), sizeof(key) - 1u, (value))

/* Decides the payload's time mode, conversion and reserved attributes (§3.8,
 * §5). Reads the clock once, for the encode time. */
static void build_batch(const leaf_state* st, microtel_leaf_internal_batch* b)
{
    const int has_clock = st->now_ns != NULL;
    const uint64_t e = read_clock(st);
    uint32_t mode = st->time_mode;
    const uint64_t dropped_items =
        (uint64_t)st->counters.dropped_attributes + (uint64_t)st->counters.dropped_events;
    memset(b, 0, sizeof(*b));
    b->core = st;
    b->scope_name = (const char*)st->buf + st->scope_name_off;
    b->scope_name_len = st->scope_name_len;
    b->scope_version = (const char*)st->buf + st->scope_version_off;
    b->scope_version_len = st->scope_version_len;
    b->span_count = st->ended_count;
    if (mode == MICROTEL_LEAF_TIME_SYNC_RELATIVE && st->synced == 0u)
    {
        mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    }
    if (mode == MICROTEL_LEAF_TIME_SYNC_RELATIVE)
    {
        b->time_offset = st->sync_unix - st->sync_leaf;
    }
    ADD_RESERVED(b, k_key_proto, WIRE_PROTO_VERSION);
    ADD_RESERVED(b, k_key_time_mode, (int64_t)mode);
    if (has_clock)
    {
        ADD_RESERVED(b, k_key_encode_time, (int64_t)(e + b->time_offset));
    }
    if (mode == MICROTEL_LEAF_TIME_SYNC_RELATIVE)
    {
        ADD_RESERVED(b, k_key_sync_age, (int64_t)(e - st->sync_leaf));
    }
    if (mode == MICROTEL_LEAF_TIME_BOOT_RELATIVE)
    {
        ADD_RESERVED(b, k_key_boot_id, (int64_t)st->boot_id);
    }
    if (st->counters.dropped_spans > 0u)
    {
        ADD_RESERVED(b, k_key_dropped_spans, (int64_t)st->counters.dropped_spans);
    }
    if (dropped_items > 0u)
    {
        ADD_RESERVED(b, k_key_dropped_items, (int64_t)dropped_items);
    }
    b->resource_count = st->resource_count + b->reserved_count;
}

int microtel_leaf_internal_next_resource_attr(const microtel_leaf_internal_batch* batch,
                                              microtel_leaf_internal_cursor* cursor,
                                              microtel_leaf_kv_t* out)
{
    const leaf_state* st = batch_state(batch);
    if (cursor->index == 0u)
    {
        cursor->pos = st->resource_off;
    }
    if (cursor->index < st->resource_count)
    {
        cursor->pos += unpack_kv(st->buf + cursor->pos, out);
        ++cursor->index;
        return 1;
    }
    if (cursor->index - st->resource_count < batch->reserved_count)
    {
        *out = batch->reserved[cursor->index - st->resource_count];
        ++cursor->index;
        return 1;
    }
    return 0;
}

/* Fills the name and status message of `out` from `slot`'s chunks. */
static void span_strings(const leaf_state* st, uint32_t head, microtel_leaf_internal_span* out)
{
    uint32_t off = head;
    while (off != NONE32)
    {
        const chunk_hdr hdr = get_chunk(st, off);
        if (hdr.kind == CHUNK_NAME)
        {
            out->name = (const char*)chunk_payload(st, off);
            out->name_len = hdr.used;
        }
        else if (hdr.kind == CHUNK_STATUS)
        {
            out->status_message = (const char*)chunk_payload(st, off);
            out->status_message_len = hdr.used;
        }
        off = hdr.next;
    }
}

int microtel_leaf_internal_next_span(const microtel_leaf_internal_batch* batch,
                                     microtel_leaf_internal_cursor* cursor,
                                     microtel_leaf_internal_span* out)
{
    const leaf_state* st = batch_state(batch);
    const uint8_t* base;
    span_rec rec;
    const uint16_t slot = cursor->index == 0u ? st->ended_head : (uint16_t)cursor->pos;
    if (slot == NONE16)
    {
        return 0;
    }
    rec = get_slot(st, slot);
    base = st->buf + slot_off(slot);
    memset(out, 0, sizeof(*out));
    out->trace_id = base + offsetof(span_rec, trace_id);
    out->span_id = base + offsetof(span_rec, span_id);
    out->parent_span_id = rec.has_parent != 0u ? base + offsetof(span_rec, parent_span_id) : NULL;
    out->kind = rec.kind;
    out->start_time_unix_nano = rec.start + batch->time_offset;
    out->end_time_unix_nano = rec.end + batch->time_offset;
    out->status_code = rec.status_code;
    out->attr_count = rec.attr_count;
    out->event_count = rec.event_count;
    out->chain = rec.head;
    span_strings(st, rec.head, out);
    out->has_status = rec.status_code != MICROTEL_LEAF_STATUS_UNSET || out->status_message_len > 0u;
    cursor->pos = rec.next_ended;
    ++cursor->index;
    return 1;
}

/* Advances `cursor` along a span's chunk list to the next chunk of `kind`. */
static uint32_t next_of_kind(const leaf_state* st,
                             const microtel_leaf_internal_span* span,
                             microtel_leaf_internal_cursor* cursor,
                             uint8_t kind)
{
    uint32_t off = cursor->index == 0u ? span->chain : cursor->pos;
    while (off != NONE32)
    {
        const chunk_hdr hdr = get_chunk(st, off);
        if (hdr.kind == kind)
        {
            cursor->pos = hdr.next;
            ++cursor->index;
            return off;
        }
        off = hdr.next;
    }
    cursor->pos = NONE32;
    ++cursor->index;
    return NONE32;
}

int microtel_leaf_internal_next_attr(const microtel_leaf_internal_batch* batch,
                                     const microtel_leaf_internal_span* span,
                                     microtel_leaf_internal_cursor* cursor,
                                     microtel_leaf_kv_t* out)
{
    const leaf_state* st = batch_state(batch);
    const uint32_t off = next_of_kind(st, span, cursor, CHUNK_ATTR);
    if (off == NONE32)
    {
        return 0;
    }
    (void)unpack_kv(chunk_payload(st, off), out);
    return 1;
}

int microtel_leaf_internal_next_event(const microtel_leaf_internal_batch* batch,
                                      const microtel_leaf_internal_span* span,
                                      microtel_leaf_internal_cursor* cursor,
                                      microtel_leaf_internal_event* out)
{
    const leaf_state* st = batch_state(batch);
    const uint32_t off = next_of_kind(st, span, cursor, CHUNK_EVENT);
    const uint8_t* p;
    event_hdr hdr;
    if (off == NONE32)
    {
        return 0;
    }
    p = chunk_payload(st, off);
    memcpy(&hdr, p, sizeof(hdr));
    memset(out, 0, sizeof(*out));
    out->time_unix_nano = hdr.time + batch->time_offset;
    out->name = (const char*)(p + sizeof(hdr));
    out->name_len = hdr.name_len;
    out->attr_count = hdr.attr_count;
    out->attrs = p + sizeof(hdr) + hdr.name_len;
    return 1;
}

int microtel_leaf_internal_next_event_attr(const microtel_leaf_internal_event* event,
                                           microtel_leaf_internal_cursor* cursor,
                                           microtel_leaf_kv_t* out)
{
    if (cursor->index >= event->attr_count)
    {
        return 0;
    }
    cursor->pos += unpack_kv(event->attrs + cursor->pos, out);
    ++cursor->index;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Encoding (§1.8)                                                          */
/* ------------------------------------------------------------------------ */

#ifndef MICROTEL_LEAF_BACKEND_ENCODE
#error "MICROTEL_LEAF_BACKEND_ENCODE must name the backend entry point (leaf/CMakeLists.txt)"
#endif

/* Releases every ended span after a successful encode. */
static void release_ended(leaf_state* st)
{
    uint16_t slot = st->ended_head;
    while (slot != NONE16)
    {
        span_rec rec = get_slot(st, slot);
        const uint16_t next = rec.next_ended;
        rec.state = SLOT_FREE;
        put_slot(st, slot, &rec);
        slot = next;
    }
    st->ended_head = NONE16;
    st->ended_tail = NONE16;
    st->ended_count = 0u;
    trim_slots(st);
    compact(st);
    memset(&st->counters, 0, sizeof(st->counters));
}

static microtel_leaf_status_t encode_with(leaf_state* st,
                                          const microtel_leaf_internal_sink* sink,
                                          size_t* written)
{
    microtel_leaf_internal_batch batch;
    microtel_leaf_status_t status;
    build_batch(st, &batch);
    status = MICROTEL_LEAF_BACKEND_ENCODE(&batch, sink, st->scratch, st->scratch_size, written);
    if (status == MICROTEL_LEAF_OK)
    {
        release_ended(st);
    }
    return status;
}

microtel_leaf_status_t microtel_leaf_encode(microtel_leaf_t* leaf,
                                            uint8_t* out,
                                            size_t out_size,
                                            size_t* written)
{
    microtel_leaf_internal_sink sink;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status == MICROTEL_LEAF_OK && (written == NULL || (out == NULL && out_size > 0u)))
    {
        status = MICROTEL_LEAF_ERR_ARG;
    }
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    memset(&sink, 0, sizeof(sink));
    sink.buf = out;
    sink.cap = out_size;
    return encode_with(state_of(leaf), &sink, written);
}

microtel_leaf_status_t microtel_leaf_encode_to(microtel_leaf_t* leaf,
                                               int (*write)(void* ctx,
                                                            const uint8_t* bytes,
                                                            size_t len),
                                               void* write_ctx,
                                               size_t* written)
{
    microtel_leaf_internal_sink sink;
    microtel_leaf_status_t status = check_leaf(leaf);
    if (status == MICROTEL_LEAF_OK && (write == NULL || written == NULL))
    {
        status = MICROTEL_LEAF_ERR_ARG;
    }
    if (status != MICROTEL_LEAF_OK)
    {
        return status;
    }
    memset(&sink, 0, sizeof(sink));
    sink.write = write;
    sink.write_ctx = write_ctx;
    return encode_with(state_of(leaf), &sink, written);
}

/* ------------------------------------------------------------------------ */
/* Size bound                                                               */
/* ------------------------------------------------------------------------ */

static size_t bound_field(size_t len)
{
    return BOUND_TAG + BOUND_LEN + len;
}

static size_t bound_kv(const microtel_leaf_kv_t* kv)
{
    size_t value = BOUND_TAG + BOUND_VARINT;
    if (kv->type == MICROTEL_LEAF_VALUE_STRING)
    {
        value = bound_field(kv->value.s.len);
    }
    return bound_field(bound_field(kv->key_len) + bound_field(value));
}

static size_t bound_event(const microtel_leaf_internal_event* ev)
{
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_kv_t kv;
    size_t n = BOUND_TAG + BOUND_FIXED64 + bound_field(ev->name_len);
    while (microtel_leaf_internal_next_event_attr(ev, &c, &kv))
    {
        n += bound_kv(&kv);
    }
    return bound_field(n);
}

static size_t bound_span(const microtel_leaf_internal_batch* b,
                         const microtel_leaf_internal_span* s)
{
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_internal_cursor e = {0u, 0u};
    microtel_leaf_internal_event ev;
    microtel_leaf_kv_t kv;
    size_t n = bound_field(TRACE_ID_BYTES) + 2u * bound_field(SPAN_ID_BYTES) +
               bound_field(s->name_len) + (size_t)3u * (BOUND_TAG + BOUND_VARINT) +
               bound_field(bound_field(s->status_message_len) + BOUND_TAG + BOUND_VARINT);
    while (microtel_leaf_internal_next_attr(b, s, &c, &kv))
    {
        n += bound_kv(&kv);
    }
    while (microtel_leaf_internal_next_event(b, s, &e, &ev))
    {
        n += bound_event(&ev);
    }
    return bound_field(n);
}

size_t microtel_leaf_encoded_size(const microtel_leaf_t* leaf)
{
    microtel_leaf_internal_batch b;
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_internal_cursor sc = {0u, 0u};
    microtel_leaf_internal_span span;
    microtel_leaf_kv_t kv;
    size_t resource = 0;
    size_t spans = 0;
    size_t scope;
    const leaf_state* st;
    if (check_leaf(leaf) != MICROTEL_LEAF_OK)
    {
        return 0u;
    }
    st = cstate_of(leaf);
    /* A bound, not the next payload: no clock read, every reserved attribute. */
    memset(&b, 0, sizeof(b));
    b.core = st;
    while (microtel_leaf_internal_next_resource_attr(&b, &c, &kv))
    {
        resource += bound_kv(&kv);
    }
    memset(&kv, 0, sizeof(kv));
    kv.key_len = sizeof(k_key_dropped_spans) - 1u; /* the longest reserved key */
    resource += MICROTEL_LEAF_INTERNAL_MAX_RESERVED * bound_kv(&kv);
    b.time_offset = 0u;
    while (microtel_leaf_internal_next_span(&b, &sc, &span))
    {
        spans += bound_span(&b, &span);
    }
    scope = bound_field(bound_field(st->scope_name_len) + bound_field(st->scope_version_len));
    return bound_field(bound_field(resource) + bound_field(scope + spans));
}
