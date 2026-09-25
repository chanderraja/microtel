# microtel Leaf / Concentrator Design

**Status:** Draft — awaiting sign-off.
**Issue:** #319.
**Implements:** [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md) (scope,
backends, nanopb, decode-into-the-pipeline), with the release renumbered to
v1.2 by [ICP 0032](icps/0032-release-reorder-v1.1.1.md).
**Companion documents:** `microtel-spec.md` §18.4 and §12,
`microtel-roadmap.md` v1.2, `docs/architecture.md`, `docs/interfaces.md`,
`docs/threading-model.md`, `docs/memory-model.md`, `docs/error-model.md`.

ICP 0031 Decision 6 requires this document to be signed off before any leaf
or concentrator code is written, the way `docs/metrics-design.md` preceded
metrics. It decides *what* and *why*. The signatures below are proposals
concrete enough to review; the headers themselves are written in the first
implementation packet and must match what is signed off here. Every point
that changes a locked interface or document is marked **ICP** and lands as
its own ICP before the code that needs it.

## Scope

In scope for v1.2 (experimental):

- **microtel-leaf**: a C library that builds spans in caller-owned memory and
  encodes them as an OTLP `ExportTraceServiceRequest`. Two encoder backends,
  upb and nanopb, selected at build time, producing identical bytes.
- **Concentrator role**: a public ingest call on the existing C++ runtime
  that decodes a leaf payload with upb, enriches its Resource per leaf,
  corrects its timestamps, and feeds the spans into the normal sampling /
  batching / export pipeline.
- The three time modes of spec §18.4.
- The ship gates of ICP 0031: fuzzed ingest, end-to-end tests per backend and
  protocol, published footprints with a Cortex-M CI job, and an example under
  `examples/leaf/`.

Out of scope, unchanged from spec §18.4 and ICP 0031 Decision 1:

- RTOS ports, reliable delivery on the leaf-to-concentrator link, time
  synchronisation beyond the three modes, leaf-side sampling.
- Pass-through forwarding of raw leaf bytes.
- Any inbound socket in microtel. The application owns the transport.
- Leaf metrics and leaf logs (§1.3 explains why; §6.4 says what metrics will
  need when they come).
- Leaf authentication, concentrator HA and multi-tenant routing (v2.2).

## Sign-off checklist

Each item has a proposed **Decision** below. The reviewer approves by checking
every box, after editing any decision they want changed.

- [ ] §1 Leaf C API: lifecycle, span building, encode, errors, no-heap rule,
      header layout, symbol prefixes, versioning. **Traces only** in v1.2.
- [ ] §2 Encoder backends: `MICROTEL_LEAF_ENCODER`, byte-identity rules and
      how they are tested, `gen/` layout, regen script, nanopb renaming.
- [ ] §3 Concentrator ingest: public `LeafReceiver` API, error model, threading,
      upb decode, pipeline entry, size limits. Three new `DropReason`s
      (**ICP**), `Provider::GetLeafReceiver` (**ICP**), per-record Resource on
      `ISpanProcessor` (**ICP**).
- [ ] §4 Per-leaf identity and configuration: leaf id, TOML / env / code
      schema, Resource merge order, fleet-size limits.
- [ ] §5 Time modes: concentrator-stamped, sync-relative, boot-relative.
- [ ] §6 ICP 0031 open questions: `MICROTEL_WITH_CONCENTRATOR` **OFF**,
      per-leaf cardinality limits, **delta-only** leaf metrics, default
      backend **nanopb**.
- [ ] §7 Test plan against the ICP 0031 ship gates.
- [ ] §8 Follow-up ICPs and document edits.

---

## §1 Leaf C API

### 1.1 Design constraints

These come from spec §18.4 and ICP 0031 Decision 3 and are not re-decided
here:

- C, not C++. No C++ runtime, no exceptions, no RTTI.
- No threads, no batching policy, no retries, no TLS, no HTTP. The leaf
  produces bytes; the application sends them.
- One public API for both backends. No `upb_*` or `pb_*` type or symbol in the
  public header. Only one source file per backend includes that encoder's
  headers.
- The nanopb build must not need a heap.
- Every allocation belongs to a buffer or arena the caller owns. Every
  resource has a paired init / free function.

### 1.2 Language level and portability

**Decision.** C11, built with `-std=c11 -pedantic-errors` and no compiler
extensions: no VLAs, no GNU statement expressions, no `__attribute__` in the
public header. C11 rather than C99 so the implementation can use
`_Static_assert` and `<stdalign.h>` to check the opaque-storage sizes in §1.4.
Every embedded GCC and Clang release of the last decade accepts C11. The leaf
calls only freestanding-safe libc functions: `memcpy`, `memmove`, `memset`
and `memcmp`. No `stdio`, no `malloc`, no `strlen` on caller input (lengths
are passed explicitly or bounded).

*Alternative considered:* C99. It would admit a few older vendor toolchains
but lose `_Static_assert`, which the opaque-storage design depends on. If the
reviewer knows of a target audience stuck on C99, the checks can be done with
the negative-array-size trick instead.

### 1.3 Signals: traces only in v1.2

**Decision.** The first leaf release encodes spans only. Metrics and logs are
not in the v1.2 leaf API.

Rationale:

- **Metrics have a temporality problem the concentrator cannot solve.** A
  leaf restarts often (power loss, watchdog, firmware update) and keeps no
  state across a restart. Cumulative sums need a stable `start_time_unix_nano`
  per series and a way to tell a reset from a wrap. In two of the three time
  modes (§5) the concentrator derives absolute time from arrival time, so the
  start time of the same series would jitter by the link latency on every
  payload, and a backend would read each payload as a new series. Converting
  delta to cumulative at the concentrator would need per-leaf, per-series
  state, which is exactly the unbounded memory §4.5 exists to prevent. The
  answer is to require delta temporality from leaves (§6.4), and that deserves
  its own design pass rather than a rider on this one.
- **Logs are simpler but not free.** They need a second record model in the
  leaf's buffer, a second message tree in both backends, a second set of golden
  vectors, and a second decode path. That roughly doubles the byte-identity
  test surface in the release where both backends are new.
- **Footprint.** The < 15 KB flash target for nanopb (roadmap §6) is for a
  trace-only leaf. Every signal added is descriptor tables and code on a
  Cortex-M0+.

Logs are the natural second signal and can follow in a later 1.x minor as an
additive API. Metrics follow after that, delta-only.

### 1.4 Types and memory ownership

The leaf never allocates. The caller provides three pieces of memory:

| Memory | Holds | Lifetime |
|---|---|---|
| `microtel_leaf_t` | the leaf's fixed-size state: config copy, counters, clock sync state, record-buffer bookkeeping | from `microtel_leaf_init` to `microtel_leaf_free` |
| record buffer | every span that has been started and not yet encoded, with its name, attributes, events and status, all copied in | same |
| output buffer | the encoded bytes of one payload | per `microtel_leaf_encode` call |

The upb backend also takes an optional **scratch** buffer for its arena
(§2.2). The nanopb backend ignores it.

**Opaque storage.** `microtel_leaf_t` and `microtel_leaf_span_t` are declared
in the public header as structs with a single private array member of fixed
size. The caller can allocate them statically or on the stack; the
implementation casts to its real struct and checks with `_Static_assert` that
the real struct fits and that the alignment matches. The sizes are the same
for both backends, because backend-specific state never lives in these
structs.

```c
/* leaf/include/microtel/leaf.h */
#define MICROTEL_LEAF_STATE_WORDS 32u  /* exact value set by the implementation */

typedef struct microtel_leaf
{
    uint64_t microtel_private[MICROTEL_LEAF_STATE_WORDS];
} microtel_leaf_t;

/* A span handle: a slot index plus a generation count. Copyable by value. */
typedef struct microtel_leaf_span
{
    uint32_t microtel_private[2];
} microtel_leaf_span_t;
```

**Strings are copied.** Span names, attribute keys, string values, event
names and status messages are copied into the record buffer when they are
set. The caller's pointers need to be valid only for the duration of the call.
Borrowing string literals would save RAM, but a leaf API whose correctness
depends on the caller never passing a stack buffer is a crash waiting for a
firmware update. Every string parameter comes with an explicit length
(`const char *s, size_t len`) so that no leaf function scans caller memory
for a terminator. Strings must be UTF-8. The leaf does not validate them (too
costly on an MCU); the concentrator rejects a payload with invalid UTF-8,
because upb validates proto3 `string` fields on decode.

**Record buffer layout.** A span table grows from the front of the buffer and
string and attribute data from the back. Encoding serialises the ended spans
and then compacts the spans that are still open to the front, so a
long-running span survives an encode. A handle carries a generation count, so
a handle to a span that has since been encoded is detected and rejected
instead of silently writing into a different span.

### 1.5 Configuration and lifecycle

```c
typedef enum microtel_leaf_time_mode
{
    MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED = 0,
    MICROTEL_LEAF_TIME_SYNC_RELATIVE = 1,
    MICROTEL_LEAF_TIME_BOOT_RELATIVE = 2
} microtel_leaf_time_mode_t;

typedef enum microtel_leaf_value_type
{
    MICROTEL_LEAF_VALUE_BOOL = 0,
    MICROTEL_LEAF_VALUE_INT64 = 1,
    MICROTEL_LEAF_VALUE_DOUBLE = 2,
    MICROTEL_LEAF_VALUE_STRING = 3
} microtel_leaf_value_type_t;

typedef struct microtel_leaf_kv
{
    const char *key;
    size_t key_len;
    microtel_leaf_value_type_t type;
    union
    {
        int b;
        int64_t i;
        double d;
        struct
        {
            const char *ptr;
            size_t len;
        } s;
    } value;
} microtel_leaf_kv_t;

typedef struct microtel_leaf_config
{
    uint32_t struct_size;  /* sizeof(microtel_leaf_config_t); see §1.9 */
    microtel_leaf_time_mode_t time_mode;

    /* Monotonic clock in nanoseconds. NULL means the leaf has no clock (§5.2). */
    uint64_t (*now_ns)(void *ctx);
    void *clock_ctx;

    /* Required: fills `out` with `len` random bytes, for trace and span ids. */
    void (*random_bytes)(void *ctx, uint8_t *out, size_t len);
    void *random_ctx;

    /* The leaf's own Resource. Copied at init. */
    const microtel_leaf_kv_t *resource;
    size_t resource_count;

    /* Instrumentation scope for every span this leaf emits. Copied at init. */
    const char *scope_name;
    size_t scope_name_len;
    const char *scope_version;
    size_t scope_version_len;

    /* Per-span caps; 0 selects the default in brackets. */
    uint16_t max_attributes_per_span;  /* [16] */
    uint16_t max_events_per_span;      /* [4]  */
    uint16_t max_attributes_per_event; /* [4]  */

    /* Boot-relative mode only: a value that differs on every boot (§5.4). */
    uint32_t boot_id;

    /* upb backend only; ignored by nanopb. NULL means use the heap (§2.2). */
    void *scratch;
    size_t scratch_size;
} microtel_leaf_config_t;

microtel_leaf_status_t microtel_leaf_init(microtel_leaf_t *leaf,
                                          const microtel_leaf_config_t *config,
                                          void *record_buffer,
                                          size_t record_buffer_size);

void microtel_leaf_free(microtel_leaf_t *leaf);
```

