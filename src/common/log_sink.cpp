// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Implementation of microtel::SetLogSink / ResetLogSink (declared in
// include/microtel/log_sink.hpp) and of the internal log entry point and its
// minimum-level filter (declared in src/common/internal_log.hpp).
//
// Stores the active sink behind a mutex so the (rare) SetLogSink call
// races safely with concurrent log emissions on internal threads. The
// internal log entry point (LogImpl below) is what production code calls;
// rate-limiting per docs/error-model.md §9.2 is still unbuilt.
//
// There is no spdlog route here, now or later. Issue #190 asked what happens
// to libmicrotel_common.a's link closure when the internal log route finally
// does something; ICP 0026 §6 takes option (1) — the public LogSink hook only,
// with spdlog staying in the consumer's own build behind
// microtel_spdlog_bridge. So this file's default route is stderr in every
// build configuration, and the archive never carries an undefined spdlog
// reference for a consumer of the installed package to resolve.

#include "microtel/log_sink.hpp"

#include "common/internal_log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel
{

namespace
{

/// @brief Active sink + mutex protecting it.
///
/// The mutex is held only briefly: SetLogSink takes it to swap in a new
/// callable; LogImpl takes it to copy the callable out before invoking,
/// so the user's lambda is not invoked under our lock (per
/// docs/error-model.md §9.3 — "the sink is invoked under no microtel-held
/// lock").
struct SinkState
{
    std::mutex mu;
    LogSink sink;  // empty default; meaning "use default fallback"
};

SinkState& State() noexcept
{
    static SinkState s_state;
    return s_state;
}

/// @brief Minimum severity `LogImpl` emits. ICP 0026 §6, issue #190.
///
/// An atomic rather than a field of `SinkState`: `LogImpl` reads it *before*
/// the sink mutex, so a record below the threshold costs one relaxed load and
/// takes no lock at all. `std::atomic<LogLevel>` is constant-initialised, so
/// there is no static-initialisation-order hazard and no `State()`-style
/// accessor is needed.
///
/// `Info` is the shipped default and changes nothing observable: all three
/// production `LogImpl` call sites emit at `Warn`.
// A process-global knob by design — the internal log path is a free function
// with no provider in scope (ICP 0026 §6).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<LogLevel> g_min_level{LogLevel::Info};

/// The highest declared enumerator, for validating a cast-in value.
constexpr auto kMaxLogLevel = static_cast<std::uint8_t>(LogLevel::Error);

const char* LevelTag(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "?";
}

}  // namespace

void SetLogSink(LogSink sink) noexcept
{
    auto& st = State();
    const std::scoped_lock lock(st.mu);
    st.sink = std::move(sink);
}

void ResetLogSink() noexcept
{
    auto& st = State();
    const std::scoped_lock lock(st.mu);
    st.sink = {};
}

namespace internal
{

bool SetMinLogLevel(LogLevel level) noexcept
{
    if (static_cast<std::uint8_t>(level) > kMaxLogLevel)
    {
        return false;
    }
    g_min_level.store(level, std::memory_order_relaxed);
    return true;
}

LogLevel MinLogLevel() noexcept
{
    return g_min_level.load(std::memory_order_relaxed);
}

/// @brief Internal log entry point. Production code calls this; the
/// public API is `microtel::SetLogSink` for redirection, and
/// `microtel::Provider::SetLogLevel` (via `SetMinLogLevel`) for filtering.
///
/// Records below the minimum level are dropped here, before the sink mutex is
/// touched — one relaxed atomic load on the way out.
///
/// If a sink is installed, it's invoked with no microtel-held lock; the
/// callable's thread-safety is the application's responsibility (LOCKED
/// per docs/error-model.md §9.3).
///
/// If no sink is installed the record goes to stderr, in every build
/// configuration. `MICROTEL_USE_SPDLOG` does not change that and never will:
/// see the file header and ICP 0026 §6.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LogImpl(LogLevel level, std::string_view message) noexcept
{
    if (level < g_min_level.load(std::memory_order_relaxed))
    {
        return;
    }

    LogSink local_copy;
    {
        auto& st = State();
        const std::scoped_lock lock(st.mu);
        local_copy = st.sink;  // copy the std::function out
    }

    if (local_copy)
    {
        // Application sink. Best-effort: if it throws, swallow per the
        // contract in error-model.md §9.3 (a sink must not destabilise
        // microtel by throwing).
        try
        {
            local_copy(level, message);
        }
        // NOLINTNEXTLINE(bugprone-empty-catch)
        catch (...)
        {
            // Intentionally empty — sinks must not throw per error-model.md §9.3.
        }
        return;
    }

    // No sink installed. Default fallback: stderr.
    // Format: "[microtel level] message\n"
    (void)std::fprintf(stderr,
                       "[microtel %s] %.*s\n",
                       LevelTag(level),
                       static_cast<int>(message.size()),
                       message.data());
}

}  // namespace internal

}  // namespace microtel
