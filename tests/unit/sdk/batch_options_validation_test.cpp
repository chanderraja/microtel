// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Issue #267 (ICP 0026 Discrepancy 1): `SdkBuilder::Build` and
// `Provider::SetBatchOptions` accept and reject exactly the same
// `BatchOptions`. One table of cases drives every door a `BatchOptions` can
// come through — `WithBatch` in code, the `[sdk]` table in `microtel.toml`,
// and the runtime setter — so the two validation surfaces cannot drift apart
// again without a test here going red.

#include "microtel/error.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mt = microtel;

using namespace std::chrono_literals;

namespace
{

constexpr std::string_view kEndpoint = "https://localhost:4318";
constexpr auto kShutdownTimeout = std::chrono::milliseconds(500);

/// One `BatchOptions` and what both surfaces must say about it. An empty
/// `field` means the value is accepted.
struct BatchCase
{
    std::string_view name;
    mt::BatchOptions opts;
    std::string_view field;
};

mt::BatchOptions Opts(std::uint32_t queue, std::uint32_t batch, std::chrono::milliseconds delay)
{
    return mt::BatchOptions{
        .max_queue_size = queue,
        .max_export_batch_size = batch,
        .schedule_delay = delay,
        .drop_policy = mt::DropPolicy::DropNewest,
    };
}

const std::vector<BatchCase>& Cases()
{
    static const std::vector<BatchCase> kCases = {
        // Rejected.
        {.name = "zero queue, zero batch", .opts = Opts(0, 0, 5s), .field = "sdk.max_queue_size"},
        {.name = "zero queue, default batch",
         .opts = Opts(0, 512, 5s),
         .field = "sdk.max_queue_size"},
        {.name = "zero batch", .opts = Opts(8192, 0, 5s), .field = "sdk.max_export_batch_size"},
        {.name = "batch larger than queue",
         .opts = Opts(10, 11, 5s),
         .field = "sdk.max_export_batch_size"},
        {.name = "zero delay", .opts = Opts(8192, 512, 0ms), .field = "sdk.schedule_delay_ms"},
        {.name = "negative delay", .opts = Opts(8192, 512, -1ms), .field = "sdk.schedule_delay_ms"},
        // Accepted — the boundaries next to each rejection.
        {.name = "smallest coherent", .opts = Opts(1, 1, 1ms), .field = ""},
        {.name = "batch equal to queue", .opts = Opts(10, 10, 5s), .field = ""},
        {.name = "defaults", .opts = mt::BatchOptions{}, .field = ""},
    };
    return kCases;
}

using BuildResult = mt::Expected<std::shared_ptr<mt::Provider>, mt::ConfigError>;

void ExpectAccepted(BuildResult& result, const BatchCase& c)
{
    ASSERT_TRUE(result.has_value()) << c.name << ": " << result.error().message;
    EXPECT_EQ((*result)->Shutdown(kShutdownTimeout), mt::Status::Completed) << c.name;
}

void ExpectRejected(const BuildResult& result, const BatchCase& c)
{
    ASSERT_FALSE(result.has_value()) << c.name << ": Build() accepted it";
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue) << c.name;
    EXPECT_EQ(result.error().field, c.field) << c.name;
}

/// Assert what `Build()` answered for @p c, shutting an accepted provider down.
void ExpectBuildVerdict(BuildResult& result, const BatchCase& c)
{
    if (c.field.empty())
    {
        ExpectAccepted(result, c);
    }
    else
    {
        ExpectRejected(result, c);
    }
}

/// Writes a `microtel.toml` carrying @p opts in its `[sdk]` table, and removes
/// it on destruction.
class TomlFile
{
public:
    explicit TomlFile(const mt::BatchOptions& opts)
        : m_path(std::filesystem::temp_directory_path() /
                 ("microtel_batch_validation_" + std::to_string(::getpid()) + ".toml"))
    {
        std::ofstream f{m_path};
        f << "[sdk]\n"
          << "max_queue_size = " << opts.max_queue_size << "\n"
          << "max_export_batch_size = " << opts.max_export_batch_size << "\n"
          << "schedule_delay_ms = " << opts.schedule_delay.count() << "\n";
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

}  // namespace

TEST(BatchOptionsValidation, BuildFromCodeRejectsExactlyTheIncoherentValues)
{
    for (const auto& c : Cases())
    {
        auto result =
            mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).WithBatch(c.opts).Build();
        ExpectBuildVerdict(result, c);
    }
}

TEST(BatchOptionsValidation, BuildFromTomlRejectsExactlyTheIncoherentValues)
{
    for (const auto& c : Cases())
    {
        const TomlFile file{c.opts};
        auto result =
            mt::SdkBuilder().FromFile(file.Path()).WithEndpoint(std::string{kEndpoint}).Build();
        ExpectBuildVerdict(result, c);
    }
}

TEST(BatchOptionsValidation, SetterAgreesWithBuild)
{
    // Its own profile name: it stays live while each case builds a "default".
    auto built =
        mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).WithProfileName("setter").Build();
    ASSERT_TRUE(built.has_value()) << built.error().message;
    auto& provider = **built;

    for (const auto& c : Cases())
    {
        const mt::Status expected =
            c.field.empty() ? mt::Status::Completed : mt::Status::InvalidArgument;
        EXPECT_EQ(provider.SetBatchOptions(c.opts), expected) << c.name;

        auto build =
            mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).WithBatch(c.opts).Build();
        EXPECT_EQ(build.has_value(), c.field.empty()) << c.name;
        if (build.has_value())
        {
            EXPECT_EQ((*build)->Shutdown(kShutdownTimeout), mt::Status::Completed) << c.name;
        }
    }
    EXPECT_EQ(provider.Shutdown(kShutdownTimeout), mt::Status::Completed);
}

// The batch settings have no environment variable (docs/configuration.md §3.7):
// the `OTEL_BSP_*` names are read by nothing, so a zero there can neither
// reach the processor nor fail `Build()`. Pinned so that wiring them up later
// is a deliberate change that has to route through the same validator.
TEST(BatchOptionsValidation, OtelBspEnvVarsAreNotRead)
{
    constexpr std::string_view kVars[] = {
        "OTEL_BSP_MAX_QUEUE_SIZE",
        "OTEL_BSP_MAX_EXPORT_BATCH_SIZE",
        "OTEL_BSP_SCHEDULE_DELAY",
    };
    for (const auto var : kVars)
    {
        ::setenv(std::string{var}.c_str(), "0", /*overwrite=*/1);
    }
    auto result = mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).Build();
    for (const auto var : kVars)
    {
        ::unsetenv(std::string{var}.c_str());
    }
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->Shutdown(kShutdownTimeout), mt::Status::Completed);
}
