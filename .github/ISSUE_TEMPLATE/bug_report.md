---
name: Bug report
about: Report a problem with microtel
title: "[bug] "
labels: bug
---

<!--
Before filing: please search existing issues to avoid duplicates.

For SECURITY vulnerabilities, do NOT use this form. See SECURITY.md.
-->

## Description

<!-- One paragraph: what's wrong? -->

## Steps to reproduce

<!-- Minimal reproduction. Code or commands. Self-contained if possible. -->

```cpp
// minimal code
```

## Expected behavior

<!-- What should have happened. -->

## Actual behavior

<!-- What happened instead. Include relevant logs, stack traces, sanitizer output. -->

```
<logs / sanitizer output>
```

## Environment

- **microtel version (or commit SHA):** <!-- e.g. v0.1.0, or 1a2b3c4 -->
- **C++ compiler and version:** <!-- e.g. gcc 12.2, clang 17 -->
- **Build configuration:** <!-- Debug / Release / RelWithDebInfo, sanitizers if any -->
- **Platform:** <!-- Linux x86_64 / Linux ARM64 -->
- **OpenSSL version (output of `openssl version`):**
- **Collector version (if applicable):** <!-- otelcol --version -->
- **Wire protocol:** <!-- OTLP/HTTP / OTLP/gRPC -->

## Configuration (sanitized)

<!-- If applicable, paste relevant parts of microtel.toml or env vars.
     Redact any secrets (`Authorization` headers, tokens, etc.). -->

```toml
[exporter]
endpoint = "..."
protocol = "..."
```

## Additional context

<!-- Anything else relevant — production vs test, scale, related changes,
     workarounds you've tried. -->
