// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/resource_detector.hpp"

#include <filesystem>
#include <memory>

namespace microtel
{

/// @brief Detector for the OTel `process.*` resource attributes.
///
/// Emits, on a stock Linux:
///
/// | Attribute | Type | Source |
/// |---|---|---|
/// | `process.pid` | int64 | `getpid(2)` |
/// | `process.executable.path` | string | `readlink(2)` on `/proc/self/exe` |
/// | `process.executable.name` | string | the file name of that path |
/// | `process.command` | string | `argv[0]` from `/proc/self/cmdline` |
/// | `process.command_args` | string[] | the whole `argv` from the same file |
///
/// `process.pid` is unconditional. The executable and command attributes are
/// best-effort: a restricted or namespaced `/proc` that hides the `exe` link,
/// and the zero-length `cmdline` a kernel thread has, both yield an omitted
/// attribute rather than a failure. `Detect` fails only when `/proc/self/cmdline`
/// cannot be opened at all, which means `/proc` is not mounted under `root` —
/// whether that failure stops `Build()` is the strict/lenient detector policy's
/// decision (`sdk.resource_detectors_strict`), not this detector's.
///
/// @param root prefix every path is resolved against. Defaults to `/`, the real
///        filesystem; tests pass a fixture tree. Not a chroot — it is string
///        composition, and `getpid(2)` is unaffected by it.
/// @return an owning detector, ready to pass to
///         `SdkBuilder::WithResourceDetector`.
///
/// @see microtel-spec.md §12.7, docs/interfaces.md §4.10
[[nodiscard]] std::unique_ptr<internal::IResourceDetector> MakeProcessDetector(
    std::filesystem::path root = "/");

/// @brief Detector for the OTel `host.*` resource attributes.
///
/// Emits, on a stock Linux:
///
/// | Attribute | Type | Source |
/// |---|---|---|
/// | `host.name` | string | `gethostname(2)` |
/// | `host.id` | string | `/etc/machine-id`, else `/var/lib/dbus/machine-id` |
///
/// The machine-id fallback covers non-systemd hosts, and an *empty*
/// `/etc/machine-id` — the unprovisioned state, a zero-length file rather than
/// an absent one — falls through to the D-Bus path as well. When neither file
/// yields a non-empty id, `host.id` is omitted: it is optional in the OTel
/// conventions and routinely absent inside containers. `Detect` fails only when
/// `gethostname(2)` itself fails.
///
/// @param root prefix every path is resolved against. Defaults to `/`, the real
///        filesystem; tests pass a fixture tree. `gethostname(2)` is unaffected
///        by it.
/// @return an owning detector, ready to pass to
///         `SdkBuilder::WithResourceDetector`.
///
/// @see microtel-spec.md §12.7, docs/interfaces.md §4.10
[[nodiscard]] std::unique_ptr<internal::IResourceDetector> MakeHostDetector(
    std::filesystem::path root = "/");

}  // namespace microtel
