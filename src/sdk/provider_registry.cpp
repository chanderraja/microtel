// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/provider_registry.hpp"

#include "microtel/provider.hpp"

#include "sdk/sdk_provider.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string_view>

#include <pthread.h>

namespace microtel::sdk
{

namespace
{

/// The slots, and the once-flag guarding the atfork registration.
///
/// A `pthread_atfork` handler takes no arguments, so the providers it must
/// reach have to be reachable from a global. Both are only ever touched
/// atomically or through `call_once`.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
Registry g_slots{};
std::once_flag g_atfork_once;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Runs in the child after `fork()`. Marks **every** live provider dead and
/// empties its slot.
///
/// Async-signal-safe by the same argument the single-slot version made, now
/// bounded at `kMaxProfiles` iterations: `MarkForkedChild` does nothing but one
/// atomic store, and no name is read, so no `std::string` is touched in the
/// child.
///
/// Clearing is deliberate. The child's supported move is to re-`Build()`
/// (`docs/sequences/fork-survival.md`, option A), naturally under the same
/// profile names, which would collide with the stale parent-era entries. An
/// empty registry is exactly what a fresh process has. The cost is that
/// `GetProvider(name)` in a child answers nullptr rather than handing back a
/// shut-down provider — a defined, checkable outcome rather than a confusing
/// name collision at re-init (ICP 0027 §3).
extern "C" void ForkChildHandler() noexcept
{
    for (auto& slot : g_slots)
    {
        if (auto* const provider = slot.load(std::memory_order_acquire); provider != nullptr)
        {
            provider->MarkForkedChild();
            slot.store(nullptr, std::memory_order_release);
        }
    }
}

/// Registered once, at the first registration.
///
/// No prepare or parent handler. `docs/threading-model.md` §7 asks the parent
/// handler to "record a diagnostic that fork was observed", but there is nothing
/// to record it to: `LogImpl` is not async-signal-safe (it takes a mutex and may
/// call an application sink), and no `DropReason` covers it. Registering an
/// empty handler would only obscure that.
void InstallForkHandlersOnce() noexcept
{
    try
    {
        std::call_once(g_atfork_once,
                       [] { (void)::pthread_atfork(nullptr, nullptr, &ForkChildHandler); });
    }
    // Losing fork-safety must not fail registration, and there is nothing to
    // handle: pthread_atfork does not throw, so this is unreachable in practice
    // and exists to keep the noexcept promise.
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (const std::exception&)
    {
    }
}

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

RegistrationResult RegisterProvider(SdkProvider* provider) noexcept
{
    InstallForkHandlersOnce();

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
