// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Reads back what the collector wrote.
//
// The conformance collector runs the `file` exporter, which appends one
// compact protojson object per ResourceSpans batch to /out/traces.jsonl. That
// file is the only place a test can observe what the collector *understood*,
// as opposed to what microtel claims it sent.
//
// Delivery is asynchronous — ForceFlush returning only means microtel handed
// the batch to the collector, which still has to run it through its own batch
// processor and flush the file. So reads poll rather than assume.

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace microtel::testing
{

namespace detail
{

/// @brief Slurps a file, returning empty if it does not exist yet.
///
/// A missing file is the normal state until the collector's first flush, so it
/// is not an error worth distinguishing from an empty one.
inline std::string ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// @brief First line of @p haystack containing @p needle.
inline std::optional<std::string> FindLineContaining(const std::string& haystack,
                                                     const std::string& needle)
{
    std::istringstream lines(haystack);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.find(needle) != std::string::npos)
        {
            return line;
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Waits for the collector to write a line containing @p needle.
///
/// Re-reads the whole file every 100 ms: the exporter appends and may rewrite
/// buffered tail content, so holding a stream open across polls risks reading
/// a partial line.
///
/// @param path    the collector's output file.
/// @param needle  substring identifying the line of interest — use a marker
///                from `UniqueMarker()` so other runs' output cannot match.
/// @param timeout how long to keep polling.
/// @return the matching line, or `nullopt` if the timeout elapsed first.
inline std::optional<std::string> PollForLineContaining(const std::filesystem::path& path,
                                                        const std::string& needle,
                                                        std::chrono::milliseconds timeout)
{
    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    std::optional<std::string> found =
        detail::FindLineContaining(detail::ReadWholeFile(path), needle);
    while (!found.has_value() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);
        found = detail::FindLineContaining(detail::ReadWholeFile(path), needle);
    }
    return found;
}

/// @brief Counts non-overlapping occurrences of @p needle in the file at @p path.
///
/// Used by duplicate-detection assertions: a retry that the collector accepted
/// twice shows up as a count of two for the same marker.
///
/// @return 0 if the file is absent or @p needle is empty.
inline std::size_t CountOccurrences(const std::filesystem::path& path, const std::string& needle)
{
    if (needle.empty())
    {
        return 0;
    }
    const std::string content = detail::ReadWholeFile(path);

    std::size_t count = 0;
    std::size_t pos = content.find(needle);
    while (pos != std::string::npos)
    {
        ++count;
        pos = content.find(needle, pos + needle.size());
    }
    return count;
}

}  // namespace microtel::testing
