// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"

#include <chrono>
#include <memory>
#include <string_view>

namespace microtel::sdk
{

/// @brief All owned objects passed to SdkProvider at construction.
///
/// Bundles the ten pipeline components so the constructor stays within the
/// seven-parameter tidy threshold. Declaration order matches the member
/// declaration order in SdkProvider (reverse-destruction semantics).
struct SdkProviderArgs
{
    std::unique_ptr<internal::IOtlpEncoder> encoder;
    std::unique_ptr<internal::IAuthProvider> auth;
    std::unique_ptr<internal::ITransport> transport;
    std::unique_ptr<internal::IWireCodec> codec;
    std::unique_ptr<internal::IExporter> exporter;
    std::unique_ptr<internal::ISpanProcessor> processor;
    std::shared_ptr<const Resource> resource;
    SamplerHandle sampler;
    SpanLimitOptions span_limits;
    internal::ConnectOptions connect_opts;
};

/// @brief Production `Provider` wiring the full export pipeline.
///
/// Owns the pipeline end-to-end: encoder → transport → codec → exporter →
/// processor. Members are declared so that reverse-destruction (i.e. the
/// compiler-generated destructor) tears down the processor first, then the
/// exporter, then the codec and transport, preserving the happens-before chain
/// required by TSAN and the threading model (interfaces.md §6).
///
/// @threadsafety Thread-safe. All methods may be called from any thread.
class SdkProvider final : public microtel::Provider
{
public:
    explicit SdkProvider(SdkProviderArgs args) noexcept;

    ~SdkProvider() noexcept override;

    SdkProvider(const SdkProvider&) = delete;
    SdkProvider& operator=(const SdkProvider&) = delete;
    SdkProvider(SdkProvider&&) = delete;
    SdkProvider& operator=(SdkProvider&&) = delete;

    [[nodiscard]] std::shared_ptr<Tracer> GetTracer(std::string_view name,
                                                    std::string_view version = {}) override;

    [[nodiscard]] Expected<void, Error> Connect() override;

    [[nodiscard]] Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] HealthSnapshot GetExporterHealth() const noexcept override;

private:
    // Declared first → destroyed last. Encoder is stateless; no teardown order concern.
    std::unique_ptr<internal::IOtlpEncoder> m_encoder;
    std::unique_ptr<internal::IAuthProvider> m_auth;
    // Transport owns the I/O thread; must outlive codec and exporter.
    std::unique_ptr<internal::ITransport> m_transport;
    std::unique_ptr<internal::IWireCodec> m_codec;
    // Exporter owns the export worker thread; must outlive codec and transport.
    std::unique_ptr<internal::IExporter> m_exporter;
    // Processor owns the batch worker thread; declared last → destroyed first.
    std::unique_ptr<internal::ISpanProcessor> m_processor;

    std::shared_ptr<const Resource> m_resource;
    SamplerHandle m_sampler;
    SpanLimitOptions m_span_limits;
    internal::ConnectOptions m_connect_opts;
};

}  // namespace microtel::sdk