`microtel_leaf_init` validates the config, copies the Resource and scope into
the record buffer, and fails without side effects if anything is invalid or
the buffer is too small to hold even the Resource. `microtel_leaf_free`
releases nothing (there is nothing to release) but clears the state so that a
use-after-free is detected as `MICROTEL_LEAF_ERR_STATE` rather than
undefined behaviour. The pair exists so the rule "every init has a free"
holds, and so that a future backend that does hold resources needs no API
change. After `free`, the caller may reuse the record buffer.

A `microtel_leaf_t` is **not thread-safe**. A leaf is single-threaded by
design; an application that emits spans from an ISR and a task must serialise
access itself or use one leaf per context.

### 1.6 Building spans

```c
/* Values match the OTLP enums, so the backends copy them unchanged. */
typedef enum microtel_leaf_span_kind
{
    MICROTEL_LEAF_SPAN_KIND_INTERNAL = 1,
    MICROTEL_LEAF_SPAN_KIND_SERVER = 2,
    MICROTEL_LEAF_SPAN_KIND_CLIENT = 3,
    MICROTEL_LEAF_SPAN_KIND_PRODUCER = 4,
    MICROTEL_LEAF_SPAN_KIND_CONSUMER = 5
} microtel_leaf_span_kind_t;

typedef enum microtel_leaf_status_code
{
    MICROTEL_LEAF_STATUS_UNSET = 0,
    MICROTEL_LEAF_STATUS_OK = 1,
    MICROTEL_LEAF_STATUS_ERROR = 2
} microtel_leaf_status_code_t;

microtel_leaf_status_t microtel_leaf_span_start(microtel_leaf_t *leaf,
                                                microtel_leaf_span_t *out_span,
                                                const char *name, size_t name_len,
                                                microtel_leaf_span_kind_t kind,
                                                const microtel_leaf_span_t *parent);

/* A parent received from upstream, e.g. from a command the concentrator sent. */
microtel_leaf_status_t microtel_leaf_span_start_remote(microtel_leaf_t *leaf,
                                                       microtel_leaf_span_t *out_span,
                                                       const char *name, size_t name_len,
                                                       microtel_leaf_span_kind_t kind,
                                                       const uint8_t trace_id[16],
                                                       const uint8_t parent_span_id[8]);

microtel_leaf_status_t microtel_leaf_span_set_attribute(microtel_leaf_t *leaf,
                                                        microtel_leaf_span_t span,
                                                        const microtel_leaf_kv_t *attr);

microtel_leaf_status_t microtel_leaf_span_add_event(microtel_leaf_t *leaf,
                                                    microtel_leaf_span_t span,
                                                    const char *name, size_t name_len,
                                                    const microtel_leaf_kv_t *attrs,
                                                    size_t attr_count);

microtel_leaf_status_t microtel_leaf_span_set_status(microtel_leaf_t *leaf,
                                                     microtel_leaf_span_t span,
                                                     microtel_leaf_status_code_t code,
                                                     const char *message, size_t message_len);

microtel_leaf_status_t microtel_leaf_span_end(microtel_leaf_t *leaf,
                                              microtel_leaf_span_t span);
```

`parent` is NULL for a root span, or a handle to an open or ended span of the
same leaf. The leaf generates ids with `random_bytes` and reads `now_ns` at
start, at each event, and at end. Setting the same attribute key twice
overwrites, as the SDK does.

What the v1.2 leaf deliberately leaves out, to keep flash small:

- **Links** and **array attribute values.** Both cost descriptor tables and
  code on the nanopb side for rare use. The decoder accepts them from any
  producer (§3.5), so adding them to the leaf later is not a wire change.
- **Trace state** and **span flags**. A leaf has no W3C propagation.
- **`dropped_*_count` fields.** `internal::SpanRecord` has nowhere to put them,
  so the concentrator could not forward them anyway. Leaf-side drops are
  reported per payload instead (§1.7).

### 1.7 Errors

```c
typedef enum microtel_leaf_status
{
    MICROTEL_LEAF_OK = 0,
    MICROTEL_LEAF_ERR_ARG = -1,          /* NULL or out-of-range argument */
    MICROTEL_LEAF_ERR_STATE = -2,        /* not initialised, freed, stale or ended handle */
    MICROTEL_LEAF_ERR_NO_SPACE = -3,     /* record buffer full; the item was dropped */
    MICROTEL_LEAF_ERR_LIMIT = -4,        /* per-span cap reached; the item was dropped */
    MICROTEL_LEAF_ERR_BUFFER_SMALL = -5, /* output buffer too small; nothing consumed */
    MICROTEL_LEAF_ERR_ENCODE = -6,       /* backend failure (upb arena exhausted) */
    MICROTEL_LEAF_ERR_CLOCK = -7         /* clock_sync given an invalid time (§5.3) */
} microtel_leaf_status_t;
```

The leaf follows the SDK's hot-path rule in C form: a failed span, attribute
or event call **drops the item, counts it, and returns an error code the
caller may ignore**. Nothing a span call can fail on leaves the leaf in a bad
state. The counters live in `microtel_leaf_t`:

- `dropped_spans`: spans refused at start for lack of space.
- `dropped_attributes`, `dropped_events`: items refused for space or limits.

```c
typedef struct microtel_leaf_counters
{
    uint32_t dropped_spans;
    uint32_t dropped_attributes;
    uint32_t dropped_events;
} microtel_leaf_counters_t;

void microtel_leaf_get_counters(const microtel_leaf_t *leaf, microtel_leaf_counters_t *out);
```

`microtel_leaf_get_counters()` exposes them to the application. They are also
sent in the next payload as reserved Resource attributes (§3.8), so the
concentrator can report leaf-side loss without the application doing
anything, and they reset once that payload has encoded successfully.

`microtel_leaf_encode` returns `MICROTEL_LEAF_ERR_BUFFER_SMALL` without
consuming anything and writes the required size to `*written`, so the caller
can retry with a larger buffer or send what fits another way. Neither backend
can produce a partial payload.

### 1.8 Encoding

```c
/* Encodes every ended span into one ExportTraceServiceRequest in `out`.
 * On MICROTEL_LEAF_OK the encoded spans are released from the record buffer.
 * On MICROTEL_LEAF_ERR_BUFFER_SMALL, *written is the size needed. */
microtel_leaf_status_t microtel_leaf_encode(microtel_leaf_t *leaf,
                                            uint8_t *out, size_t out_size,
                                            size_t *written);

/* Streaming form: bytes are handed to `write` in order, with no output buffer.
 * `write` returns 0 on success; any other value aborts the encode and leaves
 * the spans in the buffer. */
microtel_leaf_status_t microtel_leaf_encode_to(microtel_leaf_t *leaf,
                                               int (*write)(void *ctx, const uint8_t *bytes,
                                                            size_t len),
                                               void *write_ctx,
                                               size_t *written);

/* Upper bound on the next encode's size, for sizing `out`. */
size_t microtel_leaf_encoded_size(const microtel_leaf_t *leaf);
```

The payload is exactly the protobuf body of an OTLP
`ExportTraceServiceRequest`, with no gRPC length prefix and no compression. It
has exactly one `ResourceSpans` holding exactly one `ScopeSpans`. It is the
wire contract between leaf and concentrator. The application's transport must
preserve message boundaries: a UDP datagram does, a UART stream needs framing
the application supplies (COBS or a length prefix). The leaf does not frame.

The streaming form exists for the RAM budget. On nanopb it writes straight
through a `pb_ostream_t` with a callback, so a Cortex-M0+ never holds the whole
payload. On upb it encodes into the arena and then calls `write` once, since
upb cannot stream; the API is the same, only the memory profile differs.

*Alternative considered:* buffer-only encode. It is less code, but it forces
every MCU user to hold a full payload in RAM next to the record buffer that
already holds the same data, which is most of a 2 KB budget.

### 1.9 Header layout, symbols and versioning

**Layout.** The leaf lives in a new top-level directory, per ICP 0031
Decision 3:

```
leaf/
  CMakeLists.txt            usable standalone: cmake -S leaf (§2.1)
  README.md                 directory map, file list for non-CMake firmware builds
  include/microtel/leaf.h   the only public header, installed as <microtel/leaf.h>
  src/leaf_core.c           config, record buffer, span building, clocks
  src/leaf_internal.h       the backend contract (§2.2); not installed
  src/backend_upb.c         the only file that includes upb headers
  src/backend_nanopb.c      the only file that includes nanopb headers
  nanopb/otlp_trace.options nanopb generator options (§2.4)
```

`leaf.h` is valid C11 and valid C++ (it carries `extern "C"` guards), so the
concentrator's tests and the end-to-end test can include it.

**Symbols.** Every external symbol the leaf defines starts with
`microtel_leaf_`; every macro with `MICROTEL_LEAF_`. Internal cross-file
functions use `microtel_leaf_internal_` so that `symbol-scan` can tell them
from API. Vendored code is renamed: `microtel_upb_*` (already, ICP 0020) and
`microtel_pb_*` (§2.5). The leaf's `symbol-scan` pass (§7.6) fails on any other
global.

