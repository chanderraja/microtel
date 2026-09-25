// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/retry_engine.hpp"

#include "microtel/error.hpp"
#include "microtel/provider.hpp"

#include "exporter/retry_policy.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::exporter
{

RetryEngine::RetryEngine(RetryPolicyConfig policy,
                         internal::IDiagnosticsSink* diag,
                         internal::ISteadyClock* clock) noexcept
    : m_policy(policy),
      m_diag(diag),
      m_clock(clock),
      m_rng(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()))
{
}

void RetryEngine::Settle(const internal::WireResult& first_attempt,
                         const RetryAttempt& retry,
                         std::string_view failure_stage)
{
    RecordOutcome(Resolve(first_attempt, retry), failure_stage);
}

void RetryEngine::Abort() noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        m_aborted = true;
    }
    m_cv.notify_all();
}

internal::WireResult RetryEngine::Resolve(const internal::WireResult& first_attempt,
                                          const RetryAttempt& retry)
{
    if (first_attempt.success || !first_attempt.retryable)
    {
        return first_attempt;
    }
    auto retried = RunRetryLoop(retry);
    if (!retried.has_value())
    {
        // The retry budget was already spent on entry, so no further attempt
        // was made and the fan-out result stands as this batch's outcome.
        return first_attempt;
    }
    if (retried->success && m_diag != nullptr)
    {
        // Not a loss — the batch was delivered — but the export path is
        // unhealthy, and this counter is the only place that shows it.
        m_diag->RecordDrop(DropReason::RetryableFailureRecovered);
    }
    return std::move(*retried);
}

std::optional<internal::WireResult> RetryEngine::RunRetryLoop(const RetryAttempt& retry)
{
    const std::uint32_t max_attempts = (m_policy.max_attempts > 0U) ? m_policy.max_attempts : 1U;
    const auto budget_deadline = ClockNow() + m_policy.retry_budget;

    // The fan-out already made attempt 0. If the budget is spent before the
    // first retry, make none: the caller's own result is the outcome.
    if (ClockNow() >= budget_deadline)
    {
        return std::nullopt;
    }

    std::optional<internal::WireResult> last;
    for (std::uint32_t attempt = 1U; attempt < max_attempts; ++attempt)
    {
        last = retry();
        if (last->success || !last->retryable || attempt + 1U >= max_attempts)
        {
            break;
        }
        const auto backoff = ComputeBackoff(attempt, m_policy, last->retry_after, DrawJitter01());
        // Look-ahead, per `docs/sequences/retry-after-failure.md` §4: exit when
        // the *upcoming* sleep would reach or pass the budget, not once the
        // budget is already spent (issue #195). `backoff` is never negative.
        if (ClockNow() + backoff >= budget_deadline || !SleepUnlessAborted(backoff))
        {
            break;
        }
    }
    return last;
}

bool RetryEngine::SleepUnlessAborted(std::chrono::milliseconds backoff)
{
    // A condition-variable wait rather than `sleep_for`, so that `Shutdown`
    // can end it: an uninterruptible sleep held `Shutdown`'s join, and the
    // destructor's, for the rest of the backoff schedule (issue #310).
    std::unique_lock lock{m_mu};
    return !m_cv.wait_for(lock, backoff, [this] { return m_aborted; });
}

void RetryEngine::RecordOutcome(const internal::WireResult& result,
                                std::string_view failure_stage) noexcept
{
    if (m_diag == nullptr)
    {
        return;
    }
    if (result.success)
    {
        if (result.partial_success_rejected > 0)
        {
            // Delivered, but the collector kept only some of it. Never
            // retried (error-model.md §6), so this is the only account of it.
            m_diag->RecordDrop(DropReason::PartialSuccessRejection,
                               result.partial_success_rejected);
        }
        m_diag->RecordBatchSent();
        return;
    }
    // This funnel runs exactly once per batch, after every retry has been
    // resolved, so `retryable` here means "retried and still lost" rather
    // than "will be retried". Attempt exhaustion, budget exhaustion and a
    // retry cut short by Shutdown are the same outcome to an operator and
    // share one counter.
    m_diag->RecordDrop(result.retryable ? DropReason::RetryBudgetExhausted
                                        : DropReason::NonRetryableFailure);
    // A codec may report failure without populating `error`. Recording an
    // empty message would leave GetExporterHealth() saying a batch failed and
    // refusing to say why, so name the stage instead.
    m_diag->RecordBatchFailed(result.error.value_or(
        Error{.kind = Error::Kind::Network, .message = std::string{failure_stage}}));
}

internal::TimePointSteady RetryEngine::ClockNow() const noexcept
{
    if (m_clock != nullptr)
    {
        return m_clock->Now();
    }
    return std::chrono::steady_clock::now();
}

double RetryEngine::DrawJitter01() noexcept
{
    std::uniform_real_distribution<double> dist{0.0, 1.0};
    return dist(m_rng);
}

}  // namespace microtel::exporter
