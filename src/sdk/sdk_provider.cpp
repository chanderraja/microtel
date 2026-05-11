// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_provider.hpp"

#include "sdk/sdk_tracer.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

namespace
{

constexpr auto kProviderDestructorTimeout = std::chrono::milliseconds(5000);

}

SdkProvider::SdkProvider(std::unique_ptr<internal::IOtlpEncoder> encoder,
                         std::unique_ptr<internal::IAuthProvider> auth,
                         std::unique_ptr<internal::ITransport> transport,
                         std::unique_ptr<internal::IWireCodec> codec,
                         std::unique_ptr<internal::IExporter> exporter,
                         std::unique_ptr<internal::ISpanProcessor> processor,
                         std::shared_ptr<const Resource> resource,
                         SamplerHandle sampler,
                         SpanLimitOptions span_limits,
                         internal::ConnectOptions connect_opts) noexcept
    : m_encoder(std::move(encoder)),
      m_auth(std::move(auth)),
      m_transport(std::move(transport)),
      m_codec(std::move(codec)),
      m_exporter(std::move(exporter)),
      m_processor(std::move(processor)),
      m_resource(std::move(resource)),
      m_sampler(std::move(sampler)),
      m_span_limits(span_limits),
      m_connect_opts(std::move(connect_opts))
{
}

SdkProvider::~SdkProvider() noexcept
{
    (void)Shutdown(kProviderDestructorTimeout);
}

std::shared_ptr<Tracer> SdkProvider::GetTracer(std::string_view name, std::string_view version)
{
    return std::make_shared<SdkTracer>(
        m_sampler.Get(),
        m_processor.get(),
        m_resource,
        internal::InstrumentationScope{.name = std::string{name}, .version = std::string{version}},
        m_span_limits);
}

Expected<void, Error> SdkProvider::Connect()
{
    return m_transport->Connect(m_connect_opts);
}

Status SdkProvider::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    return m_processor->ForceFlush(timeout);
}

Status SdkProvider::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    const Status status = m_processor->Shutdown(timeout);
    (void)m_exporter->Shutdown(timeout);
    (void)m_transport->Close(timeout);
    return status;
}

HealthSnapshot SdkProvider::GetExporterHealth() const noexcept
{
    HealthSnapshot health;
    health.connection_state = m_transport->GetState();
    return health;
}

}  // namespace microtel::sdk
