# Security Policy

## Supported versions

microtel is **pre-1.0**. No version is currently considered "stable" for security-support purposes. Once v1.0 ships, the support matrix below will list which versions receive security updates.

| Version | Status | Security updates |
|---|---|---|
| 0.x | pre-release | best-effort, no SLA |
| 1.x | TBD when 1.0 ships | TBD |

After v1.0:
- The most recent **minor release** of the latest **major version** is fully supported.
- The previous **major version** receives security fixes for **18 months** after the next major version ships (per the LTS policy in `microtel-roadmap.md` §2).
- Older versions are end-of-life. If you're running one, the recommended path is to upgrade.

## Reporting a vulnerability

**Please do not open a public GitHub issue for security vulnerabilities.**

Send a private report to: **security@microtel.example** *(placeholder — replace before first public release)*

Or, if GitHub Private Vulnerability Reporting is enabled on the repository, use that.

What to include:

- A description of the vulnerability and its impact.
- Steps to reproduce, ideally with a minimal proof-of-concept.
- The microtel version, build configuration, and platform.
- Whether you've already disclosed this elsewhere.

## What to expect

- **Acknowledgement within 72 hours** of receipt (best-effort pre-1.0).
- **Initial assessment within 7 days.**
- **Coordinated disclosure preferred.** We aim to publish a fix and advisory together.
- **CVE assignment** when warranted.
- **Credit** in the advisory unless you prefer to remain anonymous.

## Scope

The following are in scope:

- The microtel C++ runtime, including the trace SDK, OTLP encoder, OTLP/HTTP and OTLP/gRPC codecs, and HTTP/2 transport.
- The Python bindings (when shipped).
- The compatibility shims (`microtel_otelcpp_shim`, `microtel_python_shim`).
- The vendored upb code and the proto-generation pipeline (only insofar as a vulnerability requires changes in microtel's vendoring or wrapper code; vulnerabilities in upstream upb are reported to that project).
- The `microtelctl` control plane (when shipped, post-v1.1).
- Build and packaging scripts that affect installed artifacts.

## Out of scope

The following are **not** considered vulnerabilities for the purposes of this policy:

- Resource exhaustion caused by legitimate API use without bounds (e.g., creating millions of spans without configuring `max_total_queue_bytes`).
- Issues in third-party dependencies (nghttp2, OpenSSL, zlib) — please report those upstream.
- Configuration mistakes by operators (e.g., shipping with `insecure = true` in production).
- Self-XSS or attacks requiring physical access to the host.
- Theoretical issues without a demonstrable exploit path.

## Hardening guidance

For deployments handling sensitive telemetry:

- Build with `MICROTEL_FORBID_INSECURE_TLS=ON` to refuse `insecure = true` at runtime.
- Pin to a specific minor version and review release notes before upgrading.
- Run `microtel --preflight=connect` against your collector before deploying to validate TLS and connectivity.
- Use mTLS (`client_cert` + `client_key`) where the upstream collector supports it.
- Set explicit `max_*` limits in `microtel.toml` rather than relying on defaults.

## Hall of fame

Researchers who responsibly disclose vulnerabilities will be acknowledged here unless they prefer to remain anonymous.

*(empty — no advisories yet)*
