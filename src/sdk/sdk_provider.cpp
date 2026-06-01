// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_provider.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "sdk/sdk_tracer.hpp"

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

}  // namespace

SdkProvider::SdkProvider(SdkProviderArgs args) noexcept
    : m_encoder(std::move(args.encoder)),
      m_auth(std::move(args.auth)),
      m_transport(std::move(args.transport)),
      m_codec(std::move(args.codec)),
      m_exporter(std::move(args.exporter)),
      m_processor(std::move(args.processor)),
      m_resource(std::move(args.resource)),
      m_sampler(std::move(args.sampler)),
      m_span_limits(args.span_limits),
      m_connect_opts(std::move(args.connect_opts))
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
    // Two-stage flush: drain the BSP queue into the exporter queue first,
    // then drain the exporter queue (actual HTTP sends). Both are async
    // workers; flushing only the processor leaves batches undelivered.
    const Status s = m_processor->ForceFlush(timeout);
    if (s != Status::Completed)
    {
        return s;
    }
    return m_exporter->ForceFlush(timeout);
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