*Alternative considered:* a short prefix such as `mtl_`. It reads better in
C, but it is not obviously microtel's, and the project already committed to
the `microtel_` prefix for everything it ships.

**Versioning.** Three separate version numbers:

1. **Library version.** `MICROTEL_LEAF_VERSION_MAJOR/MINOR/PATCH` in the
   header, equal to the microtel release, and `microtel_leaf_version()` which
   returns the same numbers from the compiled library so a firmware can detect
   a header / archive mismatch.
2. **Config struct size.** `microtel_leaf_config_t::struct_size` must be set
   to `sizeof(microtel_leaf_config_t)`. A later minor may append fields; the
   library treats fields past `struct_size` as zero. This lets the struct grow
   within the experimental period without breaking source that was compiled
   against an older header.
3. **Wire contract version.** Every payload carries `microtel.leaf.proto = 1`
   (§3.8). The concentrator rejects a payload whose version it does not know
   (§3.4). This changes only when the meaning of the reserved attributes or
   the timestamp encoding changes, not when the C API changes.

The C API is **experimental in v1.2** and may break in any 1.x minor; the
header says so in its opening comment. v2.0 makes it stable (roadmap v2.0).

---

## §2 Encoder backends

### 2.1 Build selection

**Decision.**

| Option | Values | Default | Meaning |
|---|---|---|---|
| `MICROTEL_BUILD_LEAF` | ON / OFF | OFF | build `microtel_leaf`, exported as `microtel::leaf` (ICP 0031 Decision 3) |
| `MICROTEL_LEAF_ENCODER` | `upb` / `nanopb` | `nanopb` (§6.1) | which backend `microtel::leaf` is built with |

- `leaf/CMakeLists.txt` works both as a subdirectory of the main project and
  as a standalone project (`cmake -S leaf`). The standalone form needs only a
  C compiler. It never runs `find_package` for nghttp2, OpenSSL or zlib, and
  never enables the CXX language. This is what makes an `arm-none-eabi-gcc`
  build possible, since the main project cannot be configured for a bare-metal
  target.
- The upb backend links the existing `microtel_upb_runtime`,
  `microtel_upb_gen` and `microtel_utf8_range` archives rather than compiling
  its own copy. A process that links both `microtel::microtel` and a upb leaf
  (the end-to-end test, or a Linux gateway that is also a leaf) therefore gets
  one copy of upb, not two conflicting ones.
- The nanopb backend links a new `microtel_nanopb` archive (runtime encoder
  only; `pb_decode.c` is not compiled) and a `microtel_nanopb_gen` archive of
  the generated trace descriptors.
- `microtel::leaf` is never a dependency of `microtel::microtel` (ICP 0031
  Decision 3).

A backend is compiled into `leaf_core.c` through a macro,
`MICROTEL_LEAF_BACKEND_ENCODE`, that names the backend's entry point. The two
entry points have different names (`microtel_leaf_internal_encode_upb`,
`microtel_leaf_internal_encode_nanopb`), so a test-only build can link both
into one binary and compare them (§7.2). Shipped builds contain one.

### 2.2 The backend contract

The core hands a backend a read-only, backend-neutral view of the batch. A
backend owns no state between calls.

```c
/* leaf/src/leaf_internal.h — not installed */
typedef struct microtel_leaf_internal_batch
{
    const microtel_leaf_internal_resource *resource; /* leaf Resource + reserved attrs */
    const microtel_leaf_internal_scope *scope;
    const microtel_leaf_internal_span *const *spans; /* ended spans, in end order */
    size_t span_count;
} microtel_leaf_internal_batch;

typedef struct microtel_leaf_internal_sink
{
    uint8_t *buf;                   /* non-NULL: buffer mode */
    size_t cap;
    int (*write)(void *, const uint8_t *, size_t); /* non-NULL: streaming mode */
    void *write_ctx;
} microtel_leaf_internal_sink;

microtel_leaf_status_t microtel_leaf_internal_encode_upb(
    const microtel_leaf_internal_batch *batch, const microtel_leaf_internal_sink *sink,
    void *scratch, size_t scratch_size, size_t *written);

microtel_leaf_status_t microtel_leaf_internal_encode_nanopb(
    const microtel_leaf_internal_batch *batch, const microtel_leaf_internal_sink *sink,
    size_t *written);
```

The core decides **what** is emitted: which spans, which fields are present,
attribute order, timestamp values. A backend decides only **how** the bytes
are produced. That split is what makes byte identity achievable: every
presence decision is made once, in shared code.

**upb backend.** Builds the message tree with the generated accessors in
`gen/` inside an arena, calls `upb_Encode`, and copies out. With `scratch`
set, the arena is `upb_Arena_Init(scratch, scratch_size, NULL)`, which cannot
grow, so the encode fails with `MICROTEL_LEAF_ERR_ENCODE` rather than touching
the heap. With `scratch` NULL it uses `upb_Arena_New()`. *Verify during
implementation:* that a NULL `upb_alloc` in the vendored v29.4 means "never
grow" rather than "use the global allocator"; the arena header declares the
signature, not that behaviour.

**nanopb backend.** Encodes with `pb_encode` over the generated descriptors,
using `FT_CALLBACK` for every string and repeated field so that no generated
struct carries a fixed-size array (§2.4). The callbacks read directly from the
record buffer. nanopb sizes a submessage by running its callbacks once with a
sizing stream and again to write, so the callbacks must be pure functions of
the batch; they are.

### 2.3 Byte identity

ICP 0031 requires both backends to produce the same bytes for the same input.
Protobuf does not define a canonical encoding, so this holds only if both
backends follow the same rules. The rules, enforced by the core and checked by
the tests in §7.2:

1. **Field order.** Fields are written in ascending field-number order. upb
   serialises in field-number order for messages without unknown fields or
   extensions (it encodes backwards, from the last field), and nanopb writes
   fields in descriptor order, which the generator sorts by tag. *Verify:*
   both claims against the pinned versions with the golden vectors before the
   rest of the backend is written.
2. **Presence.** proto3 scalars and strings equal to their default (0, false,
   empty) are omitted. Submessages are present exactly when the core says so:
   `ResourceSpans.resource` and `ScopeSpans.scope` always; `Span.status` only
   when its code is not UNSET or its message is non-empty. On nanopb this is the
   generated `has_` flag; on upb it is whether the submessage was created.
3. **Repeated order.** Spans in end order; attributes in first-set order (an
   overwrite keeps the position); events in add order.
4. **Values.** Timestamps are `fixed64`; `double` values are written from
   their bit pattern without normalisation; `int64` negatives are ten-byte
   varints on both. No packed fields occur in the trace schema.
5. **One Resource, one scope.** Exactly one `ResourceSpans` and one
   `ScopeSpans` per payload. `schema_url` fields are never set.
6. **`AnyValue`.** Only the four scalar alternatives in §1.5 are emitted.
   nanopb's handling of callback fields inside a `oneof` is the part most
   likely to be awkward. *Verify:* whether the pinned nanopb supports
   `FT_CALLBACK` members in a `oneof`. If it does not, the nanopb backend writes
   `KeyValue` with a hand-written callback that emits the tag and value through
   `pb_encode_tag` / `pb_encode_string` / `pb_encode_varint` /
   `pb_encode_fixed64`. That is about fifty lines and is probably smaller than
   the descriptor anyway.

### 2.4 Generated code under `gen/`

**Decision.** nanopb output goes in a sibling tree, so the upb tree and its
`regen-check` job are unchanged:

```
gen/
  opentelemetry/...            existing upb output, untouched
  nanopb/opentelemetry/proto/
    common/v1/common.pb.{c,h}
    resource/v1/resource.pb.{c,h}
    trace/v1/trace.pb.{c,h}
    collector/trace/v1/trace_service.pb.{c,h}
```

Only the four trace-path protos are generated for nanopb; metrics and logs
are not needed (§1.3). The generator options live in
`leaf/nanopb/otlp_trace.options`. They are microtel's own file, so they do not
belong in `proto/`, which holds only the vendored upstream schema. The key
options:

- every `string`, `bytes` and `repeated` field: `type:FT_CALLBACK`, except the
  ids below
- `trace_id`: `max_size:16 fixed_length:true`; `span_id` and
  `parent_span_id`: `max_size:8 fixed_length:true` (static, since they have a
  fixed size and are always present)
- no `FT_POINTER` anywhere, so `PB_ENABLE_MALLOC` is never defined

Compile definitions for the nanopb archive: `PB_NO_ERRMSG` (saves the error
strings) and no `PB_BUFFER_ONLY` (the streaming encode needs callback
streams). `PB_WITHOUT_64BIT` cannot be used, because OTLP timestamps are
`fixed64`.

### 2.5 Regeneration and renaming

**Regen script.** `ci/scripts/regen-protos.sh` gains a `--nanopb-generator
PATH` argument. When it is given, the script also runs `nanopb_generator.py`
(through `protoc` with the nanopb plugin) over the four trace protos with the
options file and writes to `gen/nanopb/`. When it is absent, the script
behaves as today, so existing developer invocations keep working. The
`regen-check` CI job installs the pinned nanopb generator (a Python package
plus the protoc it already has) and passes the flag, so a diff under
`gen/nanopb/` fails CI the same way `gen/opentelemetry/` does. The generator is
a developer-time tool only (ICP 0031 Decision 4).

**nanopb pin.** Vendored under `third_party/nanopb/` at a pinned 0.4.x
release: the runtime sources `pb.h`, `pb_common.{c,h}`, `pb_encode.{c,h}`, its
`LICENSE.txt` (zlib), and a `README.md` pin table in the same form as
`third_party/upb/README.md`. The generator is not vendored; the regen script
documents the exact version to install. The latest release at the time of
writing is 0.4.9.x. *Verify* the exact tag when vendoring.

**Renaming.** `third_party/nanopb/microtel_pb_rename.h` is force-included
(`-include`) into every nanopb and generated-nanopb translation unit, exactly
as `microtel_upb_rename.h` is for upb (ICP 0020 Decision 4). It maps each
global the runtime defines (`pb_encode`, `pb_encode_tag`, `pb_ostream_from_buffer`,
`pb_field_iter_begin` and the rest) to `microtel_pb_<name>`. The list is
produced from `nm` over the unrenamed archive and committed, with the recipe in
the header's comment.

