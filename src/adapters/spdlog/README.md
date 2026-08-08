# src/adapters/spdlog — spdlog → OTLP logs bridge

**What lives here.** The `microtel_spdlog_bridge` adapter (M14 L6): an opt-in
`spdlog` sink that forwards an application's spdlog log messages into microtel's
OTLP logs pipeline. It is **not** part of the core runtime.

**Interfaces it implements.** `spdlog::sinks::base_sink<Mutex>` on the spdlog
side; consumes the public `microtel::Logger` (`include/microtel/logger.hpp`) on
the microtel side. The public header is
[`include/microtel/adapters/spdlog_sink.hpp`](../../../include/microtel/adapters/spdlog_sink.hpp).

**Depends on.** The microtel public headers and `spdlog::spdlog`. Nothing in the
core links this target; applications link it explicitly alongside `microtel`.

**Build gating.** Added only when `MICROTEL_USE_SPDLOG=ON` (the default). When
OFF, the core builds with zero spdlog dependency and this target does not exist.

**Test entry point.**
[`tests/unit/adapters/spdlog_sink_test.cpp`](../../../tests/unit/adapters/spdlog_sink_test.cpp).

**Usage.**

```cpp
auto logger = provider->GetLogger("my.app");            // microtel Logger
auto sink = std::make_shared<microtel::adapters::SpdlogSinkMt>(logger);
auto app_logger = std::make_shared<spdlog::logger>("app", sink);
app_logger->info("hello");   // → emitted as an OTLP LogRecord (Info severity)
```

**Style notes.** Header-only; level mapping is a `constexpr`-friendly free
function. `SpdlogSinkMt` / `SpdlogSinkSt` follow the standard spdlog mt/st sink
naming.
