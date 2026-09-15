# AuthCallback recipes — OAuth2 and AWS SigV4

**Status:** v1.1 documentation deliverable. The substitution
[ICP 0024](icps/0024-v1.1-rescope.md) made when it replaced built-in OAuth2 /
SigV4 providers with documented recipes over the shipped
`SdkBuilder::WithAuthProvider` surface.
**Companion:** [`configuration.md`](configuration.md) §3.3 (static headers vs.
runtime auth), [`interfaces.md`](interfaces.md) §4.9 (`IAuthProvider`
contract), [`threading-model.md`](threading-model.md) §2.2 (exporter workers),
[`error-model.md`](error-model.md) §3 (drop accounting).
**Last verified against source:** every signature below checked against
[`include/microtel/sdk_builder.hpp`](../include/microtel/sdk_builder.hpp) and
[`include/microtel/error.hpp`](../include/microtel/error.hpp); every behavioural
claim against
[`src/common/config/auth_providers.cpp`](../src/common/config/auth_providers.cpp),
[`src/wire/http/http_wire_codec.cpp`](../src/wire/http/http_wire_codec.cpp),
[`src/wire/grpc/grpc_wire_codec.cpp`](../src/wire/grpc/grpc_wire_codec.cpp) and
[`src/sdk/sdk_builder.cpp`](../src/sdk/sdk_builder.cpp).

**The snippets in this document are not compiled by CI.** No docs-snippet build
target exists in `ci/`. They are written against the real headers and their
signatures are verified by hand at the revision named above; treat them as
correct-at-writing reference code, not as a tested artifact.

---

## 1. Who this is for

You export OTLP to a backend that wants a credential microtel does not
generate: an OAuth2 client-credentials access token, or an AWS SigV4 request
signature. microtel ships no token client and no request signer — deliberately,
per ICP 0024 — and instead gives you one hook:

```cpp
/// include/microtel/sdk_builder.hpp
using AuthCallback = std::function<Expected<std::string, Error>()>;

SdkBuilder& WithAuthProvider(AuthCallback cb,
                             std::chrono::milliseconds cache_ttl = std::chrono::seconds(60));
```

The string you return becomes the `authorization` header value on the next
export batch, verbatim, on both the OTLP/HTTP and OTLP/gRPC paths. That is the
whole surface. Everything below is about using it without wrecking your export
pipeline, and about the one scheme (SigV4) it cannot express.

For a credential that never changes, do not use this at all — use
`WithHeaders({{.key = "authorization", .value = "Bearer …"}})`, which builds a
zero-allocation `StaticHeadersAuthProvider` and never calls user code.

---

## 2. What runs where — read this before writing a callback

Four facts about the callback's execution context. All four are load-bearing
and none is obvious from the signature.

**2.1 The callback runs on an exporter worker thread.** The wire codec calls
`IAuthProvider::GetAuthorization` while assembling the request headers, inside
`IWireCodec::Send`, which the exporter worker invokes synchronously
(`threading-model.md` §2.2). Whatever your callback does, the worker is not
draining its queue while it does it.

**2.2 One auth provider is shared by every pipeline.** `SdkBuilder::Build`
constructs a single provider and hands the same pointer to the trace, metric
and log codecs (`src/sdk/sdk_builder.cpp`, `BuildExporters`). A fully
configured `Provider` therefore has **three exporter workers contending for one
callback**.

**2.3 The callback runs under the provider's mutex.**
`CallbackAuthProvider::GetAuthorization` takes `m_mu` and holds it across the
user callback invocation. Combined with 2.2: a callback that takes 3 seconds
stalls the trace, metric *and* log pipelines for 3 seconds, not just the one
that happened to call it.

**2.4 The per-export timeout does not bound the callback.** In the
single-payload `HttpWireCodec::Send` path the callback runs before the deadline
clock starts (`AppendAuthHeader` precedes `fut.wait_for(deadline)`), so
`TimeoutOptions::per_export` — 10 s by default — caps the HTTP request and
nothing else. In the batched `SendMany` path the deadline point is taken first,
so callback time eats the request's budget instead of being added to it; either
way there is **no upper bound on how long your callback may run**. If it blocks
forever, the pipeline blocks forever, the span queue fills, and spans drop with
`DropReason::QueueFull`.

