# `tests/unit/`

gtest unit tests, mirroring [`src/`](../../src/). One file per
non-trivial type or behaviour.

## Layout

```
tests/unit/
├── api/               mirrors src/api/
├── sdk/               mirrors src/sdk/
├── exporter/          mirrors src/exporter/
├── wire/              mirrors src/wire/ (encoder, gzip and response tests sit at the top level)
│   ├── http/          mirrors src/wire/http/
│   └── grpc/          mirrors src/wire/grpc/
├── transport/         mirrors src/transport/
├── common/
│   ├── auth/          auth providers
│   ├── config/        mirrors src/common/config/
│   └── raii/          mirrors src/common/raii/
├── adapters/          mirrors src/adapters/ (built only with MICROTEL_USE_SPDLOG;
│                      the otelcpp tests also need MICROTEL_BUILD_OTELCPP_SHIM)
├── preflight/         the microtel-preflight CLI (tools/preflight/)
└── sugar/             the header-only sugar layer (ICP 0028); no src/ counterpart
```

At the top level, the `*_smoke_test.cpp` files check that the headers,
mocks and fakes compile and link inside a gtest translation unit, and
`log_sink_test.cpp` covers `src/common/log_sink.cpp`.

## Rules

From spec §14.2:

- Under 1 ms per test. Unit tests are tight loops over mocked
  dependencies. A test that takes longer probably belongs in
  [`integration/`](../integration/).
- No I/O: no real sockets, and no disk reads beyond small in-memory
  fixtures. Use the [`fakes/`](../fakes/) for components that would
  otherwise touch I/O.
- No threads started by the test itself. Where the code under test owns
  a worker thread (the batch processors, the exporter), the test
  synchronises with it through the public `ForceFlush` / `Shutdown`
  contract. The synchronous `_test_only_drain_synchronously()` seam
  described in `threading-model.md` §9.3 was never added.
- Aggregate coverage of at least 90% line and 85% branch on SDK and
  encoder code (spec §14.2).

## Naming

- File: snake_case, mirroring the source, e.g. `batch_span_processor.cpp`
  → `tests/unit/sdk/batch_span_processor_test.cpp`.
- Fixture: PascalCase, following the usual gtest idiom, e.g.
  `class BatchSpanProcessorTest : public ::testing::Test { ... };`.
- Test name: a PascalCase verb phrase, e.g. `TEST_F(BatchSpanProcessorTest,
  DropsRecordWhenQueueIsFull)`.
