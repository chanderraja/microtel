// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Implementation of microtel::SetLogSink / ResetLogSink (declared in
// include/microtel/log_sink.hpp).
//
// Stores the active sink behind a mutex so the (rare) SetLogSink call
// races safely with concurrent log emissions on internal threads. The
// internal log entry point (LogImpl below) is what production code calls;
// M3+ adds rate-limiting per docs/error-model.md §9.2 and the spdlog
// route under MICROTEL_USE_SPDLOG.

#include "microtel/log_sink.hpp"

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
    static SinkState s;
    return s;
}

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
    const std::lock_guard lock(st.mu);
    st.sink = std::move(sink);
}

void ResetLogSink() noexcept
{
    auto& st = State();
    const std::lock_guard lock(st.mu);
    st.sink = {};
}

namespace internal
{

/// @brief Internal log entry point. Production code calls this; the
/// public API is `microtel::SetLogSink` for redirection.
///
/// If a sink is installed, it's invoked with no microtel-held lock; the
/// callable's thread-safety is the application's responsibility (LOCKED
/// per docs/error-model.md §9.3).
///
/// If no sink is installed, the default route depends on the build:
/// - MICROTEL_USE_SPDLOG=ON → spdlog (M3+; today: stderr).
/// - MICROTEL_USE_SPDLOG=OFF → minimal stderr formatter.
///
/// M2 scope: stderr in both modes. M3 wires the spdlog route.
void LogImpl(LogLevel level, std::string_view message) noexcept
{
    LogSink local_copy;
    {
        auto& st = State();
        const std::lock_guard lock(st.mu);
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
        catch (...)
        {
            // Drop the exception silently.
        }
        return;
    }

    // No sink installed. Default fallback: stderr.
    // Format: "[microtel level] message\n"
    std::fprintf(stderr,
                 "[microtel %s] %.*s\n",
                 LevelTag(level),
                 static_cast<int>(message.size()),
                 message.data());
}

}  // namespace internal

}  // namespace microtel
