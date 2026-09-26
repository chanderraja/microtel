# src/adapters/glog: glog to OTLP logs bridge

## What lives here

The build target for `microtel_glog_bridge` (issue #304), an opt-in glog
`LogSink` that forwards an application's glog output into microtel's OTLP logs
pipeline. It is not part of the core runtime. The directory holds only
`CMakeLists.txt`; the sink itself is header-only and lives in
[`include/microtel/adapters/glog_sink.hpp`](../../../include/microtel/adapters/glog_sink.hpp).

## Interfaces

On the glog side it implements `google::LogSink` (the `LogMessageTime` overload
of `send`). On the microtel side it consumes the public `microtel::Logger`
([`include/microtel/logger.hpp`](../../../include/microtel/logger.hpp)).

## Dependencies

The microtel public headers and `glog::glog`, found with
`find_package(glog CONFIG)`. **Supported: glog 0.6.x and 0.7.x.** The two
differ in `LogMessageTime` (0.7 has `when()`, 0.6 only `timestamp()` +
`usec()`) and in how `LogSeverity` is declared (a global `int` in 0.6, an enum
in `google::` in 0.7); the header handles both. Older glog lacks the
`LogMessageTime` overload and will not compile.

The target is added only when `MICROTEL_BUILD_GLOG_BRIDGE=ON` (default `OFF`).
Nothing in the core links it, glog is not a member of microtel's dependency
closure, and the header installs with the rest of `include/microtel/` but no
target is exported (ICP 0020 Decision 3). `ci/scripts/symbol-scan.sh` fails the
build if a shipped archive ever references a glog or gflags symbol.

## Mapping

| `LogRecord` field    | from                                     |
|----------------------|------------------------------------------|
| `severity_number`    | `INFO`→Info, `WARNING`→Warn, `ERROR`→Error, `FATAL`→Fatal |
| `severity_text`      | glog's severity name                     |
| `body`               | the message text, without glog's prefix  |
| `time`               | the glog timestamp (µs)                  |
| `code.file.path`     | the call site's `__FILE__`               |
| `code.line.number`   | the call site's line                     |

`VLOG(n)` arrives as `INFO`: glog does not pass the verbosity to sinks. glog
does not report the calling function, so `code.function.name` is not set.
Trace correlation comes from `Logger::Emit`, which glog calls on the logging
thread.

## Tests

[`tests/unit/adapters/glog_sink_test.cpp`](../../../tests/unit/adapters/glog_sink_test.cpp):
severity table, message/location/time conversion, `VLOG`, correlation inside an
active span, post-shutdown drop accounting, and register/unregister lifecycle.

## Usage

```cpp
auto logger = provider->GetLogger("my.app");             // microtel Logger
{
    microtel::adapters::GlogSink sink{logger};           // registers with glog
    LOG(INFO) << "hello";   // → emitted as an OTLP LogRecord (Info severity)
}                                                         // unregisters here
```

Destroy the sink before the provider that issued its `Logger`. `LOG(FATAL)` is
emitted, but glog aborts straight after the sinks run, so a batched pipeline
will not export it.

## Style notes

Header-only. The severity mapping is the inline free function
`FromGlogSeverity`; `GlogSeverity` names glog's severity type portably across
0.6 and 0.7. The sink is neither copyable nor movable: glog registers it by
address.
