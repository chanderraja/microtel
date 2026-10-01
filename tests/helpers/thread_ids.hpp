// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Thread accounting for tests that assert a thread was, or was not, started.
// Linux-only, via /proc, which matches the project's target platform.
//
// Two effects of a thread that was joined just before -- by an earlier test's
// provider, a Shutdown or a reset -- make a naive measurement flaky:
//
// - pthread_join returns before the kernel releases the thread, so it is still
//   counted, and listed, for a moment after the join. A difference of two
//   counts comes out short when it goes mid-measurement. Comparing id sets
//   instead ignores a thread that disappears.
// - A listing of /proc/self/task that walks onto a thread just as it is
//   released stops there, silently dropping every thread after it. One
//   listing can then miss a live thread, which a later listing reports as new,
//   or miss the thread just started. `ThreadIds()` lists until two listings in
//   a row agree.

#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

namespace microtel::testing
{

/// @brief One listing of /proc/self/task. May be cut short; see the file note.
[[nodiscard]] inline std::set<std::string> ThreadIdsOnce()
{
    std::set<std::string> ids;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/task"})
    {
        ids.insert(entry.path().filename().string());
    }
    return ids;
}

/// @brief The ids of this process's threads, once two listings in a row agree.
///
/// A cut-short listing differs from the next whole one, and the thread that cut
/// it short is gone by then, so it cannot cut the next one short again. Gives
/// up after a bounded number of listings and returns the last, so a process
/// that never settles fails the assertion instead of hanging the test.
[[nodiscard]] inline std::set<std::string> ThreadIds()
{
    constexpr int kMaxListings = 1000;
    auto previous = ThreadIdsOnce();
    for (int i = 1; i < kMaxListings; ++i)
    {
        auto current = ThreadIdsOnce();
        if (current == previous)
        {
            return current;
        }
        previous = std::move(current);
    }
    return previous;
}

/// @brief Threads started since @p before was taken by `ThreadIds()`.
///
/// Threads that have exited since do not count against it.
[[nodiscard]] inline std::size_t ThreadsStartedSince(const std::set<std::string>& before)
{
    const auto now = ThreadIds();
    return static_cast<std::size_t>(
        std::ranges::count_if(now, [&before](const auto& id) { return !before.contains(id); }));
}

}  // namespace microtel::testing
