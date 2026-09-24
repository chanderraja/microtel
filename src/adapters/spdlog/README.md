# src/adapters/spdlog: spdlog to OTLP logs bridge

## What lives here

The build target for `microtel_spdlog_bridge` (M14 L6), an opt-in spdlog sink
that forwards an application's spdlog messages into microtel's OTLP logs
pipeline. It is not part of the core runtime. The directory holds only
`CMakeLists.txt`; the sink itself is header-only and lives in
[`include/microtel/adapters/spdlog_sink.hpp`](../../../include/microtel/adapters/spdlog_sink.hpp).

## Interfaces

On the spdlog side it implements `spdlog::sinks::base_sink<Mutex>`. On the
microtel side it consumes the public `microtel::Logger`
([`include/microtel/logger.hpp`](../../../include/microtel/logger.hpp)).

## Dependencies

The microtel public headers and `spdlog::spdlog`. Nothing in the core links this
target; applications link it explicitly alongside `microtel`.

The target is added only when `MICROTEL_USE_SPDLOG=ON` (the default). With it
OFF, the core builds with no spdlog dependency and this target does not exist.

## Tests

[`tests/unit/adapters/spdlog_sink_test.cpp`](../../../tests/unit/adapters/spdlog_sink_test.cpp).

## Usage

```cpp
auto logger = provider->GetLogger("my.app");            // microtel Logger
auto sink = std::make_shared<microtel::adapters::SpdlogSinkMt>(logger);
auto app_logger = std::make_shared<spdlog::logger>("app", sink);
app_logger->info("hello");   // → emitted as an OTLP LogRecord (Info severity)
```

## Style notes

Header-only. The level mapping is the inline free function `ToSeverityNumber`.
`SpdlogSinkMt` and `SpdlogSinkSt` follow spdlog's usual mt/st sink naming.
