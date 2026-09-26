# ICP 0033: a `ShimOptions::attribute_value_length_limit` for the otel-cpp shim

**Status:** Draft.
**Affected interfaces / docs:** the otel-cpp shim only, all under
`src/adapters/otelcpp/`:
- new `shim_options.hpp` (`ShimOptions`)
- `shim_diagnostics.hpp` (two new `ShimDiagnostics` fields)
- `attribute_conversion.hpp` (`ConvertAttributeValue`, `ConvertKeyValues`)
- `tracer_shim.hpp`, `span_shim.hpp`, `logger_shim.hpp`, `log_record_shim.hpp`,
  `meter_shim.hpp`, `metrics_instruments_shim.hpp`, `global_registration.hpp`
  (the four entry points gain an options parameter; the shim classes carry it)
- `src/adapters/otelcpp/README.md`

Amends [ICP 0015](0015-unrepresentable-attribute-policy.md) (a byte-span
attribute can now be omitted, not only degraded) and
[ICP 0016](0016-adapter-drop-accounting.md) (a third shim-local counter and a
high-water mark). No
microtel core header, locked interface, or wire change.
**Affected tracks:** the otel-cpp shim (experimental). None of microtel core.

## Summary

Give the shim's entry points an optional `ShimOptions` carrying the attribute
value length limit, and omit — and count — any byte-span attribute whose hex
rendering would exceed it, so the SDK never cuts a hex string mid-byte.

## Motivation

Issue #238. The shim renders `span<const uint8_t>` as lowercase hex, two
characters per byte (`RenderBytesAsHex`). Since #181, `SdkSpan::SetAttribute`
truncates any string longer than `attribute_value_length_limit`. Hex is ASCII,
so the cut is an exact byte cut; an odd cut leaves half a byte, and the string
still looks like hex but decodes to a final byte the application never set.
ICP 0015's addendum (point 1) predicted this.

The SDK cannot fix it, because `SetAttribute` sees an ordinary string. The
shim has to, and today it cannot: it is built from a
`shared_ptr<microtel::Provider>` alone, `Provider`, `Tracer` and `Span` expose
no limits, and the limit exists only in `SpanLimitOptions`, set through
`SdkBuilder::WithSpanLimits` (there is no env or TOML key for it).

## Proposed change

### 1. `ShimOptions`

New header `src/adapters/otelcpp/shim_options.hpp`:

```cpp
namespace microtel::adapters::otelcpp
{

/// @brief Configuration for the otel-cpp shim.
struct ShimOptions
{
    /// Largest hex rendering, in bytes, of a `span<const uint8_t>` attribute
    /// the shim forwards. A byte span of n bytes renders as 2n characters;
    /// if 2n exceeds this, the attribute is omitted and counted in
    /// `ShimDiagnostics::oversized_byte_attributes_omitted`. Set it to the
    /// same value as `SpanLimitOptions::attribute_value_length_limit`.
    /// `std::nullopt`: no shim limit (the behaviour before ICP 0033).
    std::optional<std::uint32_t> attribute_value_length_limit = 4096;
};

}  // namespace microtel::adapters::otelcpp
```

The field has the same name and element type as the SDK's, so "set them to the
same value" needs no explanation. The default is the SDK's default, so an
application that never calls `WithSpanLimits` is correct with no change.

Unlimited is `std::nullopt` rather than 0 because the SDK treats 0 as a real
limit (every string truncated to empty). If 0 meant unlimited in the shim, the
same number would mean opposite things in the two settings and "set them
equal" would be wrong at exactly that value. With `std::optional`, a shim limit
of 0 omits every byte span, which is the consistent counterpart.

### 2. Entry points

Each gains a defaulted parameter, so existing calls still compile:

```cpp
[[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>
MakeTracerProvider(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {});

[[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>
MakeMeterProvider(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {});

[[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>
MakeLoggerProvider(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {});

void RegisterGlobally(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {});
```

`RegisterGlobally` passes the same options to all three. The provider shims
store a copy and hand it down to the tracer, span, logger, log-record, meter
and instrument shims through their constructors.

### 3. Conversion

