// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 — the two built-in resource detectors (process, host).
//
// Every filesystem source both detectors read is rooted at an injected prefix,
// so these tests run against fixture trees under a temporary directory and
// never against the real /proc, /etc or /var. The only un-injectable sources
// are the two syscalls — getpid(2) and gethostname(2) — which are asserted
// against the process's own real values.

#include "microtel/resource_detectors.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/resource.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace
{

constexpr std::size_t kHostNameMax = 256;

/// @brief RAII fixture root: a unique temporary directory, removed on destruction.
class FixtureRoot
{
public:
    explicit FixtureRoot(std::string_view tag)
        : m_path(std::filesystem::temp_directory_path() /
                 ("microtel_detector_" + std::string{tag} + "_" + std::to_string(::getpid())))
    {
        std::filesystem::remove_all(m_path);
        std::filesystem::create_directories(m_path);
    }

    ~FixtureRoot()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    FixtureRoot(const FixtureRoot&) = delete;
    FixtureRoot& operator=(const FixtureRoot&) = delete;
    FixtureRoot(FixtureRoot&&) = delete;
    FixtureRoot& operator=(FixtureRoot&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return m_path;
    }

    /// @brief Write `bytes` verbatim to `<root>/relative`, creating parents.
    void WriteFile(const std::filesystem::path& relative, std::string_view bytes) const
    {
        const auto full = m_path / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out{full, std::ios::binary};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    /// @brief Create `<root>/relative` as a symlink to `target` (may dangle).
    void WriteSymlink(const std::filesystem::path& relative,
                      const std::filesystem::path& target) const
    {
        const auto full = m_path / relative;
        std::filesystem::create_directories(full.parent_path());
        std::filesystem::create_symlink(target, full);
    }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::optional<microtel::AttributeValue> Lookup(const microtel::Resource& res,
                                                             std::string_view key)
{
    for (const auto& kv : res.Attributes())
    {
        if (kv.key == key)
        {
            return kv.value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool Has(const microtel::Resource& res, std::string_view key)
{
    return Lookup(res, key).has_value();
}

/// @brief The value at `key`. An absent key fails the test rather than throwing.
[[nodiscard]] microtel::AttributeValue Get(const microtel::Resource& res, std::string_view key)
{
    const auto value = Lookup(res, key);
    if (!value.has_value())
    {
        ADD_FAILURE() << "missing resource attribute: " << key;
        return std::string{};
    }
    return *value;
}

[[nodiscard]] std::string Str(const microtel::Resource& res, std::string_view key)
{
    return std::get<std::string>(Get(res, key));
}

/// @brief A cmdline body: NUL-separated, NUL-terminated, as the kernel writes it.
[[nodiscard]] std::string Cmdline(std::initializer_list<std::string_view> args)
{
    std::string out;
    for (const auto& a : args)
    {
        out.append(a);
        out.push_back('\0');
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ProcessDetector
// ---------------------------------------------------------------------------

TEST(ProcessDetectorTest, Detect_EmitsPidOfThisProcess)
{
    const FixtureRoot root{"pid"};
    root.WriteFile("proc/self/cmdline", Cmdline({"/usr/bin/app"}));

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(std::get<std::int64_t>(Get(*res, "process.pid")),
              static_cast<std::int64_t>(::getpid()));
}

TEST(ProcessDetectorTest, Detect_ReadsExecutablePathAndNameFromProcSelfExe)
{
    const FixtureRoot root{"exe"};
    root.WriteFile("proc/self/cmdline", Cmdline({"app"}));
    root.WriteSymlink("proc/self/exe", "/opt/acme/bin/checkout-service");

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "process.executable.path"), "/opt/acme/bin/checkout-service");
    EXPECT_EQ(Str(*res, "process.executable.name"), "checkout-service");
}

TEST(ProcessDetectorTest, Detect_MissingProcSelfExe_OmitsExecutableAttributesWithoutFailing)
{
    // A restricted /proc hides the exe link; the rest of the contribution is
    // still real, so this is an omission rather than a detector failure.
    const FixtureRoot root{"noexe"};
    root.WriteFile("proc/self/cmdline", Cmdline({"app"}));

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(Has(*res, "process.executable.path"));
    EXPECT_FALSE(Has(*res, "process.executable.name"));
    EXPECT_TRUE(Has(*res, "process.pid"));
}

TEST(ProcessDetectorTest, Detect_MultiArgCmdline_EmitsCommandAndArgs)
{
    const FixtureRoot root{"args"};
    root.WriteFile("proc/self/cmdline", Cmdline({"/usr/bin/app", "--port", "8080"}));

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "process.command"), "/usr/bin/app");
    const auto args = std::get<std::vector<std::string>>(Get(*res, "process.command_args"));
    ASSERT_EQ(args.size(), 3U);
    EXPECT_EQ(args[0], "/usr/bin/app");
    EXPECT_EQ(args[1], "--port");
    EXPECT_EQ(args[2], "8080");
}

TEST(ProcessDetectorTest, Detect_SingleArgCmdline_EmitsOneArg)
{
    const FixtureRoot root{"onearg"};
    root.WriteFile("proc/self/cmdline", Cmdline({"/usr/bin/app"}));

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "process.command"), "/usr/bin/app");
    const auto args = std::get<std::vector<std::string>>(Get(*res, "process.command_args"));
    ASSERT_EQ(args.size(), 1U);
    EXPECT_EQ(args[0], "/usr/bin/app");
}

TEST(ProcessDetectorTest, Detect_TrailingNulDoesNotProduceAnEmptyArg)
{
    // The kernel NUL-terminates the last argument. Splitting naively on NUL
    // yields a phantom empty argv entry; it must not appear.
    const FixtureRoot root{"trailnul"};
    root.WriteFile("proc/self/cmdline", Cmdline({"app", "-v"}));

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    const auto args = std::get<std::vector<std::string>>(Get(*res, "process.command_args"));
    ASSERT_EQ(args.size(), 2U);
    EXPECT_EQ(args[1], "-v");
}

TEST(ProcessDetectorTest, Detect_CmdlineWithoutTrailingNul_StillYieldsLastArg)
{
    const FixtureRoot root{"nonul"};
    root.WriteFile("proc/self/cmdline", std::string{"app\0-v", 6});

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    const auto args = std::get<std::vector<std::string>>(Get(*res, "process.command_args"));
    ASSERT_EQ(args.size(), 2U);
    EXPECT_EQ(args[0], "app");
    EXPECT_EQ(args[1], "-v");
}

TEST(ProcessDetectorTest, Detect_EmptyCmdline_OmitsCommandAttributes)
{
    // Kernel threads have a zero-length cmdline. That is not a failure — the
    // pid is still a real contribution.
    const FixtureRoot root{"emptycmd"};
    root.WriteFile("proc/self/cmdline", "");

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(Has(*res, "process.command"));
    EXPECT_FALSE(Has(*res, "process.command_args"));
    EXPECT_TRUE(Has(*res, "process.pid"));
}

TEST(ProcessDetectorTest, Detect_CmdlineOfOnlyNuls_OmitsCommandAttributes)
{
    const FixtureRoot root{"nulscmd"};
    root.WriteFile("proc/self/cmdline", std::string{"\0\0", 2});

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(Has(*res, "process.command"));
    EXPECT_FALSE(Has(*res, "process.command_args"));
}

TEST(ProcessDetectorTest, Detect_MissingCmdline_ReturnsConfigError)
{
    // No /proc at all under this root: the detector cannot do its job and says
    // so. Whether that fails Build() is the strict/lenient policy's decision,
    // not the detector's.
    const FixtureRoot root{"noproc"};

    const auto detector = microtel::MakeProcessDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().kind, microtel::ConfigError::Kind::Unspecified);
    EXPECT_FALSE(res.error().message.empty());
}

TEST(ProcessDetectorTest, Name_IdentifiesTheDetector)
{
    const auto detector = microtel::MakeProcessDetector();
    EXPECT_EQ(detector->Name(), "process");
}

TEST(ProcessDetectorTest, Detect_DefaultRoot_ReadsTheRealProc)
{
    // The one test that does touch the real filesystem: it proves the default
    // root is "/" and that a stock Linux /proc satisfies the detector.
    const auto detector = microtel::MakeProcessDetector();
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(std::get<std::int64_t>(Get(*res, "process.pid")),
              static_cast<std::int64_t>(::getpid()));
    EXPECT_TRUE(Has(*res, "process.executable.path"));
    EXPECT_TRUE(Has(*res, "process.command"));
}

// ---------------------------------------------------------------------------
// HostDetector
// ---------------------------------------------------------------------------

TEST(HostDetectorTest, Detect_EmitsHostNameOfThisMachine)
{
    const FixtureRoot root{"hostname"};

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    std::array<char, kHostNameMax> buf{};
    ASSERT_EQ(::gethostname(buf.data(), buf.size() - 1), 0);
    EXPECT_EQ(Str(*res, "host.name"), std::string{buf.data()});
}

TEST(HostDetectorTest, Detect_EtcMachineId_EmitsHostId)
{
    const FixtureRoot root{"machineid"};
    root.WriteFile("etc/machine-id", "6b2b1a0e5f7c4d3e8a9b0c1d2e3f4a5b\n");

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.id"), "6b2b1a0e5f7c4d3e8a9b0c1d2e3f4a5b");
}

TEST(HostDetectorTest, Detect_EtcMachineIdAbsent_FallsBackToDbusMachineId)
{
    const FixtureRoot root{"dbusid"};
    root.WriteFile("var/lib/dbus/machine-id", "aaaabbbbccccddddeeeeffff00001111\n");

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.id"), "aaaabbbbccccddddeeeeffff00001111");
}

TEST(HostDetectorTest, Detect_EtcMachineIdEmpty_FallsBackToDbusMachineId)
{
    // An unprovisioned /etc/machine-id is a zero-length file, not an absent
    // one. Taking it at face value would emit an empty host.id.
    const FixtureRoot root{"emptyid"};
    root.WriteFile("etc/machine-id", "\n");
    root.WriteFile("var/lib/dbus/machine-id", "0123456789abcdef0123456789abcdef\n");

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.id"), "0123456789abcdef0123456789abcdef");
}

TEST(HostDetectorTest, Detect_EtcMachineIdWins_WhenBothArePresent)
{
    const FixtureRoot root{"bothids"};
    root.WriteFile("etc/machine-id", "11111111111111111111111111111111\n");
    root.WriteFile("var/lib/dbus/machine-id", "22222222222222222222222222222222\n");

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(Str(*res, "host.id"), "11111111111111111111111111111111");
}

TEST(HostDetectorTest, Detect_NoMachineIdAnywhere_OmitsHostIdWithoutFailing)
{
    // Common in containers. host.id is optional in the OTel conventions, so an
    // absent one is an omission, not a detector failure.
    const FixtureRoot root{"noid"};

    const auto detector = microtel::MakeHostDetector(root.Path());
    const auto res = detector->Detect();

    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(Has(*res, "host.id"));
    EXPECT_TRUE(Has(*res, "host.name"));
}

TEST(HostDetectorTest, Name_IdentifiesTheDetector)
{
    const auto detector = microtel::MakeHostDetector();
    EXPECT_EQ(detector->Name(), "host");
}
