// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The concentrator's configuration sources (docs/leaf-concentrator-design.md
// §4.2-§4.3, docs/configuration.md §3.14): the value syntax, the
// [concentrator] TOML table, the MICROTEL_CONCENTRATOR_* variables, and the
// per-key merge of WithLeafReceiver's options over them. What Build() then
// does with the result is in tests/unit/sdk/leaf_receiver_builder_test.cpp.

#include "common/config/concentrator_config.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/leaf_receiver.hpp"

#include "common/config/config.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/toml_loader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mc = microtel::config;
namespace mt = microtel;

namespace
{

/// Unsets the variables it names when it goes out of scope.
class EnvVars
{
public:
    EnvVars(std::initializer_list<std::pair<const char*, const char*>> vars)
    {
        for (const auto& [name, value] : vars)
        {
            ::setenv(name, value, /*overwrite=*/1);
            m_names.emplace_back(name);
        }
    }

    ~EnvVars()
    {
        for (const auto& name : m_names)
        {
            ::unsetenv(name.c_str());
        }
    }

    EnvVars(const EnvVars&) = delete;
    EnvVars& operator=(const EnvVars&) = delete;
    EnvVars(EnvVars&&) = delete;
    EnvVars& operator=(EnvVars&&) = delete;

private:
    std::vector<std::string> m_names;
};

std::string StringAt(const std::vector<mt::KeyValue>& attrs, std::string_view key)
{
    const auto it = std::ranges::find(attrs, key, &mt::KeyValue::key);
    if (it == attrs.end())
    {
        return "<absent>";
    }
    const auto* const s = std::get_if<std::string>(&it->value);
    return s == nullptr ? "<not a string>" : *s;
}

const mt::LeafConfig* LeafAt(const mt::LeafReceiverOptions& o, std::string_view id)
{
    const auto it = std::ranges::find(o.leaves, id, &std::pair<std::string, mt::LeafConfig>::first);
    return it == o.leaves.end() ? nullptr : &it->second;
}

mt::ConfigError ParseError(std::string_view toml)
{
    const auto r = mc::ParseTomlString(toml);
    EXPECT_FALSE(r.has_value()) << toml;
    return r.has_value() ? mt::ConfigError{} : r.error();
}

void ExpectInvalid(std::string_view toml, std::string_view field)
{
    const auto err = ParseError(toml);
    EXPECT_EQ(err.kind, mt::ConfigError::Kind::InvalidValue) << toml;
    EXPECT_EQ(err.field, field) << toml;
}

constexpr std::string_view kDesignExample = R"(
[concentrator]
enabled               = true
max_payload_bytes     = "32KiB"
max_spans_per_payload = 256
max_leaves            = 64
max_leaf_resource_bytes = "1KiB"
leaf_idle_timeout     = "30m"
unknown_leaf          = "reject"
leaf_id_attribute     = "leaf.id"
default_time_mode     = "sync_relative"
max_sync_age          = "2h"
max_clock_skew        = "10s"
boot_anchor_window    = "5m"

[concentrator.leaf_defaults.resource]
"service.namespace"      = "boiler-fleet"
"deployment.environment" = "prod"

[concentrator.leaves."can0:0x1a4"]
time_mode = "boot_relative"
[concentrator.leaves."can0:0x1a4".resource]
"service.name" = "burner-controller"
"host.name"    = "boiler-7"
"sensor.count" = 3
"calibrated"   = true
"gain"         = 1.5

[concentrator.leaves."can0:0x1a5"]
)";

}  // namespace

// ---------------------------------------------------------------------------
// Value syntax
// ---------------------------------------------------------------------------

TEST(ConcentratorValueTest, ByteSizes)
{
    EXPECT_EQ(mc::ParseByteSize("65536"), 65536U);
    EXPECT_EQ(mc::ParseByteSize("10B"), 10U);
    EXPECT_EQ(mc::ParseByteSize("64KiB"), 65536U);
    EXPECT_EQ(mc::ParseByteSize("1MiB"), 1048576U);
    EXPECT_EQ(mc::ParseByteSize("4095MiB"), 4095U * 1048576U);
    for (const std::string_view bad : {"", "KiB", "64kib", "64KB", "64 KiB", "-1", "4096MiB", "x"})
    {
        EXPECT_FALSE(mc::ParseByteSize(bad).has_value()) << bad;
    }
}