```cpp
/// std::nullopt only for a byte span whose hex rendering exceeds
/// options.attribute_value_length_limit; that case is counted before return.
[[nodiscard]] std::optional<microtel::AttributeValue> ConvertAttributeValue(
    const otel_common::AttributeValue& value, const ShimOptions& options) noexcept;

/// Omitted values are left out of the result.
[[nodiscard]] std::vector<microtel::KeyValue> ConvertKeyValues(
    const otel_common::KeyValueIterable& attributes, const ShimOptions& options);
```

The check is on `2 * bytes.size()` before rendering, so an oversized span is
never allocated. Every caller drops a `std::nullopt` result instead of
forwarding it. That covers every path that converts attributes:
`SpanShim::SetAttribute`, `AddEvent` attributes, `StartSpan`'s initial
attributes and link attributes, `LogRecordShim::SetAttribute`, and the metric
instrument and observer attribute sets.

The log **body** is not an attribute and no attribute limit applies to it. It
is converted with no shim limit, as today.

### 4. Diagnostics

`ShimDiagnostics` gains two fields:

```cpp
/// A `span<const uint8_t>` attribute was omitted because its hex rendering
/// would exceed `ShimOptions::attribute_value_length_limit`. One per omitted
/// value.
std::uint64_t oversized_byte_attributes_omitted = 0;

/// Largest size, in bytes of the application's byte span (not hex
/// characters), of any attribute counted in `oversized_byte_attributes_omitted`.
/// 0 if none has been omitted. Monotonic: it only ever increases.
std::uint64_t largest_omitted_byte_attribute = 0;
```

The counter is a relaxed `fetch_add`, like the existing two. The high-water
mark is a relaxed compare-exchange loop that stores n only if n is larger
(`std::atomic::fetch_max` is C++26). The two are updated independently, so a
snapshot taken during an omission can show one updated and not yet the other;
they are diagnostics, not a transaction.

The high-water mark tells an operator which fix applies. With limit S, the
ratio 2 × `largest_omitted_byte_attribute` / S near 1 means the limit is a
little low; a ratio of 100 means blobs are being put in attributes. The field
is in bytes because that is the size the application chose and can compare
with its own data. `ShimDiagnostics` is a plain aggregate of counters returned
by value, so adding fields is source-compatible and costs one more static
atomic.

The counter is named for what happened (an oversized byte span) rather than
`unrepresentable_attributes_omitted`, because the value is representable, just
not within the limit.

### 5. When the two limits differ

Call the shim limit S and the SDK limit L. S governs only the hex rendering of
byte spans. L applies to every string attribute on a span, including ordinary
strings the shim passes through untouched. Setting S = L makes byte spans safe;
it does not make the shim enforce L. A plain string attribute longer than L is
still truncated by the SDK, correctly, at a UTF-8 boundary, and counted under
`AttributeValueTruncated`. That is ordinary truncation, not corruption.

The shim only ever forwards hex of even length 2n with 2n ≤ S. The SDK cuts a
string only when its length exceeds L. So:

- **No byte span is ever cut by the SDK if and only if 2·⌊S/2⌋ ≤ L.** S ≤ L is
  the practical rule; S = L + 1 with L even also qualifies.
- **If 2·⌊S/2⌋ > L,** a byte span with L < 2n ≤ S reaches the SDK and is cut to
  exactly L characters. If L is odd, that is the #238 corruption. If L is even,
  the result is a whole-byte prefix counted under `AttributeValueTruncated`,
  like any truncated string, but a reader cannot tell it from a shorter byte
  span.
- **If S < L,** byte spans with S < 2n ≤ L are omitted when the SDK would have
  kept them. That loses data but never corrupts it, and every omission is
  counted.

This applies to span, event and link attributes only. The SDK enforces no
value length on log or metric attributes, so on those paths the shim limit is
the only cap. It is applied there anyway so byte spans follow one rule on every
signal, and so the v1.2 logs work cannot reintroduce #238 by adding a log value
limit.

**Metrics: a known correctness hazard.** On a metric, the attribute set is the
time series' identity. Omitting one attribute sends the measurement to a
*different series*: measurements that should aggregate together are split, or
merged into the series of a different logical entity that genuinely lacks that
key. The effect depends on the data: the same instrument with the same keys
lands in a different series depending on one attribute's byte length.