**Generated descriptors are renamed too.** nanopb's generated
`opentelemetry_proto_*_msg` descriptors are global symbols. Unlike upb's
generated names, which CLAUDE.md rule 13 leaves unchanged, these are likely to
collide: a firmware that already generates OTLP with its own nanopb is exactly
this leaf's audience. The rename header therefore also maps them to
`microtel_pb_opentelemetry_proto_*`. This is within ICP 0031 Decision 4
("its globally visible symbols get a `microtel_pb_` prefix"), and is flagged
for the reviewer in §9 because it differs from the upb precedent.

---

## §3 Concentrator ingest

### 3.1 Where the code lives

| Piece | Location | Notes |
|---|---|---|
| public API | `include/microtel/leaf_receiver.hpp` (new) | `LeafReceiver`, `IngestRequest`, `IngestResult`, `LeafReceiverOptions` |
| decode | `src/wire/encoder/otlp_trace_decoder.{hpp,cpp}` | the only new upb user; ICP 0031 Decision 5 makes this directory the upb codec in both directions |
| decoder interface | `include/microtel/internal/otlp_trace_decoder.hpp` (new) | no upb types; mock and fake under `tests/` as for every interface |
| receiver | `src/sdk/leaf_receiver.{hpp,cpp}`, `src/sdk/leaf_table.{hpp,cpp}`, `src/sdk/leaf_time.{hpp,cpp}` | validation, Resource resolution, time correction, pipeline entry |
| config | `src/common/config/` | the `[concentrator]` table (§4.2) |

Naming. The spec calls the seam a `Receiver` and the process role a
concentrator. The proposed public type is **`LeafReceiver`**: the first and
only receiver, named for what it receives. A general `IReceiver` abstraction
waits until a second receiver exists. The build option keeps the
ICP 0031 name `MICROTEL_WITH_CONCENTRATOR`, since it names the feature.

### 3.2 Public API

```cpp
// include/microtel/leaf_receiver.hpp
namespace microtel
{

enum class LeafTimeMode : std::uint8_t
{
    ConcentratorStamped = 0,
    SyncRelative = 1,
    BootRelative = 2,
};

/// @brief One leaf payload handed to the receiver.
struct IngestRequest
{
    /// Transport-derived identity of the sender (§4.1). May be empty, in
    /// which case the payload's `device.id` Resource attribute is used.
    std::string_view leaf_id;
    /// The OTLP ExportTraceServiceRequest bytes. Borrowed for the call only.
    std::span<const std::byte> payload;
    /// When the application received the payload. Unset means "now" (read
    /// from the Provider's clock at the start of the call).
    std::optional<std::chrono::system_clock::time_point> received_at;
};

enum class IngestStatus : std::uint8_t
{
    Accepted = 0,          ///< every span entered the pipeline
    PartiallyAccepted = 1, ///< decoded, but the pipeline dropped some spans
    Malformed = 2,         ///< not decodable, or failed validation (§3.4)
    TooLarge = 3,          ///< over a size limit (§3.7)
    UnknownLeaf = 4,       ///< leaf not configured and unknown_leaf = reject
    ShutDown = 5,          ///< Provider shut down
    Disabled = 6,          ///< concentrator not enabled or not compiled in
};

struct IngestResult
{
    IngestStatus status = IngestStatus::Disabled;
    std::uint32_t spans_accepted = 0; ///< entered the span processor
    std::uint32_t spans_sampled_out = 0;
    std::uint32_t spans_dropped = 0;  ///< dropped by limits or the processor
};

/// @brief Counters that describe the receiver, not the export pipeline.
struct LeafReceiverStats
{
    std::uint64_t payloads_accepted = 0;
    std::uint64_t payloads_rejected = 0;
    std::uint64_t leaves_tracked = 0;      ///< current size of the leaf table
    std::uint64_t leaves_evicted = 0;
    std::uint64_t leaf_reported_drops = 0; ///< sum of microtel.leaf.dropped_* (§1.7)
    std::uint64_t time_fallbacks = 0;      ///< sync-relative payloads re-anchored (§5.3)
};

class LeafReceiver
{
public:
    virtual ~LeafReceiver() noexcept = default;

    /// @threadsafety Thread-safe; may be called concurrently from any thread.
    [[nodiscard]] virtual IngestResult Ingest(const IngestRequest& request) noexcept = 0;

    [[nodiscard]] virtual LeafReceiverStats Stats() const noexcept = 0;
};

}  // namespace microtel
```

Obtained from the Provider:

```cpp
// include/microtel/provider.hpp — ICP
[[nodiscard]] virtual std::shared_ptr<LeafReceiver> GetLeafReceiver() = 0;
```

It returns a no-op receiver whose `Ingest` returns `Disabled` when the
concentrator is not enabled in config or not compiled in, the way `GetLogger`
returns a no-op logger. Adding a pure virtual to `Provider` is an interface
change and needs an **ICP**, with ICP 0012 (`GetLogger`) as the precedent.

Enabled from `SdkBuilder`:

```cpp
SdkBuilder& WithLeafReceiver(LeafReceiverOptions opts);  // §4.3
```

A new `SdkBuilder` method and a new public header are additive and follow
ICP 0031's "no existing interface changes" rule; they need the §8 docs edits,
not an ICP.

*Alternatives considered:*

- **A free factory**, `MakeLeafReceiver(std::shared_ptr<Provider>)`. It avoids
  touching `Provider`, but the receiver needs the Provider's sampler, span
  processor, diagnostics sink and clock, which are internal. The factory would
  have to downcast to `SdkProvider`, which fails for any other `Provider`
  implementation, including the test mocks.
- **`Ingest` directly on `Provider`.** One fewer type, but it puts a
  concentrator-only call on every Provider, and `Stats()` needs a home anyway.

### 3.3 Error model

`Ingest` is a data-plane call made from the application's receive loop. It
takes the hot-path regime of `error-model.md` §2.2, with one addition:

- It is `noexcept` and never throws. An allocation failure inside it is caught
  at the boundary (`std::bad_alloc`, not `catch (...)`); the payload is
  reported as `TooLarge` and counted `leaf_payload_too_large`, since running
  out of memory on it is a size problem.
- Every drop is counted on `IDiagnosticsSink` (below), so
  `GetExporterHealth()` shows it whether or not the caller looks at the result.
- **Unlike** `StartSpan`, it also returns what happened. The application has a
  real use for that: it can NACK or rate-limit a misbehaving leaf. The result
  is a small value struct, not `Expected<T, Error>`, because `Error` carries a
  `std::string` and this path should not allocate to report a failure.

`Status` is not used: it is the lifecycle enum and its four values do not fit.

**Drop accounting.** Existing reasons cover everything after decode:

| Where | Reason | Unit |
|---|---|---|
| span limits applied to a decoded span (§3.6) | `span_attribute_limit`, `span_event_limit`, `span_link_limit`, `event_attribute_limit`, `link_attribute_limit`, `attribute_value_truncated` | as today |
| `ISpanProcessor::OnEnd` | `queue_full`, `record_too_large` | records |
| call after `Shutdown` | `post_shutdown` | **payloads** (the payload is not decoded, so its spans are not counted) |

Three failures happen before there are any records, and no existing reason
describes them. **ICP:** add three `DropReason` enumerators, with the next
free values at the time the ICP lands:

| Reason | Counted when | Unit |
|---|---|---|
| `leaf_payload_malformed` | the payload fails to decode, fails validation (§3.4), declares an unsupported wire version, or its time mode conflicts with the leaf's config (§5.1) | payloads |
| `leaf_payload_too_large` | the payload exceeds `max_payload_bytes`, `max_spans_per_payload`, the decode depth limit, or the decode arena cap (§3.7) | payloads |
| `leaf_unknown` | the leaf is not configured and `unknown_leaf = "reject"` (§4.4) | payloads |

These are the first reasons counted in payloads rather than records. That is
deliberate: a payload that cannot be decoded has no trustworthy span count, and
guessing would be worse than a different unit. `error-model.md` §3 records the
unit per row, as it already does for the others.

*Alternatives considered:*

- **One reason, `leaf_payload_rejected`.** One enumerator instead of three,
  but an operator could not tell a fleet with a firmware bug (malformed) from a
  mis-sized limit (too large) from a config gap (unknown) without the
  application logging `IngestResult`. Three counters cost nothing at runtime.
- **Reuse `record_too_large` for oversize payloads.** Its meaning is a single
  record over `max_record_bytes`; reusing it for a whole payload would make the
  counter mean two things.

### 3.4 Decoding and validation

**Decoder.** A new internal interface, implemented with upb in
`src/wire/encoder/`:

```cpp
// include/microtel/internal/otlp_trace_decoder.hpp
namespace microtel::internal
{

struct DecodeLimits
{
    std::uint32_t max_spans = 0;
    std::uint16_t max_depth = 0;
    std::size_t max_arena_bytes = 0;
};

enum class DecodeFailure : std::uint8_t
{
    Malformed = 0,  ///< not a valid ExportTraceServiceRequest, or invalid UTF-8
    TooLarge = 1,   ///< a DecodeLimits bound was hit
};

/// Plain C++ values; nothing upb-derived survives the call.
struct DecodedScopeSpans
{
    InstrumentationScope scope;
    std::vector<SpanRecord> spans;
};

struct DecodedResourceSpans
{
    std::vector<KeyValue> resource;
    std::vector<DecodedScopeSpans> scopes;
};

class IOtlpTraceDecoder
{
public:
    virtual ~IOtlpTraceDecoder() noexcept = default;

    [[nodiscard]] virtual Expected<std::vector<DecodedResourceSpans>, DecodeFailure>
    Decode(std::span<const std::byte> payload, const DecodeLimits& limits) const = 0;
};

}  // namespace microtel::internal
```