TEST(ConcentratorValueTest, Durations)
{
    EXPECT_EQ(mc::ParseDuration("30s"), std::chrono::seconds{30});
    EXPECT_EQ(mc::ParseDuration("5m"), std::chrono::seconds{300});
    EXPECT_EQ(mc::ParseDuration("1h"), std::chrono::seconds{3600});
    EXPECT_EQ(mc::ParseDuration("0s"), std::chrono::seconds{0}) << "zero is refused by Build()";
    for (const std::string_view bad :
         {"", "10", "1d", "h", "1H", "1.5h", "-1s", "99999999999999999h"})
    {
        EXPECT_FALSE(mc::ParseDuration(bad).has_value()) << bad;
    }
}

TEST(ConcentratorValueTest, Enumerations)
{
    EXPECT_EQ(mc::ParseUnknownLeafPolicy("accept"), mt::UnknownLeafPolicy::Accept);
    EXPECT_EQ(mc::ParseUnknownLeafPolicy("reject"), mt::UnknownLeafPolicy::Reject);
    EXPECT_FALSE(mc::ParseUnknownLeafPolicy("Reject").has_value());

    EXPECT_EQ(mc::ParseLeafTimeMode("concentrator_stamped"), mt::LeafTimeMode::ConcentratorStamped);
    EXPECT_EQ(mc::ParseLeafTimeMode("sync_relative"), mt::LeafTimeMode::SyncRelative);
    EXPECT_EQ(mc::ParseLeafTimeMode("boot_relative"), mt::LeafTimeMode::BootRelative);
    EXPECT_FALSE(mc::ParseLeafTimeMode("auto").has_value()) << "auto is not a leaf's own mode";

    EXPECT_EQ(mc::ParseDefaultTimeMode("auto"),
              std::make_optional(std::optional<mt::LeafTimeMode>{}))
        << "auto parses, to an unset mode";
    EXPECT_EQ(mc::ParseDefaultTimeMode("boot_relative"),
              std::optional<mt::LeafTimeMode>{mt::LeafTimeMode::BootRelative});
    EXPECT_FALSE(mc::ParseDefaultTimeMode("fast").has_value());
}

// ---------------------------------------------------------------------------
// TOML
// ---------------------------------------------------------------------------

TEST(ConcentratorTomlTest, WithoutTheTableTheReceiverIsOffWithTheDefaults)
{
    const auto cfg = mc::ParseTomlString("");
    ASSERT_TRUE(cfg.has_value());
    const auto& o = cfg->concentrator;
    const mt::LeafReceiverOptions defaults;
    EXPECT_FALSE(o.enabled) << "off unless a source turns it on (§4.2)";
    EXPECT_EQ(o.max_payload_bytes, defaults.max_payload_bytes);
    EXPECT_EQ(o.max_leaves, defaults.max_leaves);
    EXPECT_EQ(o.leaf_idle_timeout, defaults.leaf_idle_timeout);
    EXPECT_EQ(o.boot_anchor_window, defaults.boot_anchor_window);
    EXPECT_EQ(o.leaf_id_attribute, "device.id");
    EXPECT_FALSE(o.default_time_mode.has_value());
    EXPECT_TRUE(o.leaves.empty());
}