> **The rule that follows from all four:** the callback must do no network I/O
> and take no unbounded lock. It should be a cached read — a mutex acquisition
> and a string copy. Fetch tokens on a thread you own; the callback only hands
> out what that thread already fetched. Both recipes below are built this way.

### 2.5 The TTL is a memoisation of your callback, not a token lifetime

`cache_ttl` does one thing: `CallbackAuthProvider` returns its last value
without calling you again until the TTL elapses. It knows nothing about token
expiry. Two consequences:

- With the 60 s default, a token your refresher rotates at T is still served by
  microtel until T+60 s. That is safe only if your refresh overlaps the old
  token's validity — which the recipe in §4 does, by design.
- If you own a cache (you should), set `cache_ttl` to **zero** and let your
  cache be the only cache. Zero TTL invokes the callback on every batch; that
  costs one mutex acquisition and one string copy, and removes a whole class of
  "which cache is stale" reasoning. This path is covered by
  `CallbackAuthProviderTest.ZeroTtl_InvokesCallbackEveryTime`.

---

## 3. What happens when the callback fails

Both failure shapes cost **the one batch the callback was called for**, and
nothing else. This is `interfaces.md` §4.9's contract; it was not what the code
did until issues #250 and #251 were fixed.

| Your callback | What microtel does |
|---|---|
| Returns a value | `authorization: <value>` is appended to the batch's headers. |
| Returns `make_unexpected(Error{…})` | **The batch is dropped, not sent** — sending without auth is worse than not sending. `BuildHeaders` fails in the codec, nothing reaches the wire, and the exporter records one `non_retryable_failure` plus one `batches_failed`. Your `Error`'s kind survives and its message reaches `GetExporterHealth().last_error_message`, prefixed `authorization header unavailable:` (#250). |
| Throws | Caught at the provider boundary and converted to `Error::Kind::InternalFailure` carrying `what()`, then handled exactly as the row above. The rest of the drain still ships (#251). |

Write your callback to these three rules:

1. **Prefer returning `make_unexpected` to throwing.** Both cost one batch, but
   a returned error keeps your `Error::Kind` — a throw arrives at
   `GetExporterHealth()` as `InternalFailure` whatever went wrong. Wrap the
   body in `try` / `catch (const std::exception&)` and convert.
2. **Expect a dropped batch.** The spans in it are gone; there is no retry,
   because an unauthenticated retry is the same request. `batches_failed` plus
   `DropReason::NonRetryableFailure` move at the moment of failure and the
   message names auth, so the signal is no longer one hop removed in a
   receiver's 401 — but it is still worth logging the cause inside the
   callback, where you know which credential failed.
3. **Back off yourself.** An error is not cached
   (`CallbackAuthProviderTest.CallbackError_DoesNotCache_NextCallRetries`), so
   a down token endpoint means one callback invocation per batch with no
   damping. The recipes below keep the retry schedule in the refresher, where a
   sleep costs nothing, rather than in the callback, where it costs the
   pipeline.

---

## 4. Recipe 1 — OAuth2 client credentials

**Shape:** you own a token cache with its own refresh thread. The thread does
the HTTP POST to the token endpoint and refreshes at a fraction of the token's
lifetime, well before expiry. The callback reads the cached string under a
mutex. microtel's TTL is zero, so your cache is the only cache.

**What you must supply:** an HTTP client for the token endpoint. microtel's
transport is not reusable for this — it is an OTLP client bound to one
configured endpoint, not a general-purpose HTTP client, and it is not part of
the public API. Use whatever your application already has (libcurl, cpp-httplib,
your service mesh's local token agent). That client is **your** dependency, not
microtel's — see §5.4 for why that distinction is a project rule and not a
preference.

```cpp
// oauth2_token_cache.hpp
#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace myapp
{

/// @brief One fetched access token and the instant it stops being valid.
struct TokenGrant
{
    std::string access_token;
    std::chrono::steady_clock::duration expires_in{};
};

/// @brief Performs the client-credentials POST. Supplied by the application;
///        this is where your HTTP client lives.
using TokenFetcher = std::function<microtel::Expected<TokenGrant, microtel::Error>()>;

/// @brief Owns the current access token and refreshes it ahead of expiry.
///
/// The token endpoint is contacted only from this object's own thread.
/// `Authorization()` is a mutex acquisition and a string copy, so the microtel
/// exporter worker that calls it never waits on the network
/// (docs/auth-callback-recipes.md §2).
///
/// @threadsafety Thread-safe.
class Oauth2TokenCache
{
public:
    /// @param fetch performs one client-credentials grant request.
    /// @param refresh_at fraction of the token lifetime at which to refresh.
    ///        0.75 means "refresh once three-quarters of the lifetime has
    ///        elapsed", leaving a quarter of overlap for retries.
    explicit Oauth2TokenCache(TokenFetcher fetch,
                              double refresh_at = kDefaultRefreshFraction) noexcept;

    /// Requests stop and joins the refresh thread.
    ~Oauth2TokenCache() noexcept = default;

    Oauth2TokenCache(const Oauth2TokenCache&) = delete;
    Oauth2TokenCache& operator=(const Oauth2TokenCache&) = delete;
    Oauth2TokenCache(Oauth2TokenCache&&) = delete;
    Oauth2TokenCache& operator=(Oauth2TokenCache&&) = delete;

    /// @brief Fetch the first token synchronously, then start the refresh
    ///        thread. Call once, before `SdkBuilder::Build`, so the pipeline
    ///        does not start out unauthenticated (§4.2).
    ///
    /// @return the first fetch's failure, in which case no thread is started.
    [[nodiscard]] microtel::Expected<void, microtel::Error> Start();

    /// @brief Current `Authorization` header value, or the last failure.
    ///
    /// Never blocks on I/O. Safe to call from a microtel exporter worker.
    [[nodiscard]] microtel::Expected<std::string, microtel::Error> Authorization() const;

private:
    using Duration = std::chrono::steady_clock::duration;

    static constexpr double kDefaultRefreshFraction = 0.75;
    static constexpr std::chrono::seconds kRetryBackoff{5};
    static constexpr std::chrono::seconds kRefreshFloor{1};

    void RefreshLoop(std::stop_token stop, Duration first_wait);
    [[nodiscard]] bool SleepUntilStop(const std::stop_token& stop, Duration how_long);

    /// @return how long to wait before the next refresh, or `zero()` on failure.
    [[nodiscard]] Duration FetchAndStore();

    TokenFetcher m_fetch;
    double m_refresh_at;

    mutable std::mutex m_mu;
    std::string m_header;
    microtel::Error m_last_error{};
    bool m_have_token{false};

    std::mutex m_idle_mu;
    std::condition_variable_any m_idle_cv;

    // Declared last: the thread's entry point touches every member above, and
    // member initialisation is in declaration order.
    std::jthread m_thread;
};

}  // namespace myapp
```

```cpp
// oauth2_token_cache.cpp
#include "oauth2_token_cache.hpp"

#include <algorithm>
#include <exception>

namespace myapp
{

Oauth2TokenCache::Oauth2TokenCache(TokenFetcher fetch, double refresh_at) noexcept
    : m_fetch{std::move(fetch)}, m_refresh_at{refresh_at}
{
}

microtel::Expected<void, microtel::Error> Oauth2TokenCache::Start()
{
    const Duration first_wait = FetchAndStore();
    if (first_wait == Duration::zero())
    {
        const std::scoped_lock lock{m_mu};
        return microtel::make_unexpected(m_last_error);
    }
    m_thread = std::jthread{[this, first_wait](std::stop_token stop)
                            { RefreshLoop(std::move(stop), first_wait); }};
    return {};
}

microtel::Expected<std::string, microtel::Error> Oauth2TokenCache::Authorization() const
{
    const std::scoped_lock lock{m_mu};
    if (!m_have_token)
    {
        return microtel::make_unexpected(m_last_error);
    }
    return m_header;
}

Oauth2TokenCache::Duration Oauth2TokenCache::FetchAndStore()
{
    // The fetcher is application code and may throw; a throw that escaped here
    // would terminate the refresh thread and leave the token frozen.
    microtel::Expected<TokenGrant, microtel::Error> grant =
        microtel::make_unexpected(microtel::Error{.kind = microtel::Error::Kind::InternalFailure,
                                                  .message = "token fetcher did not run"});
    try
    {
        grant = m_fetch();
    }
    catch (const std::exception& e)
    {
        grant = microtel::make_unexpected(
            microtel::Error{.kind = microtel::Error::Kind::Network, .message = e.what()});
    }

    const std::scoped_lock lock{m_mu};
    if (!grant)
    {
        m_last_error = grant.error();
        // Deliberately keeps any previously fetched token: an expiring
        // credential still in its validity window beats no credential.
        return Duration::zero();
    }

    m_header = "Bearer " + grant->access_token;
    m_have_token = true;
    m_last_error = microtel::Error{};

    const auto wait =
        std::chrono::duration_cast<Duration>(grant->expires_in * m_refresh_at);
    return std::max(wait, Duration{kRefreshFloor});
}

/// @return false when stop was requested during the wait.
bool Oauth2TokenCache::SleepUntilStop(const std::stop_token& stop, Duration how_long)
{
    std::unique_lock lock{m_idle_mu};
    // The stop_token overload wakes immediately on the destructor's stop
    // request rather than waiting out a whole token lifetime.
    return !m_idle_cv.wait_for(lock, stop, how_long, [&stop] { return stop.stop_requested(); });
}

void Oauth2TokenCache::RefreshLoop(std::stop_token stop, Duration first_wait)
{
    Duration sleep_for = first_wait;
    while (SleepUntilStop(stop, sleep_for))
    {
        const Duration wait = FetchAndStore();
        sleep_for = (wait == Duration::zero()) ? Duration{kRetryBackoff} : wait;
    }
}

}  // namespace myapp
```

### 4.1 Wiring it to the builder

```cpp
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include "oauth2_token_cache.hpp"

#include <chrono>
#include <memory>

/// @param token_cache borrowed; must outlive the returned `Provider`.
microtel::Expected<std::shared_ptr<microtel::Provider>, microtel::ConfigError> BuildProvider(
    myapp::Oauth2TokenCache& token_cache)
{
    microtel::AuthCallback auth = [&token_cache]
    { return token_cache.Authorization(); };

    microtel::SdkBuilder builder;
    return builder.WithEndpoint("https://otlp.example.com:4318")
        .WithProtocol(microtel::Protocol::Http)
        .WithServiceName("checkout")
        // cache_ttl = 0: the callback is already a cached read, so microtel
        // should not stack a second cache on top of it (§2).
        .WithAuthProvider(std::move(auth), std::chrono::milliseconds{0})
        .Build();
}

// At startup, in this order:
auto token_cache = std::make_unique<myapp::Oauth2TokenCache>(MakeTokenFetcher());
if (const auto started = token_cache->Start(); !started)
{
    // Fail fast, or proceed and accept that early batches are unauthenticated.
    return HandleStartupFailure(started.error());
}
auto provider = BuildProvider(*token_cache);
```

The lambda captures a **borrowed** reference. The cache must outlive the
`Provider`: shut the provider down and release it first, then destroy the
cache. `CallbackAuthProvider` owns the `std::function`, not what it refers to,
and the exporter workers keep calling it until `Shutdown` returns.

### 4.2 Startup, and why `Start()` fetches synchronously

If the refresh thread's first fetch were allowed to race the first export, the
earliest batches would be built before any token existed. Per §3 they are
**dropped** — the callback has nothing to return, so nothing goes to the wire —
and you lose the first seconds of telemetry.
`Start()` does that first fetch on the caller thread at startup, where blocking
is free, and only then launches the refresher.

### 4.3 What this recipe does not handle

- **Token revocation.** The refresher is time-driven; a revoked-early token is
  served until its scheduled refresh. Detecting revocation means reacting to
  the receiver's 401, and microtel gives the callback no response feedback —
  it is invoked before the request, never after it.
- **Scope-per-signal.** One provider, one token, all three pipelines (§2.2).
  Separate credentials for traces and logs need separate `Provider`s, which is
  multi-profile — a v1.1 roadmap item, not a v1.0 capability.

---

## 5. Recipe 2 — AWS SigV4

### 5.1 Read this first: `AuthCallback` cannot express SigV4

SigV4 signs a *canonical request*: the HTTP method, the path, a specific set of
headers, and a SHA-256 hash of the request body. The resulting signature is
carried in `Authorization` alongside two companion headers — `x-amz-date` and
`x-amz-content-sha256` — that are part of the signed set and change on every
request.

`AuthCallback` provides none of that:

| SigV4 needs | `AuthCallback` gives |
|---|---|
| The request body, to hash it | Nothing — the callback takes no arguments and runs before the payload is attached. |
| To set `x-amz-date` per request | Nothing — it returns one string, which becomes `authorization`. The only other header surface is `WithHeaders`, which is **static** for the process lifetime. |
| To set `x-amz-content-sha256` per request | Same. |

A static `x-amz-date` via `WithHeaders` is not a workaround: AWS rejects a
signature whose date is more than 15 minutes old, so it would fail 15 minutes
after process start. There is no arrangement of the shipped surface that
produces a valid SigV4 request for a long-running process.

**So the roadmap's claim that "the shipped `WithAuthProvider` surface already
carries both [OAuth2 and SigV4]" holds for OAuth2 and not for SigV4.** That is
filed; see §7. What follows is (a) the signing primitives, which are correct and
useful in the place where a signature *can* be applied, and (b) the deployment
shape that actually works today.

### 5.2 The path that works: sign at a local sidecar

Point microtel at a loopback address and let a signing proxy add the signature,
where it can see the whole request:

```cpp
microtel::SdkBuilder builder;
auto provider = builder.WithEndpoint("http://127.0.0.1:4317")
                    .WithProtocol(microtel::Protocol::Grpc)
                    .WithServiceName("checkout")
                    .Build();
```

Two established sidecars do the signing:

- **The AWS Distro for OpenTelemetry Collector** with the `sigv4auth`
  extension on its OTLP exporter. It receives unsigned OTLP on loopback and
  forwards signed OTLP upstream.
- **`aws-sigv4-proxy`**, a general-purpose signing forward proxy, for the
  OTLP/HTTP path.

This is what ICP 0024 means by "adapter packages remain available". It costs a
process, and it adds nothing to microtel's link closure or yours. Keep the
loopback listener bound to `127.0.0.1` — the hop between you and the sidecar is
unauthenticated by construction.

### 5.3 The primitives, if you have somewhere to apply them

microtel already links OpenSSL (`find_package(OpenSSL REQUIRED)` in the root
`CMakeLists.txt`; `microtelConfig.cmake` re-runs `find_dependency(OpenSSL)` in
your project), so SigV4's HMAC-SHA256 chain needs **no new dependency** — only
`target_link_libraries(your_app PRIVATE OpenSSL::Crypto)` on your own link
line. microtel links `OpenSSL::SSL` privately for TLS and exports no crypto
helpers of its own; these are yours to own.

```cpp
// sigv4.cpp — HMAC-SHA256 chain and canonical-request hashing.
// Link: OpenSSL::Crypto. No new microtel dependency (CLAUDE.md rule 12).
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace myapp::sigv4
{

constexpr std::size_t kSha256Len = 32;

using Digest = std::array<unsigned char, kSha256Len>;

namespace
{

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string ToHex(std::span<const unsigned char> bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes)
    {
        out.push_back(kHexDigits[b >> 4U]);
        out.push_back(kHexDigits[b & 0x0FU]);
    }
    return out;
}

}  // namespace

/// @brief SHA-256 of @p data, lowercase hex. Used for the payload hash and
///        for the canonical request in the string-to-sign.
std::string Sha256Hex(std::string_view data)
{
    Digest digest{};
    unsigned int len = 0;
    ::EVP_Digest(data.data(), data.size(), digest.data(), &len, ::EVP_sha256(), nullptr);
    return ToHex(std::span{digest.data(), len});
}

/// @brief HMAC-SHA256(@p key, @p data).
Digest Hmac(std::span<const unsigned char> key, std::string_view data)
{
    Digest out{};
    unsigned int len = 0;
    ::HMAC(::EVP_sha256(),
           key.data(),
           static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()),
           data.size(),
           out.data(),
           &len);
    return out;
}

/// @brief The four-step SigV4 signing-key derivation.
///
/// @param secret AWS secret access key (without the `AWS4` prefix).
/// @param date   `YYYYMMDD`, UTC.
Digest SigningKey(std::string_view secret,
                  std::string_view date,
                  std::string_view region,
                  std::string_view service)
{
    const std::string seed = "AWS4" + std::string{secret};
    const auto* const seed_bytes = reinterpret_cast<const unsigned char*>(seed.data());

    const Digest k_date = Hmac(std::span{seed_bytes, seed.size()}, date);
    const Digest k_region = Hmac(k_date, region);
    const Digest k_service = Hmac(k_region, service);
    return Hmac(k_service, "aws4_request");
}

/// @brief The `Authorization` header value for a signed request.
///
/// @param canonical_request the canonical request per the SigV4 specification:
///        method, canonical URI, canonical query string, canonical headers,
///        signed-header list, and payload hash, newline-separated.
/// @param amz_date `YYYYMMDDTHHMMSSZ`, UTC, matching the `x-amz-date` header
///        that must accompany this value on the same request.
/// @param signed_headers `;`-joined, lowercase, sorted — identical to the list
///        inside @p canonical_request.
std::string AuthorizationHeader(std::string_view access_key_id,
                                std::string_view secret,
                                std::string_view region,
                                std::string_view service,
                                std::string_view amz_date,
                                std::string_view canonical_request,
                                std::string_view signed_headers)
{
    const std::string_view date = amz_date.substr(0, 8);  // YYYYMMDD
    const std::string scope =
        std::string{date} + "/" + std::string{region} + "/" + std::string{service} +
        "/aws4_request";

    const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string{amz_date} + "\n" +
                                       scope + "\n" + Sha256Hex(canonical_request);

    const Digest key = SigningKey(secret, date, region, service);
    const Digest signature = Hmac(key, string_to_sign);

    return "AWS4-HMAC-SHA256 Credential=" + std::string{access_key_id} + "/" + scope +
           ", SignedHeaders=" + std::string{signed_headers} +
           ", Signature=" + ToHex(signature);
}

}  // namespace myapp::sigv4
```

`AuthorizationHeader` takes the canonical request as a parameter precisely
because the caller must build it from the *actual* request — which, inside an
`AuthCallback`, you do not have. That is the blocker of §5.1 restated as a
signature.

### 5.4 Delegating to an external signer

If you already link the AWS SDK for C++, `Aws::Client::AWSAuthV4Signer` does
all of the above and handles credential rotation, instance-profile lookup and
regional endpoints. Use it — in your own request path, or in a sidecar.

**It is your dependency, not microtel's.** CLAUDE.md rule 12 fixes microtel's
runtime dependency closure at nghttp2, OpenSSL, vendored upb, zlib and optional
spdlog, and adding to it requires an ICP. Rule 13 additionally forbids any
shipped microtel artifact from carrying `absl::`, `grpc::` or
`google::protobuf::` symbols — defined *or* undefined — which the AWS SDK's
transitive graph can supply. Linking it into your application is fine and
changes nothing about microtel's artifacts; asking microtel to link it is the
thing that is not on offer.

---

## 6. Testing your callback

The callback is ordinary application code and needs no microtel test harness:

- **The cache in isolation.** Drive `Oauth2TokenCache` with a fetcher that
  returns scripted grants and failures. Assert `Authorization()` never blocks,
  that a failed refresh keeps the previous token, and that `Start()` surfaces
  the first failure without launching a thread.
- **The provider wrapper.** `CallbackAuthProvider`'s TTL, error and
  no-cache-on-error behaviour is already covered in
  [`tests/unit/common/auth/auth_providers_test.cpp`](../tests/unit/common/auth/auth_providers_test.cpp)
  against `FakeSteadyClock` — read it as the specification of what the TTL
  does, and do not re-test it.
- **End to end.** `tests/conformance/http/auth_test.cpp` and
  `tests/conformance/grpc/auth_test.cpp` exercise `WithAuthProvider` against a
  real receiver, including the wrong-token rejection path and the
  `non_retryable_failure` counter it increments. They are the template for a
  conformance test against your own backend.

---

## 7. Known gaps

Filed against the auth surface while writing this document. Each is a real
divergence, not a doc nit:

- **Issue #250 — callback error sent the batch unauthenticated.** *Fixed.* The
  codec's `BuildHeaders` now fails as a unit, so a batch whose `authorization`
  header cannot be built is dropped rather than shipped bare. See §3.
- **Issue #251 — a throwing callback was not caught at the provider boundary.**
  *Fixed.* `CallbackAuthProvider` converts any throw to
  `Error::Kind::InternalFailure`, so it costs the one batch instead of the
  whole drain. See §3.
- **Issue #252 — the auth surface's own docs are stale.** Still open. §4.9's
  "in practice only one caller" predates the metric and log pipelines, and the
  roadmap's claim that `WithAuthProvider` already carries SigV4 is not
  supportable. See §2.2 and §5.1.

This document describes behaviour as built.