It follows the encoder's arena rule (`memory-model.md` §3.1): one arena per
`Decode` call, destroyed before the call returns, never visible outside the
directory. The arena uses a counting `upb_alloc` that refuses to grow past
`max_arena_bytes`, which turns a decode that would blow up in memory into
`TooLarge`. `upb_Decode` runs with `upb_DecodeOptions_MaxDepth(max_depth)`.
`kUpb_DecodeOption_AliasString` is **not** used: strings are copied into the
`SpanRecord`s anyway, and aliasing would only add a lifetime hazard to save one
copy of data that is already small.

The decoder accepts any valid `ExportTraceServiceRequest`, not only the
subset a microtel leaf emits. Values that `microtel::AttributeValue` cannot
hold are handled as ICP 0015 Option B handles them in the otel-cpp shim:
`bytes` become a lowercase hex string. `kvlist_value`, nested arrays and
mixed-type arrays have no lossless mapping and are dropped, counted as
`span_attribute_limit`. *Reviewer:* that counter is the nearest existing
one, but not an exact fit; the alternative is a fourth new reason, and this
case cannot arise from a microtel leaf.

**Validation.** After decode, before anything enters the pipeline, the whole
payload is checked. **Any failure rejects the whole payload** as `Malformed`:

- `microtel.leaf.proto` present and a version this concentrator supports (1).
- Every span: 16-byte non-zero `trace_id`; 8-byte non-zero `span_id`;
  `parent_span_id` empty or 8 bytes; `kind` in range; `status.code` in range;
  `end_time >= start_time` in the leaf's own clock.
- The time-mode attributes that §5 requires for the declared mode are present.
- The payload's time mode is allowed by the leaf's config (§5.1).

All-or-nothing is simpler to reason about and to fuzz than per-span rejection,
and a leaf that emits one invalid span has a bug that a partial accept would
hide. After validation, per-span drops can only come from the pipeline (limits,
queue), and those are reported per span in `IngestResult`.

A payload without `microtel.leaf.proto` is rejected, so the receiver cannot be
used as a general OTLP intake. That is outside ICP 0031's scope, and accepting
it would need a decision on what timestamps mean when no mode is declared.

### 3.5 Threading, copying and backpressure

- **Which thread.** The application's, whichever one calls `Ingest`. microtel
  starts no thread for the receiver. Decode, validation, Resource resolution and
  time correction all run on the caller thread, in time linear in the payload
  size. Nothing waits on I/O.
- **Concurrency.** `Ingest` is thread-safe. The leaf table (§4.5) is guarded by
  one mutex, held only for a lookup or insert and never across a call out. The
  resolved Resource is copied out as a `shared_ptr` and the lock released
  before the span processor is called, so the "at most one non-leaf lock"
  rule (`threading-model.md` §4 rule 2) holds. A user-supplied resolver (§4.3)
  is called with no lock held; two threads that race on a new leaf may both
  resolve it, and the second insert is discarded.
- **Copying.** The payload is borrowed for the duration of the call. Bytes are
  copied twice: into the upb arena on decode, and from there into
  `SpanRecord`s. No zero-copy path is offered. The records must outlive the call
  because the processor queues them, so at least one copy is unavoidable, and
  the second is the arena, which is freed before `Ingest` returns.
- **Backpressure.** Decoded spans enter the same `BatchSpanProcessor` queue as
  in-process spans (§3.6), under the same `max_queue_size` and
  `max_total_queue_bytes`. When the queue is full, spans are dropped by the
  configured drop policy and counted `queue_full`, exactly as for in-process
  spans. `Ingest` never blocks. The application learns about pressure from
  `IngestResult::spans_dropped` and can slow its leaves down through its own
  protocol if it has one. A blocking or "wait for room" mode is not offered.

*Alternative considered:* a separate processor, queue and worker thread for
leaf spans, so a leaf flood cannot starve in-process spans. It adds a thread to
the locked inventory in `threading-model.md` §2 (an ICP) and a second queue
budget to configure. In a concentrator process, leaf spans are most of the
traffic, so a shared budget is the simpler default. It can be revisited if the
v1.2 concentrator throughput bench (roadmap §9) shows starvation.

### 3.6 How decoded spans enter the pipeline

For each span, in payload order:

1. **Time correction** (§5) rewrites `start_time`, `end_time` and event
   timestamps to Unix time.
2. **Span limits.** The Provider's `SpanLimitOptions` are applied as
   `SdkSpan` applies them: surplus attributes, events and links dropped and
   counted; long string values truncated on a UTF-8 boundary and counted. The
   existing helpers are shared rather than re-implemented.
3. **Sampling.** The Provider's sampler is called once per span with a
   `SamplingContext` whose `parent` is an **invalid** `SpanContext`, whose
   `trace_id` is the span's own, and whose name, kind, attributes and links are
   the span's. `RecordAndSample` keeps the span, applying
   `additional_attributes` and `trace_state` as for an in-process root.
   `Drop` and `RecordOnly` discard it and count it in `spans_sampled_out`,
   which is not a drop, just as an unsampled in-process span is not.
4. **Enqueue.** `ISpanProcessor` receives the `SpanRecord` with the
   leaf's scope and the leaf's resolved Resource (§4).

**Why every span is sampled as a root.** A leaf does no sampling (spec §18.4),
so it has no sampled flag to pass down, and a parent-based decision at the
concentrator would have nothing to inherit. Treating each span as a root means
a trace-id-based sampler (`TraceIdRatio`, or `ParentBased` whose root is
`TraceIdRatio`) makes the same decision for every span of a trace, even when
the trace's spans arrive in different payloads. A sampler that is not a
function of the trace id can split a leaf trace; the configuration guide will
say so.

**Per-record Resource.** Today `BatchSpanProcessor` holds one Resource for all
records and groups a drained batch by scope into one `BatchHandle` per scope
(ICP 0023). `BatchHandle` already carries its own Resource, so the exporter and
encoder need no change. The processor needs to accept a Resource per record
and group by `(Resource, scope)`. **ICP:** add to `ISpanProcessor`:

```cpp
/// @brief As OnEnd, but for a record whose Resource is not the Provider's.
/// `resource` is shared, immutable, and never null.
virtual void OnEndWithResource(SpanRecord&& record,
                               const InstrumentationScope& scope,
                               std::shared_ptr<const Resource> resource) noexcept = 0;
```

`BatchSpanProcessor` stores the pointer in its queued item (a null pointer
there means "the Provider's Resource", so in-process spans pay nothing) and
groups by pointer identity plus scope. `SimpleSpanProcessor` and the mock and
fake follow. Pure virtual rather than a default implementation, because a
default that dropped the Resource would silently mislabel a leaf's spans. This
is the "late Resource enrichment" seam of spec §18.4, realised.

**One request per leaf per batch.** Because the exporter encodes one request
per `BatchHandle`, a batch holding spans from N leaves produces N export
requests. That is correct but costs requests in a large fleet. The fix is an
encoder entry point that puts several `ResourceSpans` into one request, which
changes `IOtlpEncoder` and the exporter's partial-success accounting and so
needs its own **ICP**. **Proposed:** ship v1.2 without it, measure with the
concentrator throughput bench, and raise the ICP if the request rate is the
bottleneck. Flagged in §9.

### 3.7 Size limits

**Decision.** Defaults, all configurable (§4.2):

| Limit | Default | Enforced | On breach |
|---|---|---|---|
| `max_payload_bytes` | 64 KiB | before decode | `TooLarge`, `leaf_payload_too_large` |
| `max_spans_per_payload` | 512 | during decode | `TooLarge`, `leaf_payload_too_large` |
| decode depth | 16 | `upb_DecodeOptions_MaxDepth` | `TooLarge`, `leaf_payload_too_large` |
| decode arena | 4 × `max_payload_bytes` | counting `upb_alloc` | `TooLarge`, `leaf_payload_too_large` |
| `max_leaf_id_bytes` | 128 | before lookup | `Malformed`, `leaf_payload_malformed` |
| per-span structure | the Provider's `SpanLimitOptions` | §3.6 step 2 | per item, existing reasons |
| per-record size | `max_record_bytes` | `OnEnd` | per span, `record_too_large` |
| leaf table | §4.5 | on insert | eviction, not a drop |

The depth of 16 leaves room for the deepest legal path in the trace schema
(request → ResourceSpans → ScopeSpans → Span → Event → KeyValue → AnyValue →
ArrayValue → AnyValue is nine levels) and stops recursion bombs. The arena
factor of 4 is a first estimate of upb's decoded-to-wire ratio for OTLP spans.
*Verify* it against the golden vectors and the fuzz corpus, and set it from
the measured worst case with a margin.

A microtel leaf payload is typically well under 1 KB. 64 KiB is generous for
leaves and small enough that a hostile payload cannot cost more than about
256 KiB of transient memory per concurrent `Ingest`.

### 3.8 Reserved wire attributes

The leaf puts these in its Resource on every
payload; the concentrator reads them and removes them before resolving the
Resource (§4.4), so they never reach a collector.

| Key | Type | When | Meaning |
|---|---|---|---|
| `microtel.leaf.proto` | int | always | wire-contract version, 1 in v1.2 |
| `microtel.leaf.time_mode` | int | always | 0, 1 or 2 as in `microtel_leaf_time_mode_t`, for this payload |
| `microtel.leaf.encode_time` | int | when the leaf has a clock | `E` (§5.1) |
| `microtel.leaf.sync_age` | int | sync-relative | nanoseconds since the last `clock_sync` |
| `microtel.leaf.boot_id` | int | boot-relative | the configured `boot_id` |
| `microtel.leaf.dropped_spans` | int | when non-zero | leaf-side drops since the last successful encode (§1.7) |
| `microtel.leaf.dropped_items` | int | when non-zero | dropped attributes plus events, same period |

Integers rather than strings, because every byte counts on the leaf's link.
Resource attributes because an `ExportTraceServiceRequest` has no other
per-payload field, and inventing a wrapper message would give up "the wire
format is OTLP".

---

## §4 Per-leaf identity and configuration

### 4.1 How a leaf is identified

**Decision.** By `IngestRequest::leaf_id`, supplied by the application from
its transport: a CAN id, a BLE address, a UDP source address, a serial port
name. If it is empty, the receiver uses the payload's `device.id` Resource
attribute. If both are empty, the payload is `Malformed`.

