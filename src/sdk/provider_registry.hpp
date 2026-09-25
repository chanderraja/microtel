// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace microtel::sdk
{

class SdkProvider;

/// @brief Live providers a process may hold at once.
///
/// One edit here changes the capacity; nothing else in the tree hard-codes a
/// profile count, and no header a consumer includes names it — so raising it is
/// a rebuild, not an ICP, and not a consumer recompile either. The number is a
/// judgement call: the deployments multi-profile is for — audit telemetry split
/// from operational telemetry, or a fan-out to a local collector and a vendor
/// endpoint — want two or three. Guessing low costs a `ProfileLimitExceeded` at
/// startup; guessing high costs eight pointers of BSS
/// ([ICP 0027](../../docs/icps/0027-multi-profile-threading.md), decision 1).
inline constexpr std::size_t kMaxProfiles = 8;

/// @brief Slots are plain atomic pointers and nothing else.
///
/// The name lives in the provider (`SdkProvider::ProfileName()`, immutable from
/// before the slot is published until the provider is destroyed), so the fork
/// child handler never dereferences a `std::string` and the registry never
/// allocates.
///
/// A `std::map` or `std::vector` could not be used: the `pthread_atfork` child
/// handler has to walk this, and a container is mutated under a lock, allocated
/// by an allocator that may itself be mid-operation at `fork()` time, and
/// iterated by something other than a bounded sequence of atomic loads. The
/// array gives the handler `kMaxProfiles` loads and stores with no allocation,
/// no lock, and a compile-time bound — which is the whole reason to accept a
/// capacity limit at all.
using Registry = std::array<std::atomic<SdkProvider*>, kMaxProfiles>;

/// @brief Why a registration did or did not take a slot.
enum class RegistrationResult : std::uint8_t
{
    /// The provider owns a slot, and `GetProvider(name)` finds it.
    Registered = 0,
    /// Another live provider already carries this profile name. Nothing was
    /// changed, and the other provider keeps the name — never last-wins.
    DuplicateName = 1,
    /// Every one of the `kMaxProfiles` slots is taken.
    CapacityExhausted = 2,
};

/// @brief Claim a slot for @p provider under its own profile name.
///
/// Called by `SdkBuilder::Build` after construction — not by `SdkProvider`'s
/// constructor, which is `noexcept` and could not report this failure. It does
/// not install the `pthread_atfork` child handler that walks this registry:
/// that is done once while the library loads, so no registration has a window
/// a `fork()` could strand (issue #271).
///
/// Lock-free: a duplicate-name scan, then `compare_exchange_strong(nullptr,
/// provider)` on the first empty slot, then a re-scan in which the **lowest
/// index wins**, so two concurrent registrations of one name produce exactly one
/// registered provider and one `DuplicateName` — with no lock and no ABA,
/// because a slot only ever goes null → provider → null and a provider address
/// is claimed once.
///
/// @param provider borrowed, non-owning, must be non-null. The registry never
///        owns and never extends a provider's lifetime; the slot's occupancy is
///        strictly shorter than the provider's life.
/// @return what happened, per `RegistrationResult`.
///
/// @threadsafety Thread-safe, lock-free, allocation-free.
[[nodiscard]] RegistrationResult RegisterProvider(SdkProvider* provider) noexcept;

/// @brief Release the slot @p provider holds, if it holds one.
///
/// Called at the **top** of `~SdkProvider`, before `Shutdown` — which is what
/// makes the fork handler safe: a slot is emptied before its provider begins
/// tearing down, so a handler running in a child sees either a provider that is
/// entirely intact or nothing at all. A provider that never registered (a
/// duplicate-name `Build`, a direct construction in a test) is a no-op.
///
/// @param provider borrowed, non-owning; may be a provider that is not
///        registered.
///
/// @threadsafety Thread-safe, lock-free, allocation-free.
void DeregisterProvider(SdkProvider* provider) noexcept;

/// @brief Mark every registered provider dead and empty every slot.
///
/// The body of the `pthread_atfork` child handler, exposed because a forked
/// child that `_exit`s cannot report which slots it touched: a test that forks
/// proves the handler is *wired*, and this proves what it does. Production code
/// outside the handler has no reason to call it.
///
/// Async-signal-safe, which is not a style note but the contract: `kMaxProfiles`
/// acquire-loads, one relaxed-release store per live provider
/// (`SdkProvider::MarkForkedChild`), and one release-store per slot. No lock, no
/// allocation, and no `std::string` read — the names are not consulted.
///
/// @threadsafety Safe to call from a fork child handler; see ICP 0027 §3.
void MarkForkedChildProviders() noexcept;

/// @brief How many times `MarkForkedChildProviders` has run in this process
///        image.
///
/// A test seam, and the only way to count handler registrations from outside:
/// every registration of the `pthread_atfork` child handler runs the sweep once
/// per `fork()`, and nothing else in a child runs it, so a child that reads this
/// and subtracts the value its parent saw learns how many copies are installed
/// — none, one, or a double registration. Production code has no reason to
/// call it.
///
/// @return the count, inherited across `fork()` like any other memory.
///
/// @threadsafety Thread-safe, lock-free.
[[nodiscard]] std::size_t ForkSweepRuns() noexcept;

/// @brief Find the live provider registered under @p name.
///
/// The implementation behind `microtel::GetProvider`; see that function's
/// Doxygen in `microtel/provider.hpp` for the lifetime contract.
///
/// @param name compared byte-for-byte against each slot's provider name.
/// @return a borrowed, non-owning pointer, or nullptr.
///
/// @threadsafety Thread-safe, lock-free, allocation-free.
[[nodiscard]] SdkProvider* FindProvider(std::string_view name) noexcept;

}  // namespace microtel::sdk
