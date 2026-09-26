// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// SdkBuilder::WithLeafReceiver, the [concentrator] TOML table and the
// MICROTEL_CONCENTRATOR_* variables (docs/leaf-concentrator-design.md §4.2,
// §4.3, §6.2): what Build() accepts and refuses from each source and from
// their merge, and the receiver the built provider hands out. Built in both configurations of
// MICROTEL_WITH_CONCENTRATOR; the test target carries the same definition as the library.

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace mt = microtel;

namespace
{

mt::Expected<std::shared_ptr<mt::Provider>, mt::ConfigError> BuildWith(mt::LeafReceiverOptions opts)
{
    return mt::SdkBuilder()
        .WithEndpoint("https://localhost:4318")
        .WithLeafReceiver(std::move(opts))
        .Build();
}

constexpr std::array<std::byte, 4> kPayload{};

/// Writes a `microtel.toml` with @p content and removes it on destruction.
class TomlFile
{
public:
    explicit TomlFile(std::string_view content)
        : m_path(std::filesystem::temp_directory_path() /
                 ("microtel_leaf_builder_" + std::to_string(::getpid()) + ".toml"))
    {
        std::ofstream f{m_path};
        f << content;
    }

    ~TomlFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TomlFile(const TomlFile&) = delete;
    TomlFile& operator=(const TomlFile&) = delete;
    TomlFile(TomlFile&&) = delete;
    TomlFile& operator=(TomlFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

/// Sets one variable and unsets it on destruction.
class EnvVar
{
public:
    EnvVar(const char* name, const char* value) : m_name(name)
    {
        ::setenv(name, value, /*overwrite=*/1);
    }

    ~EnvVar()
    {
        ::unsetenv(m_name);
    }

    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    EnvVar(EnvVar&&) = delete;
    EnvVar& operator=(EnvVar&&) = delete;

private:
    const char* m_name;
};

mt::Expected<std::shared_ptr<mt::Provider>, mt::ConfigError> BuildFromFile(
    const TomlFile& file, std::optional<mt::LeafReceiverOptions> opts = std::nullopt)
{
    mt::SdkBuilder builder;
    builder.FromFile(file.Path()).WithEndpoint("https://localhost:4318");
    if (opts.has_value())
    {
        builder.WithLeafReceiver(std::move(*opts));
    }
    return builder.Build();
}

mt::IngestStatus IngestFrom(const std::shared_ptr<mt::Provider>& provider, std::string_view leaf)
{
    return provider->GetLeafReceiver()
        ->Ingest(mt::IngestRequest{.leaf_id = leaf, .payload = kPayload})
        .status;
}

}  // namespace

TEST(LeafReceiverBuilderTest, WithoutWithLeafReceiverTheReceiverIsDisabled)
{
    auto provider = mt::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(provider.has_value());
    const auto receiver = (*provider)->GetLeafReceiver();
    ASSERT_NE(receiver, nullptr);
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = kPayload}).status,
              mt::IngestStatus::Disabled);
}

TEST(LeafReceiverBuilderTest, EnabledFalseBuildsWithADisabledReceiver)
{
    auto provider = BuildWith(mt::LeafReceiverOptions{.enabled = false});
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ((*provider)
                  ->GetLeafReceiver()
                  ->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = kPayload})
                  .status,
              mt::IngestStatus::Disabled);
}

#ifdef MICROTEL_WITH_CONCENTRATOR

TEST(LeafReceiverBuilderTest, WithLeafReceiverBuildsALiveReceiver)
{
    auto provider = BuildWith(mt::LeafReceiverOptions{});
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    const auto receiver = (*provider)->GetLeafReceiver();
    // Four zero bytes are not a leaf payload, so a live receiver says Malformed.
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = kPayload}).status,
              mt::IngestStatus::Malformed);
    EXPECT_EQ(receiver->Stats().payloads_rejected, 1U);
}

