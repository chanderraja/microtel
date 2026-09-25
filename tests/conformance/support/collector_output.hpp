// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Reads back what the collector wrote.
//
// The conformance collector runs the `file` exporter, which appends one
// compact protojson object per batch: ResourceSpans to /out/traces.jsonl and
// ResourceLogs to /out/logs.jsonl. Those files are the only place a test can
// observe what the collector *understood*, as opposed to what microtel claims
// it sent.
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
#include <vector>

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

/// @brief Tracks whether a left-to-right scan of JSON text is inside a string,
///        so a brace or quote inside a string value is not mistaken for
///        structure.
class JsonScanner
{
public:
    /// @brief Consumes @p c.
    /// @return true when @p c is structural (outside any string literal).
    bool Structural(const char c) noexcept
    {
        if (!m_in_string)
        {
            m_in_string = c == '"';
            return !m_in_string;
        }
        if (m_escaped)
        {
            m_escaped = false;
        }
        else if (c == '\\')
        {
            m_escaped = true;
        }
        else
        {
            m_in_string = c != '"';
        }
        return false;
    }

private:
    bool m_in_string = false;
    bool m_escaped = false;
};

/// @brief Index one past the `}` closing the object that opens at @p open.
/// @return npos when the object is not closed within @p text.
inline std::size_t ObjectEnd(const std::string& text, const std::size_t open)
{
    JsonScanner scanner;
    std::size_t depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char c = text[i];
        if (!scanner.Structural(c) || (c != '{' && c != '}'))
        {
            continue;
        }
        depth = c == '{' ? depth + 1 : depth - 1;
        if (depth == 0)
        {
            return i + 1;
        }
    }
    return std::string::npos;
}

/// @brief Offsets of the `{` of every object still open at @p pos, outermost
///        first.
inline std::vector<std::size_t> OpenObjectsAt(const std::string& text, const std::size_t pos)
{
    JsonScanner scanner;
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < pos && i < text.size(); ++i)
    {
        const char c = text[i];
        if (!scanner.Structural(c))
        {
            continue;
        }
        if (c == '{')
        {
            open.push_back(i);
        }
        else if (c == '}' && !open.empty())
        {
            open.pop_back();
        }
    }
    return open;
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

/// @brief The JSON object @p levels deep around the first occurrence of
///        @p needle in @p line.
///
/// A collector output line is a whole batch, and the collector's batch
/// processor may merge several exports into it, so "the line contains X" does
/// not prove "this record contains X". This narrows an assertion to the object
/// holding the needle (`levels` = 1), or to one further out per extra level —
/// for a log record, 2 is its `ScopeLogs` (past the `logRecords` array) and 3
/// its `ResourceLogs`.
///
/// @return the object's text, or `nullopt` if @p needle is absent or fewer
///         than @p levels objects enclose it.
inline std::optional<std::string> EnclosingObject(const std::string& line,
                                                  const std::string& needle,
                                                  const std::size_t levels)
{
    const std::size_t pos = line.find(needle);
    if (pos == std::string::npos || levels == 0)
    {
        return std::nullopt;
    }
    const std::vector<std::size_t> open = detail::OpenObjectsAt(line, pos);
    if (open.size() < levels)
    {
        return std::nullopt;
    }
    const std::size_t start = open[open.size() - levels];
    const std::size_t end = detail::ObjectEnd(line, start);
    if (end == std::string::npos)
    {
        return std::nullopt;
    }
    return line.substr(start, end - start);
}

}  // namespace microtel::testing
