# `src/wire/custom/`

## What lives here

The third implementation of `IWireCodec`, for a Provider built with
`SdkBuilder::WithExportTransport`
([ICP 0036](../../../docs/icps/0036-custom-export-transport.md)). Built as
`microtel_custom_wire`. Everything is in
[`export_transport_codec.hpp`](export_transport_codec.hpp):

- `ExportTransportChannel`, one per Provider. Owns the application's
  `microtel::ExportTransport`, gives each `Send` its deadline (now plus
  `per_export`, clamped to the shutdown deadline), contains a throw from
  `Send`, calls `Cancel` at most once when a shutdown wait expires with a
  `Send` in flight, and keeps the "sends are succeeding" connection state.
- `ExportTransportCodec`, one per enabled signal. Refuses an empty encoding
  without calling the transport, logs a rate-limited `Warn` for a trace
  request above `max_request_bytes`, and maps `SendResult` to `WireResult`
  (`docs/error-model.md` §7.3).

There is no HTTP/2 here: no framing, headers, compression or `ITransport`.
The bytes handed over are the uncompressed encoding.

## What it implements

- `internal::IWireCodec`
  ([`include/microtel/internal/wire_codec.hpp`](../../../include/microtel/internal/wire_codec.hpp)),
  with the default `SendAll` loop.
- It calls the public `microtel::ExportTransport`
  ([`include/microtel/export_transport.hpp`](../../../include/microtel/export_transport.hpp)).

## Dependencies and test doubles

- `EncodedPayload` (input only).
- `ExportTransport`: `mock_export_transport.hpp` in
  [`tests/mocks/`](../../../tests/mocks/) and the scripted, optionally
  blocking `fake_export_transport.hpp` in [`tests/fakes/`](../../../tests/fakes/).
- The optional `ISteadyClock`: `fake_steady_clock.hpp`.
- `microtel_common` for the internal log.

## Test entry points

- [`tests/unit/wire/custom/export_transport_codec_test.cpp`](../../../tests/unit/wire/custom/export_transport_codec_test.cpp):
  the mapping, exceptions, deadlines, `Cancel`, the state, the Warn.
- [`tests/unit/sdk/export_transport_builder_test.cpp`](../../../tests/unit/sdk/export_transport_builder_test.cpp):
  through a real Provider, including the TSAN three-signal test.

## Style notes

- `ExportTransportChannel`'s mutex is a leaf lock, and the application's
  `Send` and `Cancel` are never called while it is held.
- The one bare `catch (...)` is the documented exception to the rule, as in
  `CallbackAuthProvider::InvokeCallback`: a non-`std` throw reaching the
  `noexcept` exporter worker would terminate the process.
