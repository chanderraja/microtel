// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Pins the opentelemetry-cpp API configuration the shim is built against.
//
// These are compile-time assertions by design. The shim's entire viability
// rests on one configuration choice — `OPENTELEMETRY_STL_VERSION=2020` — and the
// failure mode of getting it wrong is silent: with the upstream default
// (`WITH_STL=OFF`), `nostd::` resolves to a *vendored abseil snapshot bundled
// inside opentelemetry-cpp's own API headers*, which would drag abseil into
// every translation unit that includes them. `WITH_ABSEIL=OFF` does not prevent
// this; only the STL mode does. See ICP 0014 §"The constraint that shapes the
// design".
//
// If someone changes the STL mode, this file stops compiling — which is the
// point. A runtime test could not catch it.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/version.h>

namespace
{

namespace nostd = opentelemetry::nostd;

// ── The configuration itself ──────────────────────────────────────────────────

static_assert(OPENTELEMETRY_STL_VERSION == 2020,
              "The shim requires OPENTELEMETRY_STL_VERSION=2020. Without it the "
              "opentelemetry-cpp API falls back to its vendored abseil snapshot "
              "and abseil enters the include graph (ICP 0014).");

static_assert(OPENTELEMETRY_ABI_VERSION_NO == 1,
              "The shim pins otel-cpp ABI version 1. The API embeds this in an "
              "inline namespace, so changing it changes every mangled signature.");

// ── nostd:: must alias std:: exactly ──────────────────────────────────────────
//
// Each of these would be a distinct vendored type under the default config.
// Equality with the std type is what proves abseil is not in play, and it is
// also what makes the shim's translation layer trivial: microtel's own public
// API already speaks these exact types.

static_assert(std::is_same_v<nostd::string_view, std::string_view>,
              "nostd::string_view must be std::string_view");

static_assert(std::is_same_v<nostd::shared_ptr<int>, std::shared_ptr<int>>,
              "nostd::shared_ptr must be std::shared_ptr");

static_assert(std::is_same_v<nostd::unique_ptr<int>, std::unique_ptr<int>>,
              "nostd::unique_ptr must be std::unique_ptr");

static_assert(std::is_same_v<nostd::variant<bool, std::int64_t, double>,
                             std::variant<bool, std::int64_t, double>>,
              "nostd::variant must be std::variant — under the default config "
              "this is absl::variant from the vendored snapshot");

static_assert(std::is_same_v<nostd::span<const int>, std::span<const int>>,
              "nostd::span must be std::span (and WITH_GSL must be off, or this "
              "would be gsl::span instead)");

// ── The mapping the shim relies on ────────────────────────────────────────────
//
// microtel's AttributeValue is std::variant<bool, int64_t, double, std::string,
// ...>. Because nostd::variant is std::variant, the shim converts between the
// two without a type-erasure layer. This assertion documents that dependency so
// a future config change surfaces here rather than deep in L2's conversion code.

using OtelScalar = nostd::variant<bool, std::int64_t, double, std::string_view>;
static_assert(std::is_same_v<std::variant_alternative_t<0, OtelScalar>, bool>);
static_assert(std::is_same_v<std::variant_alternative_t<1, OtelScalar>, std::int64_t>);

TEST(OtelCppConfig, ApiHeadersAreConsumable)
{
    // The static_asserts above carry the real weight. This body exists so the
    // translation unit is linked and run, proving the headers are consumable in
    // a normal gtest target rather than only in a compile-only probe.
    const nostd::string_view sv{"microtel"};
    EXPECT_EQ(sv, std::string_view{"microtel"});
}

TEST(OtelCppConfig, VariantRoundTripsThroughStdVisit)
{
    // std::visit working on a nostd::variant is the practical consequence of the
    // aliasing above, and is what L2's attribute conversion will use.
    const nostd::variant<bool, std::int64_t, double> value{std::int64_t{42}};

    const auto as_int = std::visit(
        [](auto&& held) -> std::int64_t
        {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, std::int64_t>)
            {
                return held;
            }
            else
            {
                return -1;
            }
        },
        value);

    EXPECT_EQ(as_int, 42);
}

}  // namespace
