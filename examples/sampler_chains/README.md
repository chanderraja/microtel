# `sampler_chains`

Composing head samplers into a rule chain, and watching the chain decide.

The example builds a **first-match** chain, emits a labelled batch of spans
across the three cases it distinguishes, and prints how many of each survived.
Only sampled spans are exported, so those counts are exactly what arrives in
Tempo — which is what makes the demonstration checkable rather than asserted.
A second, shorter phase repeats the same two rules under
`ChainMode::AllMustAgree`.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_sampler_chains

./build/examples/microtel_example_sampler_chains
```

```
endpoint: http://localhost:4317
service.name: microtel-sampler-chains-example

first-match chain  [tenant=premium -> AlwaysOn | kind=Server -> ratio 0.5 | default -> AlwaysOff]
  chain.premium.checkout  tenant=premium kind=Client  ->  sampled 12/12
      trace_id: b93008bbf184de01a731156b527b1774
      trace_id: 371712430f5acaf1cca00ff8ddc2ea3d
  chain.server.request  tenant=free kind=Server  ->  sampled 8/12
      trace_id: 4c0588a2288910d833858ee442ad03fd
      trace_id: 27b641d4a23a24887b4a6602604a8ccf
  chain.background.sweep  tenant=free kind=Internal  ->  sampled 0/12
  ForceFlush: Completed  batches_sent=1 batches_failed=0 queue_depth=0  Shutdown: Completed

all-must-agree chain  [tenant=premium AND kind=Server]
  agree.premium.server  tenant=premium kind=Server  ->  sampled 1/1
      trace_id: 1d43f63959f936ad7b21d7f0492e8a44
  agree.premium.internal  tenant=premium kind=Internal  ->  sampled 0/1
  agree.free.server  tenant=free kind=Server  ->  sampled 0/1
  ForceFlush: Completed  batches_sent=1 batches_failed=0 queue_depth=0  Shutdown: Completed
```

The `8/12` moves between runs and the other five numbers do not. That is the
whole point of the middle rule: it delegates to a ratio sampler, which decides
deterministically from the trace ID, so a fresh set of trace IDs gives a fresh
count near half.

## The chain

```cpp
microtel::MakeChainSampler(
    microtel::ChainMode::FirstMatch,
    microtel::MakeAttributeRuleSampler("tenant",
                                       std::string{"premium"},
                                       microtel::MakeAlwaysOnSampler(),
                                       microtel::MakeAlwaysOffSampler()),
    microtel::MakeSpanKindRuleSampler(microtel::SpanKind::Server,
                                      microtel::MakeTraceIdRatioSampler(0.5),
                                      microtel::MakeAlwaysOffSampler()),
    microtel::MakeAlwaysOffSampler());
