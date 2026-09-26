// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_options.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/resource.hpp"

#include "sdk/leaf_resource.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microtel::sdk
{
namespace
{

[[nodiscard]] ConfigError Invalid(std::string field, std::string message)
{
    return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                       .field = std::move(field),
                       .message = std::move(message)};
}

[[nodiscard]] std::size_t Bytes(const std::vector<KeyValue>& attributes) noexcept
{
    std::size_t total = 0;
    for (const auto& kv : attributes)
    {
        total += AttributeBytes(kv);
    }
    return total;
}

/// A configured Resource table's keys: none reserved, and not the id key.
[[nodiscard]] std::optional<ConfigError> CheckKeys(const std::vector<KeyValue>& resource,
                                                   const std::string& field,
                                                   std::string_view id_key)
{
    for (const auto& kv : resource)
    {
        if (kv.key.starts_with(kLeafReservedPrefix))
        {
            return Invalid(field,
                           "'" + kv.key +
                               "' is reserved for the leaf wire contract and cannot be configured");
        }
        if (!id_key.empty() && kv.key == id_key)
        {
            return Invalid(field,
                           "'" + kv.key +
                               "' is the leaf_id_attribute; the leaf id always overrides it, so "
                               "configuring it "
                               "could never take effect");
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> CheckLimits(const LeafReceiverOptions& o)
{
    if (o.max_payload_bytes == 0)
    {
        return Invalid("concentrator.max_payload_bytes", "must be greater than zero");
    }
    if (o.max_spans_per_payload == 0)
    {
        return Invalid("concentrator.max_spans_per_payload", "must be greater than zero");
    }
    if (o.max_leaves == 0)
    {
        return Invalid("concentrator.max_leaves", "must be greater than zero");
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> CheckLeaf(const LeafReceiverOptions& o,
                                                   const std::string& id,
                                                   const LeafConfig& leaf)
{
    if (id.empty() || id.size() > kMaxLeafIdBytes)
    {
        return Invalid("concentrator.leaves",
                       "a leaf id must be 1 to " + std::to_string(kMaxLeafIdBytes) + " bytes long");
    }
    const std::string field = "concentrator.leaves." + id + ".resource";
    if (auto err = CheckKeys(leaf.resource, field, o.leaf_id_attribute))
    {
        return err;
    }
    const Resource merged =
        Resource::Merge(Resource{o.leaf_defaults_resource}, Resource{leaf.resource});
    if (Bytes(merged.Attributes()) > o.max_leaf_resource_bytes)
    {
        return Invalid(field, "the leaf's configured Resource exceeds max_leaf_resource_bytes");
    }
    return std::nullopt;
}

}  // namespace

Expected<void, ConfigError> ValidateLeafReceiverOptions(const LeafReceiverOptions& options)
{
    if (!options.enabled)
    {
        return {};
    }
    if (auto err = CheckLimits(options))
    {
        return make_unexpected(std::move(*err));
    }
    const std::string defaults_field = "concentrator.leaf_defaults.resource";
    if (auto err =
            CheckKeys(options.leaf_defaults_resource, defaults_field, options.leaf_id_attribute))
    {
        return make_unexpected(std::move(*err));
    }
    if (Bytes(options.leaf_defaults_resource) > options.max_leaf_resource_bytes)
    {
        return make_unexpected(
            Invalid(defaults_field, "leaf_defaults.resource exceeds max_leaf_resource_bytes"));
    }
    for (const auto& [id, leaf] : options.leaves)
    {
        if (auto err = CheckLeaf(options, id, leaf))
        {
            return make_unexpected(std::move(*err));
        }
    }
    return {};
}

}  // namespace microtel::sdk