The transport-derived id comes first because it is the one the leaf cannot
choose. A self-declared id lets a misconfigured or cloned firmware image
present itself as another device. The payload fallback exists for transports
that carry no useful address (a shared bus, a message queue). There is no
authentication in v1.2 in either case; leaf authentication is v2.2
(roadmap v2.2).

Leaf ids are opaque byte strings compared exactly, at most `max_leaf_id_bytes`
long.

### 4.2 Configuration schema: TOML

```toml
[concentrator]
enabled               = true                   # default false
max_payload_bytes     = "64KiB"
max_spans_per_payload = 512
max_leaves            = 1024                   # §4.5
max_leaf_resource_bytes = "2KiB"               # §4.5
leaf_idle_timeout     = "1h"                   # §4.5
unknown_leaf          = "accept"               # accept | reject   (§4.4)
leaf_id_attribute     = "device.id"            # "" disables        (§4.4)
default_time_mode     = "auto"                 # auto | concentrator_stamped | sync_relative | boot_relative
max_sync_age          = "1h"                   # §5.3
max_clock_skew        = "5m"                   # §5.3
boot_anchor_window    = "10m"                  # §5.4

[concentrator.leaf_defaults.resource]          # fills gaps for every leaf (§4.4)
"service.namespace"      = "boiler-fleet"
"deployment.environment" = "prod"

[concentrator.leaves."can0:0x1a4"]
time_mode = "boot_relative"
[concentrator.leaves."can0:0x1a4".resource]
"service.name" = "burner-controller"
"host.name"    = "boiler-7"
```

The strict unknown-key rule applies to the whole table. Configuring a
`microtel.leaf.*` key in any resource table is `ConfigError::Kind::InvalidValue`,
because those keys are reserved for the wire contract (§3.8).

### 4.3 Environment and code

**Environment.** Scalar settings only:

| Variable | Setting |
|---|---|
| `MICROTEL_CONCENTRATOR_ENABLED` | `enabled` |
| `MICROTEL_CONCENTRATOR_MAX_PAYLOAD_BYTES` | `max_payload_bytes` |
| `MICROTEL_CONCENTRATOR_MAX_LEAVES` | `max_leaves` |
| `MICROTEL_CONCENTRATOR_UNKNOWN_LEAF` | `unknown_leaf` |
| `MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE` | `default_time_mode` |
| `MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES` | `leaf_defaults.resource`, in `OTEL_RESOURCE_ATTRIBUTES` syntax |

Per-leaf settings have no environment form. Leaf ids contain characters that
are not valid in variable names, and a fleet's per-device table belongs in a
file or a resolver, not in a process environment.

**Code.**

```cpp
struct LeafConfig
{
    std::optional<LeafTimeMode> time_mode;  ///< unset: default_time_mode
    std::vector<KeyValue> resource;
};

/// Called the first time a leaf id is seen (and again after eviction).
/// Runs on the Ingest caller's thread with no microtel lock held.
using LeafConfigResolver =
    std::function<std::optional<LeafConfig>(std::string_view leaf_id)>;

struct LeafReceiverOptions
{
    bool enabled = true;  ///< WithLeafReceiver implies enabled
    std::uint32_t max_payload_bytes = 64U * 1024U;
    std::uint32_t max_spans_per_payload = 512;
    std::uint32_t max_leaves = 1024;
    std::uint32_t max_leaf_resource_bytes = 2U * 1024U;
    std::chrono::seconds leaf_idle_timeout{3600};
    UnknownLeafPolicy unknown_leaf = UnknownLeafPolicy::Accept;
    std::string leaf_id_attribute = "device.id";
    std::optional<LeafTimeMode> default_time_mode;  ///< unset: auto
    std::chrono::seconds max_sync_age{3600};
    std::chrono::seconds max_clock_skew{300};
    std::chrono::seconds boot_anchor_window{600};
    std::vector<KeyValue> leaf_defaults_resource;
    std::vector<std::pair<std::string, LeafConfig>> leaves;
    LeafConfigResolver resolver;
};

enum class UnknownLeafPolicy : std::uint8_t
{
    Accept = 0,
    Reject = 1,
};
```

The resolver is for fleets whose per-device table lives in a database or an
inventory service. It is the answer to "a TOML table with ten thousand
entries". A resolver that returns `std::nullopt` means "not configured", which
`unknown_leaf` then governs.

