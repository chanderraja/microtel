# `sampler_chains`

Composes head samplers into a rule chain and shows what the chain decides.

The example builds a first-match chain, emits a labelled batch of spans for
each of the three cases it distinguishes, and prints how many of each were
sampled. Only sampled spans are exported, so those counts are exactly what
arrives in Tempo, and you can check them there. A second, shorter phase runs
the same two rules under `ChainMode::AllMustAgree`.

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

Only the sampled spans were exported. In Grafana:
  { resource.service.name = "microtel-sampler-chains-example" }
```

The `8/12` changes from run to run and the other five counts don't. That's the
middle rule at work: it delegates to a ratio sampler, which decides
deterministically from the trace ID, so each run's fresh trace IDs give a fresh
count somewhere near half.

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

Reading top to bottom: premium tenants are always sampled, server spans half
the time, and everything else never.

Its `Description()`, composed once at construction, spells out the same
policy:

```
ChainSampler{FirstMatch, [AttributeRuleSampler{key=tenant, match=AlwaysOnSampler, else=AlwaysOffSampler}, SpanKindRuleSampler{kind=Server, match=TraceIdRatioSampler{0.500}, else=AlwaysOffSampler}, AlwaysOffSampler]}
```

Two parts of that string deserve a closer look.

`TraceIdRatioSampler{0.500}` is the resolved ratio, printed to three decimals.
`MakeTraceIdRatioSampler` clamps its argument to `[0, 1]` and normalises NaN to
`0.0`, so this is where you find out what the sampler actually received. If
`Provider::SetSamplerRatio` retunes it later, the description is regenerated
(ICP 0026).

Every `else=` arm is inert in `FirstMatch` mode. A rule whose predicate misses
is skipped entirely, and the chain itself handles the no-match case by moving
on. You still have to supply the arms, because each rule combinator is a
complete sampler that also works standalone (`multi_profile/` uses one that
way). By convention, pass `MakeAlwaysOffSampler()`.

`Description()` is declared on the internal `ISampler` interface and reached
through `SamplerHandle::Get()`. The public handle is deliberately opaque, so
the example doesn't print it. It's quoted here because it's the one place the
whole composed policy appears as a single readable line.

## What each rule decided

| Case | tenant | kind | Rule that decided | Sampled |
|---|---|---|---|---|
| `chain.premium.checkout` | `premium` | `Client` | 1 — attribute rule matched → `AlwaysOn` | 12 / 12 |
| `chain.server.request` | `free` | `Server` | 1 missed, skipped; 2 matched → ratio 0.5 | ~6 / 12 |
| `chain.background.sweep` | `free` | `Internal` | 1 and 2 missed; 3 is the default → `AlwaysOff` | 0 / 12 |

The premium case uses `Client` kind on purpose. Rule 2 wouldn't match it, so
rule 1 is the only rule that could have sampled it, and the first row of the
table is something the run can actually prove.

## What Grafana shows

Only sampled spans were exported, so the backend gives you a second,
independent check on the counts above. In Explore → Tempo, run one TraceQL
query per case:

```
{ resource.service.name = "microtel-sampler-chains-example" }   # 21 traces
{ name = "chain.premium.checkout" }                             # 12
{ name = "chain.server.request" }                               #  8, matching the console
{ name = "chain.background.sweep" }                             #  0 — no results
{ name = "agree.premium.server" }                               #  1
{ name = "agree.premium.internal" }                             #  0 — no results
{ name = "agree.free.server" }                                  #  0 — no results
```

The empty results for the three dropped cases are the interesting ones.
Nothing was queued, encoded or sent. An unsampled span never existed as far as
the pipeline is concerned: `StartSpan` returned the process-wide no-op
singleton, and `SetAttribute` and `End` on it did nothing.

The sampled spans carry `tenant` as a span attribute, so you can also see the
split within one query:

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

These are the same two rules, but a miss is handled the other way round. Every
child is asked for a decision, so a rule whose predicate misses answers
through its `else` arm, and a single `AlwaysOff` ends the walk with `Drop`.
Neither `premium` nor `Server` is enough on its own. Only a span that is both
`premium` and `Server` is sampled, which is what the three one-span cases show.

When every child agrees, the result from the last child is the one returned,
so its extra attributes and trace state are what survive. Put the child whose
contribution you want to keep last.

## Two providers for two chains

The example builds two `Provider`s, one per chain, because a provider's sampler
is fixed for its lifetime. `SetSamplerRatio` retunes a ratio in place but
never swaps one sampler for another. The two providers differ only in
`WithProfileName`, which has to be unique among live providers; see
[`../multi_profile/`](../multi_profile/) for what else that name gets you.

## What to notice in the code

The tenant is an initial attribute, passed in `StartSpanOptions` and not set
with `SetAttribute` afterwards. `ShouldSample` runs inside `StartSpan` and
sees only what the options carry, so an attribute set after the call can't
affect a decision that's already been made.

The rule compares against `std::string{"premium"}` rather than a bare
`"premium"`. `AttributeValue` is a `std::variant` whose first alternative is
`bool`. A C++20 standard library converts a string literal to the
`std::string` alternative, but libraries that predate the P0608 fix to
`variant`'s converting constructor bind a `const char*` to `bool`. The rule
would then compare against `true`, match nothing, and give no sign of it.

There's no sample-on-duration rule, by design. `ShouldSample` runs when the
span starts, before it has a duration. Choosing spans by how long they took is
tail sampling, which belongs in the collector; see
[`docs/icps/0024-v1.1-rescope.md`](../../docs/icps/0024-v1.1-rescope.md).

Matching is exact. `MakeSpanNameRuleSampler` compares whole names, and
`MakeAttributeRuleSampler` compares whole `AttributeValue`s, so the same key
holding a different alternative counts as a miss. v1.1 has no prefix, glob or
regex form.
