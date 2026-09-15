// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The two built-in `IResourceDetector` realisations, plus their public
// factories. Both classes live in an anonymous namespace: nothing outside this
// translation unit names them, because `MakeProcessDetector` /
// `MakeHostDetector` hand back the abstract interface.
//
// Every filesystem source is resolved against an injected `root` prefix so the
// tests run against fixture trees rather than the machine's real /proc, /etc
// and /var. The two syscalls — getpid(2) and gethostname(2) — have no such
// seam and are read directly.

#include "microtel/resource_detectors.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/resource.hpp"

#include "common/raii/unique_fd.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace microtel
{

namespace
{

constexpr std::string_view kProcessDetectorName = "process";
constexpr std::string_view kHostDetectorName = "host";

constexpr std::string_view kProcSelfExe = "proc/self/exe";
constexpr std::string_view kProcSelfCmdline = "proc/self/cmdline";
constexpr std::string_view kEtcMachineId = "etc/machine-id";
constexpr std::string_view kDbusMachineId = "var/lib/dbus/machine-id";

/// Read granularity. procfs files are small; one pass usually suffices.
constexpr std::size_t kReadChunkBytes = 4096;

/// `gethostname(2)` is capped at `HOST_NAME_MAX` (64 on Linux); 256 leaves
/// room for a longer limit on another kernel and for the NUL.
constexpr std::size_t kHostNameBufferBytes = 256;

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

/// @brief One `read(2)`, retried while it is interrupted by a signal.
///
/// @param fd borrowed, open for reading.
/// @param buffer destination.
/// @return bytes read, 0 at end of file, or -1 with `errno` set.
[[nodiscard]] ssize_t ReadRetryingOnEintr(int fd, std::span<char> buffer)
{
    ssize_t n = ::read(fd, buffer.data(), buffer.size());
    while (n < 0 && errno == EINTR)
    {
        n = ::read(fd, buffer.data(), buffer.size());
    }
    return n;
}

/// @brief Read a whole file into a string.
///
/// procfs files report a size of zero, so `std::filesystem::file_size` and any
/// stat-then-read scheme is useless here — the only way to learn the length is
/// to read until EOF.
///
/// @param path absolute path to read.
/// @return the file's bytes, or `nullopt` if it could not be opened or read.
[[nodiscard]] std::optional<std::string> ReadWholeFile(const std::filesystem::path& path)
{
    // NOLINTNEXTLINE(hicpp-signed-bitwise) — O_RDONLY / O_CLOEXEC are signed
    const common::raii::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd.IsValid())
    {
        return std::nullopt;
    }

    std::string out;
    std::array<char, kReadChunkBytes> buffer{};
    while (true)
    {
        const ssize_t n = ReadRetryingOnEintr(fd.Get(), buffer);
        if (n < 0)
        {
            return std::nullopt;
        }
        if (n == 0)
        {
            return out;
        }
        out.append(buffer.data(), static_cast<std::size_t>(n));
    }
}

/// @brief Resolve a symlink's target.
///
/// @param path absolute path to the link.
/// @return the target, or `nullopt` when the link is absent or unreadable.
///         A *dangling* link resolves fine — `/proc/self/exe` points at a
///         deleted binary often enough to matter.
[[nodiscard]] std::optional<std::string> ReadSymlinkTarget(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
    if (ec)
    {
        return std::nullopt;
    }
    return target.string();
}

/// @brief Split a NUL-separated buffer into its non-empty pieces.
///
/// The kernel NUL-*terminates* the last argument in `/proc/self/cmdline`, so a
/// naive split yields a phantom trailing entry. Skipping empty pieces removes
/// it, and also collapses the all-NUL buffer some kernel threads present. The
/// cost is that a genuinely empty `argv` entry is dropped rather than carried;
/// that is vanishingly rare and preferable to a phantom one in every process.
[[nodiscard]] std::vector<std::string> SplitNulSeparated(std::string_view bytes)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < bytes.size())
    {
        const std::size_t nul = bytes.find('\0', start);
        const std::size_t end = (nul == std::string_view::npos) ? bytes.size() : nul;
        if (end > start)
        {
            out.emplace_back(bytes.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

/// @brief Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view Trim(std::string_view s)
{
    constexpr std::string_view kSpace = " \t\n\r\f\v";
    const std::size_t first = s.find_first_not_of(kSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

// ---------------------------------------------------------------------------
// ProcessDetector
// ---------------------------------------------------------------------------

/// @brief `process.*` detector. See `MakeProcessDetector` for the attribute set.
class ProcessDetector final : public internal::IResourceDetector
{
public:
    explicit ProcessDetector(std::filesystem::path root) : m_root(std::move(root)) {}

    [[nodiscard]] Expected<Resource, ConfigError> Detect() override
    {
        const auto cmdline = ReadWholeFile(m_root / kProcSelfCmdline);
        if (!cmdline)
        {
            return make_unexpected(ConfigError{.kind = ConfigError::Kind::Unspecified,
                                               .field = "resource.detectors.process",
                                               .message = "process detector: cannot read " +
                                                          (m_root / kProcSelfCmdline).string() +
                                                          " - is /proc mounted?"});
        }

        std::vector<KeyValue> attrs;
        attrs.push_back({.key = "process.pid", .value = static_cast<std::int64_t>(::getpid())});
        AppendExecutable(attrs);
        AppendCommand(attrs, *cmdline);
        return Resource{std::move(attrs)};
    }

    [[nodiscard]] std::string_view Name() const noexcept override
    {
        return kProcessDetectorName;
    }

private:
    /// @brief Append `process.executable.path` / `.name` when the exe link resolves.
    void AppendExecutable(std::vector<KeyValue>& attrs) const
    {
        const auto exe = ReadSymlinkTarget(m_root / kProcSelfExe);
        if (!exe)
        {
            return;
        }
        const std::string name = std::filesystem::path{*exe}.filename().string();
        attrs.push_back({.key = "process.executable.path", .value = *exe});
        attrs.push_back({.key = "process.executable.name", .value = name});
    }

    /// @brief Append `process.command` / `.command_args` from a cmdline buffer.
    static void AppendCommand(std::vector<KeyValue>& attrs, std::string_view cmdline)
    {
        std::vector<std::string> args = SplitNulSeparated(cmdline);
        if (args.empty())
        {
            return;
        }
        attrs.push_back({.key = "process.command", .value = args.front()});
        attrs.push_back({.key = "process.command_args", .value = std::move(args)});
    }

    std::filesystem::path m_root;
};

// ---------------------------------------------------------------------------
// HostDetector
// ---------------------------------------------------------------------------

/// @brief `host.*` detector. See `MakeHostDetector` for the attribute set.
class HostDetector final : public internal::IResourceDetector
{
public:
    explicit HostDetector(std::filesystem::path root) : m_root(std::move(root)) {}

    [[nodiscard]] Expected<Resource, ConfigError> Detect() override
    {
        std::array<char, kHostNameBufferBytes> buffer{};
        if (::gethostname(buffer.data(), buffer.size() - 1) != 0)
        {
            return make_unexpected(ConfigError{.kind = ConfigError::Kind::Unspecified,
                                               .field = "resource.detectors.host",
                                               .message = "host detector: gethostname failed"});
        }

        std::vector<KeyValue> attrs;
        attrs.push_back({.key = "host.name", .value = std::string{buffer.data()}});
        const std::string id = MachineId();
        if (!id.empty())
        {
            attrs.push_back({.key = "host.id", .value = id});
        }
        return Resource{std::move(attrs)};
    }

    [[nodiscard]] std::string_view Name() const noexcept override
    {
        return kHostDetectorName;
    }

private:
    /// @brief The first non-empty machine id of the two known locations.
    ///
    /// @return the trimmed id, or an empty string when neither file yields one.
    [[nodiscard]] std::string MachineId() const
    {
        for (const std::string_view candidate : {kEtcMachineId, kDbusMachineId})
        {
            const auto contents = ReadWholeFile(m_root / candidate);
            if (!contents)
            {
                continue;
            }
            const std::string_view id = Trim(*contents);
            if (!id.empty())
            {
                return std::string{id};
            }
        }
        return {};
    }

    std::filesystem::path m_root;
};

}  // namespace

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

std::unique_ptr<internal::IResourceDetector> MakeProcessDetector(std::filesystem::path root)
{
    return std::make_unique<ProcessDetector>(std::move(root));
}

std::unique_ptr<internal::IResourceDetector> MakeHostDetector(std::filesystem::path root)
{
    return std::make_unique<HostDetector>(std::move(root));
}

}  // namespace microtel