```

Read top to bottom: **premium tenants always**, **server spans half the
time**, **everything else never**.

Its `Description()` — composed once, at construction — spells the same thing
out:

```
ChainSampler{FirstMatch, [AttributeRuleSampler{key=tenant, match=AlwaysOnSampler, else=AlwaysOffSampler}, SpanKindRuleSampler{kind=Server, match=TraceIdRatioSampler{0.500}, else=AlwaysOffSampler}, AlwaysOffSampler]}
```

Two things to read carefully in that string:

- **`TraceIdRatioSampler{0.500}`** is the resolved ratio, printed to three
  decimals. `MakeTraceIdRatioSampler` clamps its argument to `[0, 1]` and
  normalises NaN to `0.0`, so this is where you find out what the sampler
  actually got — `Provider::SetSamplerRatio` later retunes it and the
  description is regenerated (ICP 0026).
- **Every `else=` arm is inert in `FirstMatch` mode.** A rule whose predicate
  misses is skipped *whole*; the chain, not the rule, is the no-match path. The
  arms still have to be supplied, because each rule combinator is a complete
  sampler in its own right and works standalone — `multi_profile/` uses one
  that way. Passing `MakeAlwaysOffSampler()` is the convention.

`Description()` is declared on the internal `ISampler` interface and reached
through `SamplerHandle::Get()`, so the example does not print it: the public
handle is deliberately opaque. It is quoted here because it is the one place
the whole composed policy appears as a single readable line.

## What each rule decided

| Case | tenant | kind | Rule that decided | Sampled |
|---|---|---|---|---|
| `chain.premium.checkout` | `premium` | `Client` | 1 — attribute rule matched → `AlwaysOn` | 12 / 12 |
| `chain.server.request` | `free` | `Server` | 1 missed, skipped; 2 matched → ratio 0.5 | ~6 / 12 |
| `chain.background.sweep` | `free` | `Internal` | 1 and 2 missed; 3 is the default → `AlwaysOff` | 0 / 12 |

The premium case is `Client`-kind deliberately. Rule 2 would not have matched
it at all, so the only rule that could have sampled it is rule 1 — the table
row is a claim the run can actually support.

## What Grafana shows

Only the sampled spans were exported, so the backend is the second, independent
witness to the counts above. In **Explore → Tempo**, one TraceQL query per case:

```
{ resource.service.name = "microtel-sampler-chains-example" }   # 21 traces
{ name = "chain.premium.checkout" }                             # 12
{ name = "chain.server.request" }                               #  8, matching the console
{ name = "chain.background.sweep" }                             #  0 — no results
{ name = "agree.premium.server" }                               #  1
{ name = "agree.premium.internal" }                             #  0 — no results
{ name = "agree.free.server" }                                  #  0 — no results
```

An empty result for the three dropped cases is the interesting one: nothing was
queued, nothing was encoded, nothing was sent. An unsampled span is not a span
that got filtered downstream — `StartSpan` handed back the process-wide no-op
singleton, and `SetAttribute` and `End` on it did nothing.

The sampled spans carry `tenant` as a span attribute, so the split is visible
inside one query too:

```
{ resource.service.name = "microtel-sampler-chains-example" && span.tenant = "premium" }
```

## All-must-agree

```cpp
microtel::MakeChainSampler(
    microtel::ChainMode::AllMustAgree,
    microtel::MakeAttributeRuleSampler("tenant", std::string{"premium"},
                                       microtel::MakeAlwaysOnSampler(),
                                       microtel::MakeAlwaysOffSampler()),
    microtel::MakeSpanKindRuleSampler(microtel::SpanKind::Server,
                                      microtel::MakeAlwaysOnSampler(),
                                      microtel::MakeAlwaysOffSampler()));
```

```
ChainSampler{AllMustAgree, [AttributeRuleSampler{key=tenant, match=AlwaysOnSampler, else=AlwaysOffSampler}, SpanKindRuleSampler{kind=Server, match=AlwaysOnSampler, else=AlwaysOffSampler}]}
```

Same two rules, opposite treatment of a miss. Here every child is *asked for a
decision*, so a rule whose predicate misses answers through its `else` arm —
and one `AlwaysOff` ends the walk with `Drop`. `premium` alone is not enough;
`Server` alone is not enough; only `premium` **and** `Server` survives, which
is what the three one-span cases show.

When every child agrees, the **last** child's result is the one returned, so
its additional attributes and trace state survive. Put the child whose
contribution you want last.

## Two providers for two chains

The example builds two `Provider`s, one per chain, because a provider's sampler
is fixed for its life — `SetSamplerRatio` retunes a ratio in place but never
swaps one sampler for another. They differ only in `WithProfileName`, which has
to be unique among live providers; see [`../multi_profile/`](../multi_profile/)
for what else that name buys.

## What to notice in the code

- **The tenant is an *initial* attribute**, passed in `StartSpanOptions`, not
  set with `SetAttribute` afterwards. `ShouldSample` runs inside `StartSpan`
  and sees only what the options carry; an attribute set after the call cannot
  influence a decision that has already been made.
- **`std::string{"premium"}`, not `"premium"`.** `AttributeValue` is a
  `std::variant` whose first alternative is `bool`, and a `const char*` binds to
  it. The rule would compare against `true`, match nothing, and say nothing
  about it.
- **There is no sample-on-duration rule, by design.** `ShouldSample` runs at
  span start, before the span has a duration. Selecting spans by how long they
  took is tail sampling and belongs in the collector — see
  [`docs/icps/0024-v1.1-rescope.md`](../../docs/icps/0024-v1.1-rescope.md).
- **Matching is exact.** `MakeSpanNameRuleSampler` compares whole names, and
  `MakeAttributeRuleSampler` compares whole `AttributeValue`s, so the same key
  holding a different alternative is a miss. There is no prefix, glob, or regex
  form in v1.1.