TEST(LeafReceiverBuilderTest, ZeroLimitsAreRejected)
{
    for (const auto& opts : {mt::LeafReceiverOptions{.max_payload_bytes = 0},
                             mt::LeafReceiverOptions{.max_spans_per_payload = 0},
                             mt::LeafReceiverOptions{.max_leaves = 0}})
    {
        const auto r = BuildWith(opts);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
        EXPECT_TRUE(r.error().field.starts_with("concentrator.")) << r.error().field;
    }
}

TEST(LeafReceiverBuilderTest, ConfiguringTheLeafIdAttributeKeyIsRejected)
{
    const auto in_defaults = BuildWith(mt::LeafReceiverOptions{
        .leaf_defaults_resource = {{.key = "device.id", .value = std::string{"x"}}}});
    ASSERT_FALSE(in_defaults.has_value());
    EXPECT_EQ(in_defaults.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(in_defaults.error().field, "concentrator.leaf_defaults.resource");

    const auto in_leaf = BuildWith(mt::LeafReceiverOptions{
        .leaves = {
            {"a", mt::LeafConfig{.resource = {{.key = "device.id", .value = std::string{"x"}}}}}}});
    ASSERT_FALSE(in_leaf.has_value());
    EXPECT_EQ(in_leaf.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(in_leaf.error().field, "concentrator.leaves.a.resource");
}

TEST(LeafReceiverBuilderTest, ARenamedLeafIdAttributeFreesDeviceId)
{
    const auto r = BuildWith(mt::LeafReceiverOptions{
        .leaf_id_attribute = "leaf.id",
        .leaf_defaults_resource = {{.key = "device.id", .value = std::string{"x"}}}});
    EXPECT_TRUE(r.has_value());
}

TEST(LeafReceiverBuilderTest, ConfiguringAReservedKeyIsRejected)
{
    const auto r = BuildWith(mt::LeafReceiverOptions{
        .leaf_defaults_resource = {{.key = "microtel.leaf.proto", .value = std::int64_t{1}}}});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
}

TEST(LeafReceiverBuilderTest, AConfiguredResourceOverTheBudgetIsRejected)
{
    const auto r = BuildWith(mt::LeafReceiverOptions{
        .max_leaf_resource_bytes = 8,
        .leaf_defaults_resource = {{.key = "service.namespace", .value = std::string{"fleet"}}}});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
}

TEST(LeafReceiverBuilderTest, BadLeafIdsAreRejected)
{
    for (const std::string& id : {std::string{}, std::string(129, 'x')})
    {
        const auto r = BuildWith(mt::LeafReceiverOptions{.leaves = {{id, mt::LeafConfig{}}}});
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    }
}


TEST(LeafReceiverBuilderTest, TheConcentratorTableAloneBuildsALiveReceiver)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"};
    auto provider = BuildFromFile(file);
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Malformed) << "live, not Disabled";
}

TEST(LeafReceiverBuilderTest, TheEnvironmentAloneBuildsALiveReceiver)
{
    const EnvVar enabled{"MICROTEL_CONCENTRATOR_ENABLED", "true"};
    auto provider = mt::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Malformed);
}

TEST(LeafReceiverBuilderTest, TheFilesLeavesAndPolicyReachTheReceiver)
{
    // Under reject, an unconfigured transport id is refused before decode, so
    // the status tells a configured leaf from an unknown one.
    const TomlFile file{"[concentrator]\nenabled = true\nunknown_leaf = \"reject\"\n"
                        "[concentrator.leaves.\"a\"]\n"};
    auto provider = BuildFromFile(file);
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Malformed);
    EXPECT_EQ(IngestFrom(*provider, "b"), mt::IngestStatus::UnknownLeaf);
}

TEST(LeafReceiverBuilderTest, EnvironmentScalarsOverrideTheFile)
{
    const TomlFile file{"[concentrator]\nenabled = true\nunknown_leaf = \"reject\"\n"};
    const EnvVar policy{"MICROTEL_CONCENTRATOR_UNKNOWN_LEAF", "accept"};
    auto provider = BuildFromFile(file);
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "b"), mt::IngestStatus::Malformed);
}