TEST(ConcentratorTomlTest, EveryKeyOfTheTableIsRead)
{
    const auto cfg = mc::ParseTomlString(kDesignExample);
    ASSERT_TRUE(cfg.has_value()) << cfg.error().field << ": " << cfg.error().message;
    const auto& o = cfg->concentrator;
    EXPECT_TRUE(o.enabled);
    EXPECT_EQ(o.max_payload_bytes, 32U * 1024U);
    EXPECT_EQ(o.max_spans_per_payload, 256U);
    EXPECT_EQ(o.max_leaves, 64U);
    EXPECT_EQ(o.max_leaf_resource_bytes, 1024U);
    EXPECT_EQ(o.leaf_idle_timeout, std::chrono::minutes{30});
    EXPECT_EQ(o.unknown_leaf, mt::UnknownLeafPolicy::Reject);
    EXPECT_EQ(o.leaf_id_attribute, "leaf.id");
    EXPECT_EQ(o.default_time_mode, mt::LeafTimeMode::SyncRelative);
    EXPECT_EQ(o.max_sync_age, std::chrono::hours{2});
    EXPECT_EQ(o.max_clock_skew, std::chrono::seconds{10});
    EXPECT_EQ(o.boot_anchor_window, std::chrono::minutes{5});
    EXPECT_EQ(StringAt(o.leaf_defaults_resource, "service.namespace"), "boiler-fleet");
    EXPECT_EQ(StringAt(o.leaf_defaults_resource, "deployment.environment"), "prod");

    ASSERT_EQ(o.leaves.size(), 2U);
    const auto* const leaf = LeafAt(o, "can0:0x1a4");
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(leaf->time_mode, mt::LeafTimeMode::BootRelative);
    EXPECT_EQ(StringAt(leaf->resource, "service.name"), "burner-controller");
    EXPECT_EQ(StringAt(leaf->resource, "host.name"), "boiler-7");
    const auto count = std::ranges::find(leaf->resource, "sensor.count", &mt::KeyValue::key);
    ASSERT_NE(count, leaf->resource.end());
    EXPECT_EQ(std::get<std::int64_t>(count->value), 3);
    const auto calibrated = std::ranges::find(leaf->resource, "calibrated", &mt::KeyValue::key);
    ASSERT_NE(calibrated, leaf->resource.end());
    EXPECT_TRUE(std::get<bool>(calibrated->value));
    const auto gain = std::ranges::find(leaf->resource, "gain", &mt::KeyValue::key);
    ASSERT_NE(gain, leaf->resource.end());
    EXPECT_DOUBLE_EQ(std::get<double>(gain->value), 1.5);

    const auto* const bare = LeafAt(o, "can0:0x1a5");
    ASSERT_NE(bare, nullptr) << "a leaf listed with no keys is still configured";
    EXPECT_FALSE(bare->time_mode.has_value());
    EXPECT_TRUE(bare->resource.empty());
}

TEST(ConcentratorTomlTest, AByteSizeMayBeABareInteger)
{
    const auto cfg = mc::ParseTomlString("[concentrator]\nmax_payload_bytes = 4096\n");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->concentrator.max_payload_bytes, 4096U);
}

TEST(ConcentratorTomlTest, DefaultTimeModeAutoIsUnset)
{
    const auto cfg = mc::ParseTomlString("[concentrator]\ndefault_time_mode = \"auto\"\n");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_FALSE(cfg->concentrator.default_time_mode.has_value());
}

TEST(ConcentratorTomlTest, AWrongTypeOrValueIsInvalidAndNamesTheKey)
{
    ExpectInvalid("[concentrator]\nenabled = \"yes\"\n", "concentrator.enabled");
    ExpectInvalid("[concentrator]\nmax_payload_bytes = \"64KB\"\n",
                  "concentrator.max_payload_bytes");
    ExpectInvalid("[concentrator]\nmax_payload_bytes = 1.5\n", "concentrator.max_payload_bytes");
    ExpectInvalid("[concentrator]\nmax_spans_per_payload = \"x\"\n",
                  "concentrator.max_spans_per_payload");
    ExpectInvalid("[concentrator]\nmax_leaves = -1\n", "concentrator.max_leaves");
    ExpectInvalid("[concentrator]\nmax_leaves = 4294967296\n", "concentrator.max_leaves");
    ExpectInvalid("[concentrator]\nmax_leaf_resource_bytes = -2\n",
                  "concentrator.max_leaf_resource_bytes");
    ExpectInvalid("[concentrator]\nleaf_idle_timeout = 3600\n", "concentrator.leaf_idle_timeout");
    ExpectInvalid("[concentrator]\nunknown_leaf = \"maybe\"\n", "concentrator.unknown_leaf");
    ExpectInvalid("[concentrator]\nleaf_id_attribute = 5\n", "concentrator.leaf_id_attribute");
    ExpectInvalid("[concentrator]\ndefault_time_mode = \"fast\"\n",
                  "concentrator.default_time_mode");
    ExpectInvalid("[concentrator]\nmax_sync_age = \"1d\"\n", "concentrator.max_sync_age");
    ExpectInvalid("[concentrator]\nmax_clock_skew = true\n", "concentrator.max_clock_skew");
    ExpectInvalid("[concentrator]\nboot_anchor_window = \"\"\n", "concentrator.boot_anchor_window");
    ExpectInvalid("[concentrator]\nleaf_defaults = 1\n", "concentrator.leaf_defaults");
    ExpectInvalid("[concentrator.leaf_defaults]\nresource = 1\n",
                  "concentrator.leaf_defaults.resource");
    ExpectInvalid("[concentrator.leaf_defaults.resource]\n\"a\" = [1, 2]\n",
                  "concentrator.leaf_defaults.resource");
    // An unquoted dotted key is a nested table, not an attribute.
    ExpectInvalid("[concentrator.leaf_defaults.resource]\nservice.name = \"x\"\n",
                  "concentrator.leaf_defaults.resource");
    ExpectInvalid("[concentrator]\nleaves = 1\n", "concentrator.leaves");
    ExpectInvalid("[concentrator.leaves]\na = 1\n", "concentrator.leaves.a");
    ExpectInvalid("[concentrator.leaves.a]\ntime_mode = \"auto\"\n",
                  "concentrator.leaves.a.time_mode");
    ExpectInvalid("[concentrator.leaves.a.resource]\n\"k\" = {x = 1}\n",
                  "concentrator.leaves.a.resource");
}

