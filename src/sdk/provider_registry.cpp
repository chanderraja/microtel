// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/provider_registry.hpp"

#include "microtel/provider.hpp"

#include "sdk/sdk_provider.hpp"

#include <atomic>
#include <cstddef>
#include <string_view>

#include <pthread.h>

namespace microtel::sdk
{

namespace
{

/// The slots, and a count of fork sweeps run (`ForkSweepRuns`).
///
/// A `pthread_atfork` handler takes no arguments, so the providers it must
/// reach have to be reachable from a global. Both are only ever touched
/// atomically.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
Registry g_slots{};
std::atomic<std::size_t> g_fork_sweep_runs{0};  // NOSONAR(cpp:S5421) mutable by design, see above
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Runs in the child after `fork()`. Nothing but the sweep, so that what the
/// handler does is testable without forking (`MarkForkedChildProviders`).
extern "C" void ForkChildHandler() noexcept
{
    MarkForkedChildProviders();
}

/// Registers the child handler. Called exactly once per load of this object,
/// by the static initialiser below — never from `RegisterProvider`.
///
/// Why at load, and not lazily at the first registration (issue #271). A lazy
/// install needs a once-guard, and every blocking once-guard — `std::call_once`,
/// `pthread_once`, a function-local `static` — has the same hole: a `fork()`
/// while another thread is inside the guarded call leaves the child's guard
/// "in progress" with no thread left to finish it, so the child's first
/// `Build()` — the recovery §7 supports — blocks forever. A non-blocking
/// compare-exchange avoids the hang but not the loser's problem: it either
/// registers without the handler in place yet, or waits, which is the hang
/// again. Installing during static initialisation leaves no guard at all:
/// nothing waits, and the initialiser runs once, so the handler cannot be
/// registered twice (a double registration would run the sweep twice per
/// fork). Static initialisation normally runs before `main`, with no other
/// thread to race it; loaded by `dlopen`, it runs inside `dlopen`, which a
/// `Build()` from this library cannot precede. A process that never builds a
/// provider pays one sweep of an empty array per `fork()`.
///
/// No prepare or parent handler. `docs/threading-model.md` §7 asks the parent
/// handler to "record a diagnostic that fork was observed", but there is nothing
/// to record it to: `LogImpl` is not async-signal-safe (it takes a mutex and may
/// call an application sink), and no `DropReason` covers it. Registering an
/// empty handler would only obscure that.
[[nodiscard]] bool InstallForkHandlers() noexcept
{
    return ::pthread_atfork(nullptr, nullptr, &ForkChildHandler) == 0;
}

/// The one call to `InstallForkHandlers`. Its value is unused: an `ENOMEM`
/// from `pthread_atfork` at load leaves the process without fork-safety, and
/// there is nowhere to report that before `main`.
[[maybe_unused]] const bool kForkHandlersInstalled = InstallForkHandlers();

/// True when a slot below @p limit holds a live provider named @p name.
///
/// Called twice by `RegisterProvider`: once over the whole array, to settle the
/// ordinary sequential duplicate before anything is claimed, and once over the
/// slots *below* the one just claimed, which is where lowest-index-wins is
/// decided.
[[nodiscard]] bool NameIsTaken(std::string_view name, std::size_t limit) noexcept
{
    for (std::size_t i = 0; i < limit; ++i)
    {
        const auto* const other = g_slots.at(i).load(std::memory_order_acquire);
        if (other != nullptr && other->ProfileName() == name)
        {
            return true;
        }
    }
    return false;
}

/// Claim the first empty slot for @p provider, or `kMaxProfiles` if full.
[[nodiscard]] std::size_t ClaimEmptySlot(SdkProvider* provider) noexcept
{
    for (std::size_t i = 0; i < kMaxProfiles; ++i)
    {
        SdkProvider* expected = nullptr;
        if (g_slots.at(i).compare_exchange_strong(
                expected, provider, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return i;
        }
    }
    return kMaxProfiles;
}

}  // namespace

void MarkForkedChildProviders() noexcept
{
    g_fork_sweep_runs.fetch_add(1);

    // Clearing is deliberate, and it is a trade. The child's supported move is
    // to re-`Build()` (`docs/sequences/fork-survival.md`, option A), naturally
    // under the same profile names, which would collide with the stale
    // parent-era entries; an empty registry is exactly what a fresh process
    // has. The cost is that `GetProvider(name)` in a child answers nullptr
    // rather than handing back a shut-down provider — a defined, checkable
    // outcome rather than a confusing name collision at re-init (ICP 0027 §3).
    for (auto& slot : g_slots)
    {
        if (auto* const provider = slot.load(std::memory_order_acquire); provider != nullptr)
        {
            provider->MarkForkedChild();
            slot.store(nullptr, std::memory_order_release);
        }
    }
}

RegistrationResult RegisterProvider(SdkProvider* provider) noexcept
{
    const std::string_view name = provider->ProfileName();
    if (NameIsTaken(name, kMaxProfiles))
    {
        return RegistrationResult::DuplicateName;
    }

    const std::size_t claimed = ClaimEmptySlot(provider);
    if (claimed == kMaxProfiles)
    {
        return RegistrationResult::CapacityExhausted;
    }

    // Another registration of the same name may have claimed a slot between the
    // scan above and the claim. Lowest index wins, deterministically: whoever
    // sees a same-named peer below them backs out, and the lowest claimant sees
    // none and keeps the name.
    if (NameIsTaken(name, claimed))
    {
        g_slots.at(claimed).store(nullptr, std::memory_order_release);
        return RegistrationResult::DuplicateName;
    }
    return RegistrationResult::Registered;
}

void DeregisterProvider(SdkProvider* provider) noexcept
{
    for (auto& slot : g_slots)
    {
        // Cannot be a pointer-to-const: compare_exchange_strong takes its
        // expected value by mutable reference.
        // NOLINTNEXTLINE(misc-const-correctness)
        SdkProvider* expected = provider;
        if (slot.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return;
        }
    }
}

std::size_t ForkSweepRuns() noexcept
{
    return g_fork_sweep_runs.load();
}

SdkProvider* FindProvider(std::string_view name) noexcept
{
    for (auto& slot : g_slots)
    {
        if (auto* const provider = slot.load(std::memory_order_acquire);
            provider != nullptr && provider->ProfileName() == name)
        {
            return provider;
        }
    }
    return nullptr;
}

}  // namespace microtel::sdk

namespace microtel
{

Provider* GetProvider(std::string_view name) noexcept
{
    return sdk::FindProvider(name);
}

}  // namespace microtel
