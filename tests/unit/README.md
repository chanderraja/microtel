# `tests/unit/`

gtest unit tests, mirroring [`src/`](../../src/) one-to-one. One file
per non-trivial type or behaviour.

## Layout

```
tests/unit/
├── api/               mirrors src/api/
├── sdk/               mirrors src/sdk/
├── exporter/          mirrors src/exporter/
├── wire/
│   ├── encoder/       mirrors src/wire/encoder/
│   ├── http/          mirrors src/wire/http/
│   └── grpc/          mirrors src/wire/grpc/
├── transport/         mirrors src/transport/
└── common/
    ├── config/        mirrors src/common/config/
    └── raii/          mirrors src/common/raii/
```

## Bar

Per spec §14.2:

- **< 1 ms per test.** Unit tests are tight loops over mocked
  dependencies; if a test takes longer, it probably belongs in
  [`integration/`](../integration/).
- **No I/O.** No real sockets, no real disk reads (small in-memory
  fixtures only). Use [`fakes/`](../fakes/) for components that would
  otherwise touch I/O.
- **No threading on the test thread.** If the production code under
  test spins up a worker, the test drives it via the synchronous-mode
  `_test_only_drain_synchronously()` seam (per `interfaces.md` §4.6
  and `threading-model.md` §9.3).
- **Aggregate coverage** ≥ 90% line / ≥ 85% branch on SDK + encoder
  code per spec §14.2.

## Naming

- File: snake_case mirroring the source — `batch_span_processor.cpp` →
  `tests/unit/sdk/batch_span_processor_test.cpp`.
- Test fixture: PascalCase per the standard gtest idiom —
  `class BatchSpanProcessorTest : public ::testing::Test { ... };`.
- Test name: PascalCase verb phrase — `TEST_F(BatchSpanProcessorTest,
  DropsRecordWhenQueueIsFull)`.
