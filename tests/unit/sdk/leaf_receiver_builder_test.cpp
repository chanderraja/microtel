// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// SdkBuilder::WithLeafReceiver (docs/leaf-concentrator-design.md §4.3, §6.2):
// what Build() accepts and refuses, and the receiver the built provider hands
// out. Built in both configurations of MICROTEL_WITH_CONCENTRATOR; the test
// target carries the same definition as the library.

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

#else

TEST(LeafReceiverBuilderTest, EnablingAConcentratorThatIsNotCompiledInFailsTheBuild)
{
    const auto r = BuildWith(mt::LeafReceiverOptions{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "concentrator.enabled");
    EXPECT_NE(r.error().message.find("MICROTEL_WITH_CONCENTRATOR"), std::string::npos);
}

#endif
