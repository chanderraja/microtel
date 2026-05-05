# `tests/mocks/`

**Dumb** mocks per locked interface in [`docs/interfaces.md`](../../docs/interfaces.md) §4.

## What lives here

One mock header per internal interface:

| Mock | Interface | Owning track |
|---|---|---|
| `mock_transport.hpp`         | `internal::ITransport`       | D |
| `mock_otlp_encoder.hpp`      | `internal::IOtlpEncoder`     | F |
| `mock_wire_codec.hpp`        | `internal::IWireCodec`       | B / C |
| `mock_exporter.hpp`          | `internal::IExporter`        | A → B/C |
| `mock_sampler.hpp`           | `internal::ISampler`         | A |
| `mock_span_processor.hpp`    | `internal::ISpanProcessor`   | A |

Some interfaces don't have a mock here — they have a fake under
[`tests/fakes/`](../fakes/) instead because logic is required (clocks,
reactors, the diagnostics sink as a backing store). The mock-vs-fake
choice per interface is recorded in `docs/interfaces.md` §4.

## Bar — the dumb-mock contract (LOCKED)

Per `CLAUDE.md` rule 4 and spec §14.2:

- **Returns what it's configured to return.** No logic, no state
  computed from inputs.
- **Records calls** for the test to assert on. Order, arguments,
  count.
- **Move-only or copyable** — author's choice; default to copyable for
  ergonomics in test fixtures.
- **No `friend` of the production type.** If the mock needs internals,
  the production type's surface is wrong.
- **No conditional behaviour by argument value.** If the test needs
  "respond differently to the second call than the first," that's
  scripted-fake territory; promote to `tests/fakes/`.

## Naming

`Mock<InterfaceName>` — `MockTransport`, `MockOtlpEncoder`. Lives in
the `microtel::testing` namespace, not the production namespace.
