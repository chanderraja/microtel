// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

/// @brief A retunable `ISampler::Description()` that never invalidates a view
/// it has already handed out.
///
/// `Description()` returns a borrowed `std::string_view`, so the string behind
/// it may not be mutated or freed while a concurrent reader still holds the
/// view. A `std::string` member cannot be reassigned under that rule, and a
/// reader-writer lock would put a lock on `Description()`, which is called
/// from diagnostics on live samplers.
///
/// So the strings are **append-only**: every accepted retune pushes a fresh
/// rendering onto a `std::deque` (whose element references are stable across
/// growth) and publishes a pointer to it. Every view previously returned stays
/// valid for the slot's life. `Get()` is one acquire load and takes no lock;
/// `Publish()` takes `m_mu`, which is therefore a **leaf** — nothing else is
/// acquired while it is held, so a composite may hold its own slot's lock
/// while reading a child's description (`docs/threading-model.md` §4 rule 2).
///
/// The bound on growth is one short string per *accepted* retune, which is
/// operator cadence, not a hot path (ICP 0026 §5).
///
/// @threadsafety Thread-safe.
class DescriptionSlot
{
public:
    /// @param initial the description before any retune. Moved in.
    ///
    /// Not `noexcept`: a sampler that cannot render its own name at
    /// construction has failed to construct, which is a different situation
    /// from one that cannot render a retune.
    explicit DescriptionSlot(std::string initial)
    {
        m_strings.push_back(std::move(initial));
        m_current.store(&m_strings.back(), std::memory_order_release);
    }

    DescriptionSlot(const DescriptionSlot&) = delete;
    DescriptionSlot& operator=(const DescriptionSlot&) = delete;
    DescriptionSlot(DescriptionSlot&&) = delete;
    DescriptionSlot& operator=(DescriptionSlot&&) = delete;
    ~DescriptionSlot() noexcept = default;

    /// @brief The current description.
    ///
    /// @return a borrowed view, valid for this slot's lifetime even across
    ///         later `Publish` calls.
    [[nodiscard]] std::string_view Get() const noexcept
    {
        return *m_current.load(std::memory_order_acquire);
    }

    /// @brief Render a new description and make it current.
    ///
    /// The composer runs **under this slot's lock**, which keeps two concurrent
    /// retunes from publishing in one order and rendering in another. That is
    /// safe for a composite recomposing from its children, because reading a
    /// child's `Description()` takes no lock — so this mutex acquires nothing
    /// and stays a leaf.
    ///
    /// Allocation failure is swallowed, deliberately. This is the last step of
    /// a retune that has already taken effect, and a description one revision
    /// stale is a better outcome than a `TrySetRatio` reporting a failure that
    /// did not happen. It is also the only reason the whole path is `noexcept`
    /// rather than each caller carrying its own guard.
    ///
    /// @param compose callable returning the new `std::string`. Called once.
    template <typename Compose>
    void Publish(Compose&& compose) noexcept
    {
        try
        {
            const std::scoped_lock lock{m_mu};
            m_strings.push_back(compose());
            m_current.store(&m_strings.back(), std::memory_order_release);
        }
        catch (const std::exception&)
        {
            // Diagnostics, not state: the retune stands, its rendering does not.
        }
    }

private:
    mutable std::mutex m_mu;  ///< leaf; taken only by `Publish`
    /// Append-only. `std::deque` is chosen over `std::vector` for its stable
    /// element references: a `push_back` must not move the string a concurrent
    /// reader is looking at.
    std::deque<std::string> m_strings;
    std::atomic<const std::string*> m_current{nullptr};
};

}  // namespace microtel::sdk