**Precedence** is spec §12.1, per setting, and per key within every table
(#257, `docs/configuration.md` §1):

- scalars: code > env > file > default
- `leaf_defaults.resource`: merged per key, code over env over file
- `leaves`: merged per leaf id; within one leaf, `resource` per key, code over
  file
- the resolver's answer sits above static `leaves` for the same leaf, per key

All table merges use `config::MergeResourceAttrs`
(`src/common/config/table_merge.hpp`), which is `Resource::Merge`, so there is
one merge rule in the codebase.

### 4.4 The merge order for a leaf's Resource

**Decision.** Lowest to highest precedence:

1. `leaf_id_attribute` (default `device.id`) set to the leaf id, if the
   setting is non-empty.
2. `leaf_defaults.resource`.
3. The leaf's own Resource from the payload, with every `microtel.leaf.*` key
   removed.
4. The leaf's configured `resource` (file, then code, then resolver).

Each step is a `Resource::Merge` with the later layer overriding, so a layer
replaces only the keys it names. If no layer sets `service.name`, it is set to
`unknown_service`, as the OTel SDK specification requires for a missing
service name.

Why this order:

- **Defaults sit below the leaf.** A default is a gap-filler. An operator
  setting `service.namespace` for the whole fleet does not mean to override a
  firmware that knows better.
- **Per-leaf config sits above the leaf.** An operator who names a specific
  device in config is correcting or enriching that device, which is the
  `device-id → service.*` mapping of spec §18.4. Firmware is harder to change
  than config.
- **The leaf id is the floor.** It guarantees that every leaf's spans can be
  told apart even with no config at all, and anything more specific wins.

**The concentrator's own Resource is not merged in.** Its `host.name`,
`service.name` and detector output describe the gateway, not the device, and
merging them would misattribute every leaf span to the gateway. Operators who
want a shared attribute (`deployment.environment`, `cloud.region`) on leaf
spans put it in `leaf_defaults.resource`. Flagged in §9 in case the reviewer
prefers an opt-in `inherit_resource_keys` list.

**Unknown leaves.** With `unknown_leaf = "accept"` (default), a leaf with no
config entry and no resolver answer is processed with layers 1–3. With
`"reject"`, its payloads are `UnknownLeaf` and counted `leaf_unknown`.
`accept` is the default because a new device in the field should show up in
telemetry, not vanish; `reject` is for deployments that treat the config as an
allow-list.

### 4.5 Fleet size and cardinality limits

Each leaf the receiver has seen costs an entry in the leaf table: the resolved
`shared_ptr<const Resource>`, a hash of the leaf-declared Resource (to notice
when it changes), the boot-relative anchor (§5.4), and a last-seen time.

**Decision.**

- `max_leaves` (default **1024**) bounds the table. When it is full, the
  least recently seen entry is evicted. Eviction loses only cached state: the
  next payload from that leaf re-resolves its Resource (calling the resolver
  again) and re-anchors its boot-relative time. No telemetry is dropped for
  eviction; it is counted in `LeafReceiverStats::leaves_evicted`.
- `leaf_idle_timeout` (default 1 h) evicts entries not seen for that long,
  checked on insert, so an idle concentrator costs no timer thread.
- `max_leaf_resource_bytes` (default 2 KiB) bounds one leaf's resolved
  Resource, keys plus values. Leaf-declared attributes that would push past it
  are dropped (lowest precedence first) and counted `span_attribute_limit`. An
  over-budget *configured* Resource is a `ConfigError::Kind::InvalidValue` at
  `Build()` time instead, because it is the operator's own setting.
- The worst-case table size is about `max_leaves × (max_leaf_resource_bytes +
  per-entry overhead)`, about 2.5 MiB at the defaults. Operators with larger
  fleets per concentrator raise `max_leaves` knowing the cost.
- If the leaf-declared Resource changes between payloads (a firmware update),
  the entry is re-resolved and replaced. Spans already queued keep the old
  Resource pointer, which is correct for them.

Batches still group by `(Resource, scope)`, so the number of distinct
Resources in flight is bounded by the queue size, not the fleet size.

---

## §5 Time modes

### 5.1 Common rules

**The leaf declares, the config constrains.** A payload says which mode its
numbers are in (`microtel.leaf.time_mode`), because only the leaf knows how it
encoded them. The leaf's config `time_mode` (or `default_time_mode`) says
which modes the concentrator accepts from that leaf:

- `auto` (default): accept whatever the payload declares.
- a specific mode: accept that mode, and also `concentrator_stamped`, which is
  the fallback every mode degrades to (§5.3). Anything else is `Malformed`.

**Timestamp fields.** In concentrator-stamped and boot-relative modes, the
leaf writes values from its own clock into the OTLP `*_time_unix_nano` fields.
They are not Unix times until the concentrator corrects them. That is a
deliberate reuse of the fields, and it is one more reason raw leaf payloads
cannot be forwarded to a collector unchanged.

**Notation.** `R` is the receive time (`IngestRequest::received_at`, or the
Provider's clock when unset). `E` is the leaf's clock reading at encode time,
sent as `microtel.leaf.encode_time`. `t` is any leaf timestamp in the payload;
`t'` is its corrected value.

### 5.2 Concentrator-stamped

For a leaf whose clock has no relation to wall time: a free-running tick
counter, or no clock at all.

- **Leaf sends:** timestamps in its own clock (any epoch, nanoseconds), and
  `E`. A leaf with no clock (`now_ns == NULL`) sends every timestamp as 0 and
  no `E`.
- **Concentrator computes:** `t' = t + (R − E)`. With no `E`, every timestamp
  becomes `R`, so spans have zero duration and the order within a payload is
  the only order preserved.
- **State:** none.
- **Error:** every timestamp is late by the time from encode to `R`: the
  application's send queue plus the link. Durations within one payload are
  exact to the leaf clock's accuracy.

### 5.3 Sync-relative

For a leaf that is told the wall time now and then, by the concentrator over
the application's protocol, or by GPS or an RTC that the application reads.

- **Leaf API:**

  ```c
  microtel_leaf_status_t microtel_leaf_clock_sync(microtel_leaf_t *leaf,
                                                  uint64_t unix_ns,
                                                  uint64_t leaf_now_ns);
  ```

  records the pair `(U₀, M₀)`. From then on the leaf converts its own
  timestamps: `u = U₀ + (m − M₀)`.
- **Leaf sends:** Unix-nanosecond timestamps, `E` as Unix nanoseconds at encode
  time, and `microtel.leaf.sync_age = m_encode − M₀`, the time since the last
  sync.
- **When conversion happens.** The leaf always records raw clock values and
  converts them at encode time, using the sync current at that moment. A span
  started before a sync and ended after it is therefore converted consistently.
  If no sync has happened by encode time, the payload is encoded in
  concentrator-stamped form and declares that mode, so a leaf that boots
  before its first sync still produces usable data. `MICROTEL_LEAF_ERR_CLOCK`
  is returned only by `microtel_leaf_clock_sync`, when `unix_ns` is 0 or
  `leaf_now_ns` is ahead of the leaf's own clock.
- **Concentrator:** trusts the timestamps (`t' = t`) when
  `sync_age ≤ max_sync_age` **and** `|R − E| ≤ max_clock_skew`. Otherwise it
  falls back to the concentrator-stamped correction on the Unix values,
  `t' = t + (R − E)`, and counts `LeafReceiverStats::time_fallbacks`. The
  fallback catches a leaf whose sync is stale (drift has accumulated) or wrong
  (a bad RTC).
- **State:** none at the concentrator.
- **Error:** the leaf clock's drift since the last sync, plus the error of the
  sync itself. No link latency.

A leaf with a trusted wall clock of its own (an NTP-synced Linux board running
the upb leaf) uses this mode, calling `microtel_leaf_clock_sync` once with the
current time and its monotonic clock, or periodically.

### 5.4 Boot-relative

For a leaf with a monotonic clock that counts from boot, whose data should keep
a consistent timeline across many payloads.

- **Leaf sends:** timestamps as nanoseconds since boot, `E` in the same clock,
  and `microtel.leaf.boot_id`, the `boot_id` from its config. The application
  must supply a value that differs on every boot: a persisted counter, or
  random bytes if nothing persists.
- **Concentrator keeps**, per leaf, `(boot_id, B)` where `B` estimates the
  wall time of the boot. For each payload, `b = R − E` is one estimate, late by
  that payload's latency. `B` is the **minimum** `b` seen over the last
  `boot_anchor_window` (default 10 min). The minimum is the estimate with the
  least latency in it; the window lets `B` follow the leaf clock's drift instead
  of pinning to one old sample for the life of the boot.
- **Concentrator computes:** `t' = t + B`.
- **A new `boot_id`** replaces the anchor. An evicted leaf (§4.5) re-anchors
  from its next payload.
- **Error:** the latency of the best payload in the window, plus drift within
  the window. Unlike concentrator-stamped, the error is the same for every
  payload in the window, so relative timing across payloads is preserved.
  When `B` moves to a lower value, payloads already exported keep the old,
  later anchor; ordering across that boundary can be off by the difference.

### 5.5 Summary

| Mode | Leaf needs | Payload carries | Concentrator state | Error source |
|---|---|---|---|---|
| concentrator-stamped | optional monotonic clock | leaf-clock times, `E` (or neither) | none | send + link latency, per payload |
| sync-relative | monotonic clock, a wall-time source, `clock_sync` calls | Unix times, `E`, `sync_age` | none | drift since sync |
| boot-relative | monotonic clock from boot, a per-boot id | since-boot times, `E`, `boot_id` | per-leaf anchor | best latency in window, drift within window |

---

---

## §6 ICP 0031's open questions

### 6.1 Default leaf backend

**Proposed: `nanopb`.**

- nanopb builds for every target upb builds for; the reverse is not true. A
  default that fails to fit on the most common IoT target is a poor first
  experience for the audience the leaf exists for.
- It is the smaller backend on every target, and byte identity means the
  choice has no effect on what the concentrator sees.
- A user who wants upb (shared code with a microtel process on the same
  Linux board) is making a deliberate choice and can set one option.

*Alternative:* no default: configuring with `MICROTEL_BUILD_LEAF=ON` and no
`MICROTEL_LEAF_ENCODER` is an error that explains the two choices. It forces a
decision that most users can make in one line, at the cost of one failed first
configure. *Alternative:* upb, as the backend with the longer history in the
project. Left to the reviewer (§9).

### 6.2 `MICROTEL_WITH_CONCENTRATOR`

**Proposed: add it, default OFF in v1.2.**

- The receiver parses untrusted bytes. An experimental feature with that
  attack surface should be present only in the binaries that use it.
- It pulls upb's decoder (`upb/wire/decode.c` and its utf8_range dependency)
  into the link, which no current microtel binary contains. ICP 0030's rule
  that "with every option at its default, the build behaves exactly as it does
  today" is kept only if the default is OFF.
- It follows ICP 0030's mechanism: the receiver's `.cpp` files and the decoder
  are left out of the build, the types in `leaf_receiver.hpp` stay in the public
  headers unconditionally, `GetLeafReceiver` returns the no-op receiver, and
  `enabled = true` in config returns `ConfigError::Kind::FeatureNotCompiled`
  naming the option. ICP 0030 is still a draft; if it has not landed when this
  work starts, the option is added on its own with the same shape and folded in
  when ICP 0030 lands.
- Revisit for ON in v2.0, when the API goes stable.

### 6.3 Per-leaf Resource cardinality

Answered in §4.5: `max_leaves` = 1024 with LRU eviction, `leaf_idle_timeout`
= 1 h, `max_leaf_resource_bytes` = 2 KiB. Eviction drops cached state, never
telemetry.

### 6.4 Delta-only leaf metrics

**Proposed: yes. When leaf metrics come, a leaf must send delta temporality,
and the concentrator rejects cumulative leaf metrics as `Malformed`.**

- A leaf cannot keep cumulative state across a restart, and the concentrator
  cannot rebuild it: it would need per-leaf, per-series memory (exactly what
  §4.5 bounds) and would still misread a restart it didn't see.
- In concentrator-stamped and boot-relative modes, a cumulative series'
  `start_time_unix_nano` would be recomputed with a different latency on each
  payload, so a backend would see a new series every time.
- The concentrator forwards delta as delta. A backend that needs cumulative
  converts downstream (the collector's `deltatocumulative` processor), where
  the state lives once instead of in every concentrator.
- Gauges have no temporality and are unaffected.

This is recorded now so the leaf's metrics design starts from it. It does not
affect v1.2, which has no leaf metrics (§1.3).

---

## §7 Test plan

Mapped to ICP 0031's ship gates. Every leaf test runs once per backend, as
the ICP requires.

### 7.1 Leaf unit tests (both backends)

GoogleTest on the host, calling the C API through `leaf.h`. One test binary per
backend (`microtel_leaf_upb_test`, `microtel_leaf_nanopb_test`) from one test
source. Coverage:

- init / free: every invalid config field, too-small buffer, `struct_size`
  older and newer than the library, use after free.
- span building: every API function's success and each error code; per-span
  caps; record buffer exhaustion, with the leaf staying usable; stale and ended
  handles; open spans surviving an encode and compaction.
- encode: buffer too small reports the exact size and consumes nothing;
  streaming with a failing `write` consumes nothing; `encoded_size` is an upper
  bound.
- time: each mode's reserved attributes; sync-relative before and after the
  first sync; no-clock leaf.
- counters: drops counted, sent as reserved attributes, reset after a
  successful encode.
- the upb backend with a fixed `scratch` never calls the heap allocator (a
  counting `upb_alloc` in the test).

### 7.2 Byte identity

- **Golden vectors.** `tests/leaf/vectors/` holds a set of C functions, each
  building one scenario with a fixed clock and a fixed random source: empty
  batch, one span, every attribute type, events, status, remote parent, each
  time mode, dropped counters, maximum-size strings, UTF-8 edge cases. Each
  vector has a committed `.bin`. Each backend's test binary encodes every
  vector and compares with the golden file byte for byte. Both backends
  matching the same file means they match each other.
- **Structural check.** Each golden file is also decoded with upb in the test
  and compared field by field with the scenario, so a golden file cannot
  silently encode the wrong thing. The first golden set is reviewed by hand
  with `protoc --decode` in the PR that adds it.
- **Differential.** A test-only library, `microtel_leaf_dual`, links both
  backends (they have distinct entry-point names, §2.1). A fuzz target,
  `leaf_backend_diff_fuzz`, reads its input as a program of builder calls,
  encodes the result with both backends, and asserts identical bytes and a
  successful decode by `IOtlpTraceDecoder`. It runs in the standing fuzz job
  with a corpus seeded from the golden vectors.
- The wire test suite (`tests/wire/`) lists the golden vectors so that
  "byte-for-byte comparison is part of the wire test suite" (ICP 0031
  Decision 3) is literally true.

### 7.3 Ingest fuzz target (gate 2)

`tests/fuzz/leaf_ingest_fuzz.cpp` drives `LeafReceiver::Ingest` on a Provider
built with a fake exporter. The first input byte selects the leaf id and the
configured time mode, and the rest is the payload. Seeds: every golden vector,
plus truncations and single-byte corruptions of each. It asserts, in addition
to the standing invariants in `tests/fuzz/README.md`:

- `spans_accepted + spans_sampled_out + spans_dropped` equals the decoded span
  count for an accepted payload, and is zero for a rejected one;
- the decode arena never exceeds its cap (a counting allocator);
- every rejected payload increments exactly one of the three new drop
  counters (§3.3) by exactly one.

A second target, `otlp_trace_decoder_fuzz.cpp`, drives `IOtlpTraceDecoder`
alone with random `DecodeLimits`, so decoder bugs are not hidden behind
validation. Both join `corpus-check.sh` and the periodic `fuzz.yml` job.

### 7.4 Concentrator unit and integration tests

With `tests/mocks/mock_otlp_trace_decoder.hpp` (returns what it is given) and
the existing fake span processor and fake exporter:

- validation: each rule in §3.4, one test per rule.
- Resource merge: each layer of §4.4 overriding the one below it, per key;
  reserved keys stripped; `unknown_service`; unknown-leaf accept and reject;
  resolver called once per leaf and again after eviction; resolver called with
  no lock held (TSAN run).
- time modes: each formula in §5, including fallback, anchor window, new boot
  id, and the `auto` / constrained config rules.
- limits: every row of §3.7 at the boundary and one past it.
- cardinality: LRU eviction order, idle timeout, Resource change re-resolution.
- sampling: `TraceIdRatio` decides the same for all spans of a trace spread
  over several payloads.
- pipeline: queue-full drops reported in `IngestResult` and in
  `GetExporterHealth()`; per-record Resource grouping in `BatchSpanProcessor`
  (one `BatchHandle` per `(Resource, scope)`).
- TSAN: concurrent `Ingest` from several threads with overlapping leaf ids.

### 7.5 End-to-end (gates 3 and 5)

`tests/integration/leaf/leaf_e2e_test.cpp`: a leaf built with the backend
under test → an in-memory byte queue standing in for the transport →
`LeafReceiver::Ingest` → a Provider exporting to a real collector. The matrix
is {upb, nanopb} × {OTLP/HTTP, OTLP/gRPC}, four runs. The collector writes with
its file exporter; the test asserts on the written spans: ids, names,
attributes, events, status, the merged Resource with no `microtel.leaf.*` keys,
and timestamps corrected per mode within a tolerance. It runs in the
collector job in `.github/workflows/interop.yml`, which already starts an
otel-collector container.

### 7.6 Footprint and closure (gate 4)

- **`leaf-cortex-m` CI job, on every PR.** Configures `cmake -S leaf` with an
  `arm-none-eabi-gcc` toolchain file, `MICROTEL_LEAF_ENCODER=nanopb`, `-Os`,
  `-ffunction-sections -fdata-sections`, for `cortex-m0plus` and `cortex-m4`.
  Links `examples/leaf/size_probe.c` (a minimal firmware that initialises a
  leaf, builds one span with one attribute, and encodes it) with
  `--gc-sections` and `nosys.specs`, and writes `arm-none-eabi-size` output for
  the leaf's own sections and the whole image to the job summary. It fails only
  if the build fails; the < 15 KB figure is a target in v1.2 and becomes a gate
  in v2.1 (ICP 0031 gate 4).
- **`leaf-aarch64` CI job.** The same with `aarch64-linux-gnu-gcc` and
  `MICROTEL_LEAF_ENCODER=upb`, against the < 30 KB target.
- **RAM.** Reported separately: `sizeof(microtel_leaf_t)`, the record buffer
  the probe needs for its span, and the upb scratch it needs. Static data
  (`.data` + `.bss`) comes from the size output.
- **Published numbers.** Each release records both backends' figures in
  `docs/bench-results/leaf-footprint.md`, and the roadmap §6 table points at
  it.
- **Closure scan.** `symbol-scan.sh` gains a leaf pass over `libmicrotel_leaf.a`
  and its backend archives. It fails on any C++ runtime symbol (`_Z*`,
  `__cxa_*`, `__gxx_personality_*`), any unprefixed `pb_*` / `upb_*` /
  `utf8_range_*` global, any global outside the `microtel_` prefix, and, for
  the nanopb build, any **undefined** reference to `malloc`, `calloc`,
  `realloc` or `free`. That last check is the mechanical form of "the nanopb
  leaf needs no heap". The existing passes gain a check that no non-leaf
  archive references `pb_*` or `microtel_pb_*` (ICP 0031 Decision 4).

### 7.7 Example (gate 6)

`examples/leaf/`:

- `udp_leaf.c`: a POSIX program that plays a leaf. It creates spans on a
  timer in the time mode given on the command line and sends each payload as
  one UDP datagram.
- `udp_concentrator.cpp`: receives datagrams, uses the sender's
  `address:port` as the leaf id, calls `Ingest`, and prints each
  `IngestResult`. It exports to the collector named by the usual
  `OTEL_EXPORTER_OTLP_ENDPOINT`.
- `microtel.toml`: a `[concentrator]` section with defaults and one
  configured leaf.
- `size_probe.c`: the firmware stub the Cortex-M job links (§7.6).
- `README.md`: how to run the pair, and how to build the leaf half for a
  microcontroller.

Built by `MICROTEL_BUILD_EXAMPLES=ON` together with `MICROTEL_BUILD_LEAF=ON`.

### 7.8 Static analysis for C

`clang-format` covers `leaf/` with the existing config. `tidy-check.sh`
extends to `leaf/` with a C profile: the `bugprone-*`, `cert-*` C checks,
`readability-function-cognitive-complexity` (15),
`readability-function-size` (nesting 3), and `hicpp-signed-bitwise`, without
the C++-only checks. The C section that ICP 0031 adds to
`docs/coding-standards.md` records the profile.

---

## §8 Follow-up ICPs and document edits

**ICPs required before the code that needs them:**

1. `Provider::GetLeafReceiver()` pure virtual (§3.2). Precedent: ICP 0012.
2. Three new `DropReason` enumerators and their payload unit (§3.3).
   Precedent: ICPs 0008 and 0011. It must pick values after the last
   enumerator at the time, taking account of ICP 0030's draft
   `SignalNotCompiled`, which currently claims a value (23) that
   `LogAttributeLimit` already holds.
3. `ISpanProcessor::OnEndWithResource` (§3.6).

These can be one ICP ("leaf receiver public and internal surface") since they
land together and have no use apart.

**Possible later ICP**, only if the §3.6 measurement calls for it: an encoder
entry point that packs several Resources into one export request.

**No ICP needed** (additive or already decided by ICP 0031):

- `include/microtel/leaf_receiver.hpp`, `SdkBuilder::WithLeafReceiver`,
  `LeafReceiverStats`.
- `IOtlpTraceDecoder`: a new internal interface, added to
  `docs/interfaces.md` §4 in the packet that introduces it, with its mock and
  fake.
- `docs/memory-model.md` §3.1: the per-call arena rule extended to the decoder.
- `CLAUDE.md` rules 12 and 13: already amended per ICP 0031 Decisions 3
  and 4.
- `docs/coding-standards.md`: the C section whose content ICP 0031
  Decision 3 fixes (ownership, init / free pairs, no VLAs or extensions,
  SonarQube limits), plus the tidy profile in §7.8. Its line 10 still says
  the leaf is v2.0 work.
- `docs/error-model.md` §3: the three rows, once the ICP above is accepted.
- `docs/configuration.md`: the `[concentrator]` table and its variables.
- `docs/interfaces.md` and `docs/architecture.md`: replace the "not realised
  yet" notes with pointers to the sections here.

---

## §9 Decisions left for the reviewer

Each has a proposal above; these are the ones where a reasonable reviewer
might choose differently.

1. **Default leaf backend** (§6.1): nanopb, no default, or upb.
2. **Public type name** (§3.1): `LeafReceiver` / `GetLeafReceiver`, or
   `Concentrator` / `GetConcentrator`, or a general `Receiver`.
3. **Three drop reasons or one** (§3.3).
4. **Attribute drops outside a span's own limits** (§3.4, §4.5): `kvlist` /
   nested-array values from a non-microtel producer, and leaf-declared
   Resource attributes over `max_leaf_resource_bytes`, are counted as
   `span_attribute_limit`, or get a fourth reason.
5. **Merge order** (§4.4): leaf defaults below the leaf's own Resource and
   per-leaf config above it; and whether the concentrator's own Resource should
   be inheritable through an opt-in key list.
6. **`device.id` floor** (§4.4): on by default, or off unless configured.
7. **Shared span queue** for leaf and in-process spans (§3.5), rather than a
   separate processor and thread.
8. **One export request per leaf per batch** in v1.2 (§3.6), with the
   multi-Resource encoder deferred to a measured ICP.
9. **Renaming nanopb's generated descriptors** (§2.5), which upb's generated
   names are not.
10. **Streaming encode** in the leaf API (§1.8), or buffer-only.
11. **C11** rather than C99 (§1.2).

## Items to verify during implementation

Assumptions this design rests on that were not checked against the pinned
sources:

- upb v29.4: `upb_Arena_Init` with a NULL `upb_alloc` never grows (§2.2);
  field-number output order (§2.3); the decoded-to-wire memory ratio behind the
  arena factor of 4 (§3.7).
- nanopb 0.4.x: descriptor field order matches tag order (§2.3);
  `FT_CALLBACK` members inside a `oneof` (§2.3 rule 6); `pb_encode_submessage`
  calling callbacks twice (§2.2); the exact global symbol list for the rename
  header (§2.5).
- `arm-none-eabi-gcc` from Ubuntu's apt is recent enough for `-std=c11` and
  Cortex-M0+ (§7.6); any of the last several releases is.

## References

- [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md): the decisions this
  document implements.
- [ICP 0032](icps/0032-release-reorder-v1.1.1.md): release renumbering; #257
  and #222 as prerequisites.
- [ICP 0012](icps/0012-provider-get-logger.md): precedent for adding a
  `Provider` method.
- [ICP 0015](icps/0015-unrepresentable-attribute-policy.md): unrepresentable
  attribute values.
- [ICP 0020](icps/0020-install-and-package-config.md) Decision 4: vendored
  symbol renaming.
- [ICP 0023](icps/0023-span-processor-scope.md): per-scope batching.
- [ICP 0030](icps/0030-compile-time-feature-selection.md): `MICROTEL_WITH_*`
  options (draft).
- `microtel-spec.md` §5.4, §5.5, §5.6, §12, §18.4.
- `microtel-roadmap.md` v1.2, v2.0, v2.1, §6.
- `docs/metrics-design.md`: structure this document follows.
- `src/common/config/table_merge.hpp`, `include/microtel/resource.hpp`
  (`Resource::Merge`): the per-key merge rule.
- `third_party/upb/upb/mem/arena.h`, `third_party/upb/upb/wire/decode.h`:
  the upb calls named here.
- nanopb documentation (reference manual, generator options): the API named in
  §2; the pinned release is chosen when vendoring.
