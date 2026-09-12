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
//
// ToHex() below duplicates what include/microtel/trace.hpp already promises.
// TraceId::ToHex() and SpanId::ToHex() are declared there but defined in no
// shipped translation unit, so calling either from outside the library is a
// link error — this test tier, which builds against public headers only, is
// the first thing in the repo to notice. Delete this helper and switch to the
// public formatter once those two are implemented.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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

/// @brief Lower-case hex, no separators — the protojson id encoding.
///
/// The collector renders `trace_id` and `span_id` this way, so this is how a
/// test turns a `SpanContext` into a needle for the output file:
/// `ToHex(context.trace_id.AsBytes())`.
///
/// @param bytes borrowed; any length.
inline std::string ToHex(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view kHexDigits = "0123456789abcdef";
    constexpr unsigned int kNibbleShift = 4U;
    constexpr unsigned int kNibbleMask = 0xFU;

    std::string out;
    out.reserve(bytes.size() * 2U);
    for (const std::uint8_t byte : bytes)
    {
        out.push_back(kHexDigits[static_cast<unsigned int>(byte) >> kNibbleShift]);
        out.push_back(kHexDigits[static_cast<unsigned int>(byte) & kNibbleMask]);
    }
    return out;
}

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
