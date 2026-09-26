# src/adapters/log4cxx: log4cxx to OTLP logs bridge

## What lives here

The build target for `microtel_log4cxx_bridge` (issue #304), an opt-in log4cxx
appender that forwards an application's log4cxx output into microtel's OTLP
logs pipeline. It is not part of the core runtime. The directory holds only
`CMakeLists.txt`; the appender itself is header-only and lives in
[`include/microtel/adapters/log4cxx_appender.hpp`](../../../include/microtel/adapters/log4cxx_appender.hpp).

## Interfaces

On the log4cxx side it derives from `log4cxx::AppenderSkeleton` (implementing
`append`, `close` and `requiresLayout`). On the microtel side it consumes the
public `microtel::Logger`
([`include/microtel/logger.hpp`](../../../include/microtel/logger.hpp)).

## Dependencies

The microtel public headers and log4cxx, found with
`find_package(log4cxx CONFIG)` (imported target `log4cxx`). **Supported:
log4cxx 1.1 and later** — the `std::shared_ptr`-based 1.x API; built and tested
against 1.1 (Ubuntu 24.04) and 1.8 (Fedora 44). The 0.x series is not
supported.

The target is added only when `MICROTEL_BUILD_LOG4CXX_BRIDGE=ON` (default
`OFF`). Nothing in the core links it, log4cxx is not a member of microtel's
dependency closure, and the header installs with the rest of
`include/microtel/` but no target is exported (ICP 0020 Decision 3).
`ci/scripts/symbol-scan.sh` fails the build if a shipped archive ever
references a log4cxx symbol.

## Mapping

| `LogRecord` field    | from                                              |
|----------------------|---------------------------------------------------|
| `severity_number`    | `TRACE`→Trace, `DEBUG`→Debug, `INFO`→Info, `WARN`→Warn, `ERROR`→Error, `FATAL`→Fatal; custom levels round down to the nearest standard one |
| `severity_text`      | the level name                                    |
| `body`               | the rendered message                              |
| `time`               | the event timestamp (µs)                          |
| `code.file.path`     | the call site's file, when location is known      |
| `code.line.number`   | the call site's line, when location is known      |
| `code.function.name` | `ns::Class::method`, when log4cxx can parse it    |
| MDC entries          | one string attribute each, under the MDC key      |

The NDC and the log4cxx logger name are not mapped: the instrumentation scope
is the microtel `Logger` the appender was built with, so attach separately
scoped appenders where the split matters. Trace correlation comes from
`Logger::Emit`, which log4cxx calls on the logging thread — do not put an
`AsyncAppender` in front of this one.

## Tests

[`tests/unit/adapters/log4cxx_appender_test.cpp`](../../../tests/unit/adapters/log4cxx_appender_test.cpp):
level table, message/location/time/MDC conversion, correlation inside an
active span, post-shutdown drop accounting, and the
`addAppender`/`removeAppender`/`close` lifecycle.

## Usage

```cpp
auto logger = provider->GetLogger("my.app");                // microtel Logger
auto appender = std::make_shared<microtel::adapters::Log4cxxAppender>(logger);
log4cxx::Logger::getRootLogger()->addAppender(appender);
LOG4CXX_INFO(log4cxx::Logger::getLogger("app"), "hello");   // → OTLP LogRecord
log4cxx::Logger::getRootLogger()->removeAppender(appender); // before provider teardown
```

## Style notes

Header-only. The level mapping is the inline free function `FromLog4cxxLevel`
over `Level::toInt()`. The class is not registered with log4cxx's class
registry (no `DECLARE_LOG4CXX_OBJECT`): that would need out-of-line
definitions, which a header-only bridge cannot provide, so it can only be
attached programmatically, not named in a log4cxx configuration file.