The alternative is to omit the whole measurement, which keeps every series
correct but loses the value. This ICP ships attribute omission and documents
the hazard in the shim README. Losing measurements outright is worse for most
users than a misattributed one: a counter that silently undercounts is harder
to notice than a stray series, and every omission here is counted. It also
keeps one rule across signals. Byte-span attributes on metrics are rare, since
no semantic convention uses them and a high-cardinality blob in a metric key is
already a mistake. Revisit if that turns out to be wrong.

### 6. Calling `RegisterGlobally` more than once

The shim keeps no process-wide state of its own apart from the
`ShimDiagnostics` counters. `RegisterGlobally` builds three new provider shims
and hands them to otel-cpp's `Provider::Set*Provider`, which replaces
otel-cpp's global. Options are copied into each provider shim when it is
constructed and never change afterwards, and every tracer, span, meter,
instrument, logger and log record copies them from the object that created it.
So:

- **The last call wins** for anything obtained through otel-cpp's globals
  after it.
- **Objects already handed out keep the options they were created with**, for
  their lifetime. A tracer obtained under the first call keeps the first limit;
  spans started from it do too. Instrumentation that caches a tracer or meter
  at startup does not see a later change.
- The same holds for providers built directly with `Make*Provider`: each has
  its own options, and several can coexist with different limits.

This follows from how the code already works (every shim class holds its
inputs by value, and otel-cpp owns the global slot). A process-wide shim
option read on every conversion would let a second call reach objects already
handed out, but it would need an atomic read on every attribute conversion and
could not express two providers with different limits. It is not proposed.

### 7. Diagnostics are process-wide

`ShimDiagnostics` is backed by function-local static atomics in a header-only
library, so there is one set per process. It is shared across every provider,
every option set and every `RegisterGlobally` call, and it has no reset. An
operator running two option sets sees their omissions added together, and the
high-water mark is the largest across both.

Tests that exercise more than one option set, or share a process with other
shim tests, see interleaved counts. They should take a snapshot before and
compare after, as the existing shim diagnostics tests do: assert on the
difference for `oversized_byte_attributes_omitted`, and on
`after == max(before, n)` for `largest_omitted_byte_attribute`, since a
maximum cannot be diffed. No reset function is proposed; it would be a
test-only public entry point that could also clear an operator's counts.

## Migration

- Applications that use the SDK's default limit need do nothing. Byte spans
  over 2048 bytes are now omitted and counted instead of silently cut.
- Applications that call `WithSpanLimits` with a non-default
  `attribute_value_length_limit` should pass the same value in `ShimOptions`.
  The shim README gets that instruction and the rule in §5.
- Code that calls `ConvertAttributeValue` or `ConvertKeyValues` directly (the
  shim's own tests) must pass options and handle `std::nullopt`. That reverses
  ICP 0015's migration note that `std::optional` left the signature. The
  otherwise-total conversion from ICP 0015 is unchanged.
- The implementing PR adds the truncation test #238 asks for, beside
  `LargeByteSpanEncodesCorrectlyAtScale`.

## Rationale & alternatives

- **A (chosen): shim-local option.** It needs no change to microtel core. The
  cost is that S and L are set in two places and can drift; §5 states exactly
  what drift does, and the default makes the common case correct.
- **B: `Provider::GetSpanLimits()`.** The shim would read L directly and could
  never drift. Rejected because it adds a virtual to the locked `Provider`
  interface, a permanent core API change made for an experimental,
  source-only adapter. ICPs 0015 and 0016 turned down core growth for the shim
  on the same grounds. If a second adapter needs the limits, B becomes worth
  revisiting.
- **C: native bytes in `microtel::AttributeValue`.** OTLP has `bytes_value`,
  so this is the right long-term model, and the SDK would not truncate bytes as
  text. Rejected for now: it changes a locked public type, needs encoder and
  wire-conformance work, and changes the exported type for anyone relying on
  today's hex strings (ICP 0015 addendum, point 5). Nothing here prevents it
  later.
- **Round the rendering down to an even length instead of omitting.** Issue
  #238's first option. Rejected because the output would look like a complete
  byte span the application never set, which ICP 0015's addendum (point 4)
  treats as disqualifying. Omission with a counter preserves ICP 0015's
  preserve-or-omit rule.
- **On metrics, omit the whole measurement instead of the attribute.** It keeps
  every series correct, but the value is lost. Rejected for now; see §5.