TEST(ConcentratorTomlTest, UnknownKeysAreRefusedThroughoutTheTable)
{
    for (const auto& [toml, field] : std::vector<std::pair<std::string_view, std::string_view>>{
             {"[concentrator]\nmax_leafs = 3\n", "concentrator.max_leafs"},
             {"[concentrator.leaf_defaults]\nattributes = {}\n",
              "concentrator.leaf_defaults.attributes"},
             {"[concentrator.leaves.\"a\"]\nmode = \"x\"\n", "concentrator.leaves.a.mode"},
         })
    {
        const auto err = ParseError(toml);
        EXPECT_EQ(err.kind, mt::ConfigError::Kind::UnknownKey) << toml;
        EXPECT_EQ(err.field, field) << toml;
    }
}

TEST(ConcentratorTomlTest, UnknownKeysFollowTheConfiguredPolicy)
{
    const auto cfg = mc::ParseTomlString(
        "[config]\nunknown_keys = \"ignore\"\n[concentrator]\nenabled = true\nmax_leafs = 3\n"
        "[concentrator.leaves.a]\nmode = 1\n");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_TRUE(cfg->concentrator.enabled);
    EXPECT_NE(LeafAt(cfg->concentrator, "a"), nullptr);
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

TEST(ConcentratorEnvTest, EveryVariableIsReadOverTheFile)
{
    auto cfg = mc::ParseTomlString(
        "[concentrator]\nenabled = false\nmax_payload_bytes = 100\nmax_leaves = 5\n"
        "unknown_leaf = \"accept\"\ndefault_time_mode = \"auto\"\n"
        "[concentrator.leaf_defaults.resource]\n\"a\" = \"file\"\n\"b\" = \"file\"\n");
    ASSERT_TRUE(cfg.has_value());
    const EnvVars env{
        {"MICROTEL_CONCENTRATOR_ENABLED", "true"},
        {"MICROTEL_CONCENTRATOR_MAX_PAYLOAD_BYTES", "2KiB"},
        {"MICROTEL_CONCENTRATOR_MAX_LEAVES", "7"},
        {"MICROTEL_CONCENTRATOR_UNKNOWN_LEAF", "reject"},
        {"MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE", "boot_relative"},
        {"MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES", "b=env,c=env"},
    };

    ASSERT_TRUE(mc::OverlayEnv(*cfg).has_value());

    const auto& o = cfg->concentrator;
    EXPECT_TRUE(o.enabled);
    EXPECT_EQ(o.max_payload_bytes, 2048U);
    EXPECT_EQ(o.max_leaves, 7U);
    EXPECT_EQ(o.unknown_leaf, mt::UnknownLeafPolicy::Reject);
    EXPECT_EQ(o.default_time_mode, mt::LeafTimeMode::BootRelative);
    EXPECT_EQ(StringAt(o.leaf_defaults_resource, "a"), "file") << "per key: a survives";
    EXPECT_EQ(StringAt(o.leaf_defaults_resource, "b"), "env");
    EXPECT_EQ(StringAt(o.leaf_defaults_resource, "c"), "env");
}

TEST(ConcentratorEnvTest, EnabledAcceptsZeroAndOneAndAutoClearsTheMode)
{
    mc::Config cfg;
    cfg.concentrator.enabled = true;
    cfg.concentrator.default_time_mode = mt::LeafTimeMode::SyncRelative;
    const EnvVars env{{"MICROTEL_CONCENTRATOR_ENABLED", "0"},
                      {"MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE", "auto"}};

    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());

    EXPECT_FALSE(cfg.concentrator.enabled);
    EXPECT_FALSE(cfg.concentrator.default_time_mode.has_value());
}