TEST(LeafReceiverBuilderTest, CodeLeavesMergeWithTheFilesPerLeafId)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"
                        "[concentrator.leaves.\"a\"]\n"};
    mt::LeafReceiverOptions opts;
    opts.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    opts.leaves = {{"b", mt::LeafConfig{}}};
    auto provider = BuildFromFile(file, std::move(opts));
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Malformed) << "the file's leaf";
    EXPECT_EQ(IngestFrom(*provider, "b"), mt::IngestStatus::Malformed) << "the code's leaf";
    EXPECT_EQ(IngestFrom(*provider, "c"), mt::IngestStatus::UnknownLeaf);
}

TEST(LeafReceiverBuilderTest, CodeCanDisableWhatTheFileEnables)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"};
    auto provider = BuildFromFile(file, mt::LeafReceiverOptions{.enabled = false});
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Disabled);
}

TEST(LeafReceiverBuilderTest, AReservedKeyInTheFilesLeafResourceIsRejected)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"
                        "[concentrator.leaves.\"a\".resource]\n\"microtel.leaf.boot_id\" = 1\n"};
    const auto r = BuildFromFile(file);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "concentrator.leaves.a.resource");
}

TEST(LeafReceiverBuilderTest, TheLeafIdKeyInTheFilesDefaultsIsRejected)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"
                        "[concentrator.leaf_defaults.resource]\n\"device.id\" = \"x\"\n"};
    const auto r = BuildFromFile(file);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().field, "concentrator.leaf_defaults.resource");
}

TEST(LeafReceiverBuilderTest, NonPositiveDurationsAreRejected)
{
    const TomlFile file{"[concentrator]\nenabled = true\nleaf_idle_timeout = \"0s\"\n"};
    const auto from_file = BuildFromFile(file);
    ASSERT_FALSE(from_file.has_value());
    EXPECT_EQ(from_file.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(from_file.error().field, "concentrator.leaf_idle_timeout");

    for (const auto& [opts, field] : std::vector<std::pair<mt::LeafReceiverOptions, std::string>>{
             {mt::LeafReceiverOptions{.max_sync_age = std::chrono::seconds{0}},
              "concentrator.max_sync_age"},
             {mt::LeafReceiverOptions{.max_clock_skew = std::chrono::seconds{-1}},
              "concentrator.max_clock_skew"},
             {mt::LeafReceiverOptions{.boot_anchor_window = std::chrono::seconds{0}},
              "concentrator.boot_anchor_window"},
         })
    {
        const auto r = BuildWith(opts);
        ASSERT_FALSE(r.has_value()) << field;
        EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
        EXPECT_EQ(r.error().field, field);
    }
}

TEST(LeafReceiverBuilderTest, AnInvalidTomlValueFailsTheBuild)
{
    const TomlFile file{"[concentrator]\nmax_payload_bytes = \"64KB\"\n"};
    const auto r = BuildFromFile(file);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "concentrator.max_payload_bytes");
}
#else

TEST(LeafReceiverBuilderTest, EnablingTheConcentratorFromTheFileFailsTheBuildWhenNotCompiledIn)
{
    const TomlFile file{"[concentrator]\nenabled = true\n"};
    const auto r = BuildFromFile(file);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "concentrator.enabled");
}

TEST(LeafReceiverBuilderTest, ADisabledConcentratorTableBuildsWhenNotCompiledIn)
{
    const TomlFile file{"[concentrator]\nenabled = false\nmax_leaves = 8\n"};
    auto provider = BuildFromFile(file);
    ASSERT_TRUE(provider.has_value()) << provider.error().message;
    EXPECT_EQ(IngestFrom(*provider, "a"), mt::IngestStatus::Disabled);
}

TEST(LeafReceiverBuilderTest, EnablingAConcentratorThatIsNotCompiledInFailsTheBuild)
{
    const auto r = BuildWith(mt::LeafReceiverOptions{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "concentrator.enabled");
    EXPECT_NE(r.error().message.find("MICROTEL_WITH_CONCENTRATOR"), std::string::npos);
}

#endif
