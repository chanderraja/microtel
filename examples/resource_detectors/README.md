# `resource_detectors`

Filling the `Resource` from the running process and the host it runs on, and
watching the precedence rules decide who wins a contested key.

The example registers both built-in detectors, emits one span, and prints the
values the detectors are expected to have found — so the console and the span
in Grafana can be compared line for line.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_resource_detectors

./build/examples/microtel_example_resource_detectors
```

```
endpoint: http://localhost:4317
service.name: microtel-resource-detectors-example
OTEL_RESOURCE_ATTRIBUTES: <unset>

what the detectors should report:
  process.pid: 1658567
  host.name:   fedora

trace_id: fae82fbdcc1a50cff0462a0309b85bca
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed
```

## Registering them

Neither detector is registered automatically — a `Resource` gets exactly the
detectors you ask for:

```cpp
microtel::SdkBuilder{}
    .WithServiceName("microtel-resource-detectors-example")
    .WithResourceDetector(microtel::MakeProcessDetector())
    .WithResourceDetector(microtel::MakeHostDetector())
    .Build();
```

Registration order is significant: a later detector overrides an earlier one on
the same key. These two share no keys, so the order is free here.

Both take an optional `root` prefix — `MakeProcessDetector("/fixtures/proc-a")`
— which every path is resolved against. It is for tests pointing at a fixture
tree; `getpid(2)` and `gethostname(2)` are unaffected by it, so it is not a
chroot.

## What lands on the span

Open <http://localhost:3000>, paste the printed trace ID into **Explore →
Tempo**, and open the span. Grafana shows resource attributes in their own
section of the span detail panel, below the span's own attributes. This run
produced:

| Attribute | Value | Detector | Source |
|---|---|---|---|
| `process.pid` | `1658567` | process | `getpid(2)` |
| `process.executable.path` | `/home/…/build/examples/microtel_example_resource_detectors` | process | `readlink(2)` on `/proc/self/exe` |
| `process.executable.name` | `microtel_example_resource_detectors` | process | file name of that path |
| `process.command` | `./build/examples/microtel_example_resource_detectors` | process | `argv[0]` from `/proc/self/cmdline` |
| `process.command_args` | `["./build/examples/microtel_example_resource_detectors"]` | process | the whole `argv`, as a string array |
| `host.name` | `fedora` | host | `gethostname(2)` |
| `host.id` | `fc78d6a3cab4484aaedc3a3ced36720f` | host | `/etc/machine-id`, else `/var/lib/dbus/machine-id` |
| `service.name` | `microtel-resource-detectors-example` | — | `WithServiceName` |
| `service.version` | `1.0.0` | — | `WithServiceVersion` |

`process.pid` is the one to check first: it is the number the console printed,
and matching it is how you know you are looking at this run's span rather than
a previous one's.

Resource attributes are queryable, which is most of why they are worth
populating:

```
{ resource.host.name = "fedora" }
{ resource.process.executable.name = "microtel_example_resource_detectors" }
```

Everything in the table is **best-effort except `process.pid` and
`host.name`**. A restricted or namespaced `/proc` that hides the `exe` link
omits the executable attributes; a container with no machine-id omits
`host.id`, which is optional in the OTel conventions and routinely absent.
`Detect` fails only when `/proc/self/cmdline` cannot be opened at all, or when
`gethostname(2)` itself fails.

A detector that fails is logged at `Warn` and skipped, and `Build()` succeeds
without its contribution. That is the lenient default; setting
`sdk.resource_detectors_strict` in `microtel.toml`, or
`MICROTEL_RESOURCE_DETECTORS_STRICT=1` in the environment, makes the same
failure fail `Build()` instead.

## Precedence: detector → env → user

`Build()` merges four layers, key by key, each overriding the one before it
(`microtel-spec.md` §12.7, [`docs/configuration.md`](../../docs/configuration.md)):

1. **Built-in defaults** — the `unknown_service` placeholder, and only when
   nothing configured a service name.
2. **Detectors**, in registration order.
3. **Environment** — `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`.
4. **File and code** — `[resource]`, `WithResource`, `WithServiceName`.

So a detector can replace the `unknown_service` placeholder — a default is not
a configured value — but it can never override something an operator
configured. Later wins, and the operator is later than the machine.

### Seeing layer 3 beat layer 2

`host.name` is layer 2's, from `gethostname(2)`. Claim it for layer 3 and run
again:

```bash
OTEL_RESOURCE_ATTRIBUTES=host.name=resource-demo-override \
    ./build/examples/microtel_example_resource_detectors
```

```
OTEL_RESOURCE_ATTRIBUTES: host.name=resource-demo-override
...
what the detectors should report:
  process.pid: 1658574
  host.name:   fedora
trace_id: 30c24ef5422fdd7a9d8de1f94ed574cc
```

The console still prints `fedora`, because that line is this program reading
`gethostname(2)` itself. The span tells the real story:

```
host.name   = resource-demo-override      <- the environment won
host.id     = fc78d6a3cab4484aaedc3a3ced36720f
process.pid = 1658574
```

Only the contested key moved. `host.id` came from the same detector and was
never claimed by anything above it, so it survives untouched — merging is per
key, not per layer, and a detector is not discarded wholesale because one of
its keys lost.

`OTEL_RESOURCE_ATTRIBUTES` is a comma-separated `k=v` list, so
`host.name=a,deployment.environment=prod` sets both. To watch layer 4 beat
layer 3 as well, add `.WithResource({{.key = "host.name", .value =
std::string{"from-code"}}})` to the builder and run with the variable still
set: code wins.

## What to notice in the code

- **The example prints `getpid()` and `gethostname()` itself.** Application
  code has no reason to — the detectors do it. It is here purely so the console
  and the span can be compared without trusting either alone.
- **Nothing in the program reads the merged `Resource`.** There is no public
  accessor for it, and microtel does not log it at init today (spec §12.7 says
  it should — [#284](https://github.com/chanderraja/microtel/issues/284)), so
  the backend is currently the only place the merged result can be observed.
  That is exactly why this example emits a span rather than printing a table.
- **`OTEL_RESOURCE_ATTRIBUTES` is read here only to echo it.** microtel reads
  the variable itself inside `Build()`; the `std::getenv` call in `main` labels
  the run for the reader and feeds nothing into the SDK.