TEST(ConcentratorEnvTest, AnUnparseableVariableFailsAndNamesIt)
{
    for (const auto& [name, value] : std::vector<std::pair<const char*, const char*>>{
             {"MICROTEL_CONCENTRATOR_ENABLED", "yes"},
             {"MICROTEL_CONCENTRATOR_MAX_PAYLOAD_BYTES", "64KB"},
             {"MICROTEL_CONCENTRATOR_MAX_LEAVES", "12x"},
             {"MICROTEL_CONCENTRATOR_UNKNOWN_LEAF", "drop"},
             {"MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE", "gps"},
             {"MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES", "novalue"},
         })
    {
        const EnvVars env{{name, value}};
        mc::Config cfg;
        const auto r = mc::OverlayEnv(cfg);
        ASSERT_FALSE(r.has_value()) << name;
        EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::EnvParseFailure) << name;
        EXPECT_EQ(r.error().field, name);
    }
}

// ---------------------------------------------------------------------------
// Code over file and environment
// ---------------------------------------------------------------------------

TEST(ConcentratorMergeTest, CodeScalarsAndTheResolverWin)
{
    mt::LeafReceiverOptions base = mc::DefaultConcentratorOptions();
    base.max_leaves = 5;
    base.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    mt::LeafReceiverOptions code;
    code.max_leaves = 9;
    code.resolver = [](std::string_view) -> std::optional<mt::LeafConfig> { return std::nullopt; };

    mc::MergeLeafReceiverOptions(base, code);

    EXPECT_TRUE(base.enabled) << "WithLeafReceiver implies enabled";
    EXPECT_EQ(base.max_leaves, 9U);
    EXPECT_EQ(base.unknown_leaf, mt::UnknownLeafPolicy::Accept)
        << "every scalar comes from code, as with WithBatch";
    EXPECT_TRUE(static_cast<bool>(base.resolver));
}

TEST(ConcentratorMergeTest, TablesMergePerKeyAndLeavesPerId)
{
    mt::LeafReceiverOptions base = mc::DefaultConcentratorOptions();
    base.leaf_defaults_resource = {{.key = "a", .value = std::string{"file"}},
                                   {.key = "b", .value = std::string{"file"}}};
    base.leaves = {
        {"x",
         mt::LeafConfig{.time_mode = mt::LeafTimeMode::BootRelative,
                        .resource = {{.key = "k1", .value = std::string{"file"}},
                                     {.key = "k2", .value = std::string{"file"}}}}},
        {"y", mt::LeafConfig{.time_mode = mt::LeafTimeMode::SyncRelative, .resource = {}}},
    };
    mt::LeafReceiverOptions code;
    code.leaf_defaults_resource = {{.key = "b", .value = std::string{"code"}}};
    code.leaves = {
        {"x",
         mt::LeafConfig{.time_mode = std::nullopt,
                        .resource = {{.key = "k2", .value = std::string{"code"}}}}},
        {"y", mt::LeafConfig{.time_mode = mt::LeafTimeMode::ConcentratorStamped, .resource = {}}},
        {"z",
         mt::LeafConfig{.time_mode = std::nullopt,
                        .resource = {{.key = "k", .value = std::string{"code"}}}}},
    };

    mc::MergeLeafReceiverOptions(base, code);

    EXPECT_EQ(StringAt(base.leaf_defaults_resource, "a"), "file");
    EXPECT_EQ(StringAt(base.leaf_defaults_resource, "b"), "code");
    ASSERT_EQ(base.leaves.size(), 3U);
    const auto* const x = LeafAt(base, "x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->time_mode, mt::LeafTimeMode::BootRelative) << "code set no mode for x";
    EXPECT_EQ(StringAt(x->resource, "k1"), "file");
    EXPECT_EQ(StringAt(x->resource, "k2"), "code");
    const auto* const y = LeafAt(base, "y");
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->time_mode, mt::LeafTimeMode::ConcentratorStamped);
    const auto* const z = LeafAt(base, "z");
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(StringAt(z->resource, "k"), "code");
}
