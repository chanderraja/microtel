# `resource_detectors`

Fills the `Resource` from the running process and the host it runs on, then
shows the precedence rules deciding who wins a contested key.

The example registers both built-in detectors, emits one span, and prints the
values the detectors should have found, so you can compare the console with
the span in Grafana line by line.

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

Neither detector is registered automatically. A `Resource` gets exactly the
detectors you ask for:

```cpp
microtel::SdkBuilder{}
    .WithServiceName("microtel-resource-detectors-example")
    .WithResourceDetector(microtel::MakeProcessDetector())
    .WithResourceDetector(microtel::MakeHostDetector())
    .Build();
```

Registration order matters: a later detector overrides an earlier one on the
same key. These two share no keys, so either order works here.

Both factories take an optional `root` prefix, e.g.
`MakeProcessDetector("/fixtures/proc-a")`, and resolve every path against it.
It exists so tests can point at a fixture tree. `getpid(2)` and
`gethostname(2)` ignore it, so it isn't a chroot.

## What lands on the span

Open <http://localhost:3000>, paste the printed trace ID into Explore → Tempo,
and open the span. Grafana shows resource attributes in their own section of
the span detail panel, below the span's own attributes. This run produced:

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

Check `process.pid` first. It's the number the console printed, and a match
tells you you're looking at this run's span and not an earlier one.

Resource attributes are queryable, which is most of the reason to populate
them:

```
{ resource.host.name = "fedora" }
{ resource.process.executable.name = "microtel_example_resource_detectors" }
```

Everything in the table is best-effort except `process.pid` and `host.name`.
If a restricted or namespaced `/proc` hides the `exe` link, the executable
attributes are left out. A container with no machine-id has no `host.id`,
which the OTel conventions mark optional and which is often absent. `Detect`
fails only when `/proc/self/cmdline` can't be opened at all, or when
`gethostname(2)` itself fails.

A detector that fails is logged at `Warn` and skipped, and `Build()` succeeds
without its attributes. That's the lenient default. Setting
`sdk.resource_detectors_strict` in `microtel.toml`, or
`MICROTEL_RESOURCE_DETECTORS_STRICT=1` in the environment, makes the same
failure fail `Build()` instead.

## Precedence: detector → env → user

`Build()` merges four layers key by key, each overriding the one before it
(`microtel-spec.md` §12.7, [`docs/configuration.md`](../../docs/configuration.md)):

1. Built-in defaults: the `unknown_service` placeholder, used only when nothing
   configured a service name.
2. Detectors, in registration order.
3. Environment: `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`.
4. File and code: `[resource]`, `WithResource`, `WithServiceName`.

A detector can therefore replace the `unknown_service` placeholder, since a
default isn't a configured value, but it can never override something an
operator configured. Later wins, and the operator comes after the machine.

### Seeing layer 3 beat layer 2

`host.name` comes from layer 2, via `gethostname(2)`. Set it in layer 3 and
run again:

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

The console still prints `fedora`, because that line is the program calling
`gethostname(2)` itself. The span shows what actually happened:

```
host.name   = resource-demo-override      <- the environment won
host.id     = fc78d6a3cab4484aaedc3a3ced36720f
process.pid = 1658574
```

Only the contested key changed. `host.id` came from the same detector and
nothing above it set that key, so it survives. Merging works per key, and a
detector's other attributes stay put when one of its keys loses.

`OTEL_RESOURCE_ATTRIBUTES` is a comma-separated `k=v` list, so
`host.name=a,deployment.environment=prod` sets both. To see layer 4 beat
layer 3 as well, add `.WithResource({{.key = "host.name", .value =
std::string{"from-code"}}})` to the builder and run with the variable still
set. The code wins.

## What to notice in the code

The example prints `getpid()` and `gethostname()` itself. Application code has
no reason to, since the detectors do it. It's only here so you can compare the
console with the span without having to trust either one alone.

Nothing in the program reads the merged `Resource`. There's no public accessor
for it, but `Build()` logs it once at `Info` (spec §12.7), so the merged
result shows up on stderr, or in your `LogSink` if you installed one:

```
resolved resource (profile "default", 3 attributes): deployment.environment="prod", host.name="node-7", service.name="checkout"
```

Keys are sorted, values of secret-looking keys print as `<redacted>`, and a
long Resource is cut off with `...and N more`. The example still emits a span,
so you can also check that the backend received the same attributes.

`OTEL_RESOURCE_ATTRIBUTES` is read here only to echo it. microtel reads the
variable itself inside `Build()`. The `std::getenv` call in `main` just labels
the run for the reader and feeds nothing into the SDK.
