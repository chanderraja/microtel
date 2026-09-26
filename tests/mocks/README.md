# `tests/mocks/`

Dumb mocks for the locked interfaces in
[`docs/interfaces.md`](../../docs/interfaces.md) §4.

## What lives here

One mock header per internal interface. The track column records which
M2 work track introduced the mock; mocks added later for metrics and
logs have none.

| Mock | Interface | Owning track |
|---|---|---|
| `mock_transport.hpp`             | `internal::ITransport`           | D |
| `mock_otlp_encoder.hpp`          | `internal::IOtlpEncoder`         | F |
| `mock_wire_codec.hpp`            | `internal::IWireCodec`           | B / C |
| `mock_exporter.hpp`              | `internal::IExporter`            | A → B/C |
| `mock_sampler.hpp`               | `internal::ISampler`             | A |
| `mock_span_processor.hpp`        | `internal::ISpanProcessor`       | A |
| `mock_metric_encoder.hpp`        | `internal::IMetricEncoder`       | |
| `mock_metric_exporter.hpp`       | `internal::IMetricExporter`      | |
| `mock_log_encoder.hpp`           | `internal::ILogEncoder`          | |
| `mock_log_exporter.hpp`          | `internal::ILogExporter`         | |
| `mock_log_record_processor.hpp`  | `internal::ILogRecordProcessor`  | |
| `mock_otlp_trace_decoder.hpp`    | `internal::IOtlpTraceDecoder`    | |

Some interfaces have a fake under [`tests/fakes/`](../fakes/) instead of
a mock here, because the test needs logic: clocks, reactors, the
diagnostics sink as a backing store. The mock-or-fake choice for each
interface is recorded in `docs/interfaces.md` §4.

## The dumb-mock contract (LOCKED)

From `CLAUDE.md` rule 4 and spec §14.2:

- It returns what it's configured to return. No logic, and no state
  computed from its inputs.
- It records calls (order, arguments, count) for the test to assert on.
- Copyable or move-only is the author's choice. Default to copyable,
  which is easier to use in test fixtures.
- It is never a `friend` of the production type. If the mock needs
  internals, the production type's surface is wrong.
- It never behaves differently depending on argument values. A test
  that needs "respond differently to the second call than the first"
  wants a scripted fake; promote it to `tests/fakes/`.

## Naming

`Mock<InterfaceName>`, e.g. `MockTransport`, `MockOtlpEncoder`. Mocks
live in the `microtel::testing` namespace, never in a production
namespace.
