# `leaf_mqtt`

The [`leaf/`](../leaf/) example again, over the transport IoT devices most
often have: MQTT. **Experimental in v1.2**, like the leaf and the concentrator
themselves.

`mqtt_leaf` plays the device. It's a C program that builds spans with the
microtel leaf library (`microtel/leaf.h`), encodes each batch as one OTLP
payload, and publishes it to `microtel/<device-id>/traces` with
[coreMQTT](https://github.com/FreeRTOS/coreMQTT), the MQTT 3.1.1 client
FreeRTOS firmware uses. coreMQTT does no I/O either; it calls a
`TransportInterface_t` the application supplies. Here that's a few dozen
lines of POSIX TCP, standing in for the device's network stack.

`mqtt_concentrator` plays the gateway. It's an ordinary microtel `Provider`
with its leaf receiver turned on, plus a
[libmosquitto](https://mosquitto.org/api/) client subscribed to
`microtel/+/traces`. For each message it takes the leaf id from the topic and
hands the payload to `LeafReceiver::Ingest`. From there the spans take the
Provider's normal path to the collector, with every leaf's spans sharing
export requests.

Between them sits a Mosquitto broker, started as an opt-in overlay next to the
shared stack. Nothing microtel-shaped changes from the UDP example: the
payload bytes are the same, and so is the `Ingest` call. Only the program
around them does.

## Why MQTT, and where the leaf id comes from

The concentrator has to name each leaf, and the name has to come from the
transport, not from the payload. A payload can claim to be anything; the
design (§4.4 of
[`docs/leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md))
has the transport id win over whatever the payload says, and exports it as
`device.id`.

Over UDP the transport id is the sender's `address:port`, which is stable only
on a network you control. MQTT has a better one: the topic. Each device
publishes to its own topic, `microtel/<device-id>/traces`, and the
concentrator's single `+` wildcard subscription receives all of them. The
topic level that `+` matched is the leaf id. Leaves can come and go, change
address or sit behind NAT, and the concentrator never needs a list of them.

The topic is only as trustworthy as the broker makes it. This demo's broker
is anonymous, so any client could publish under any device's name. A real
broker authenticates each device and restricts it to its own topic; see
[Real deployments](#real-deployments).

The leaf uses its device id as its MQTT client id as well. That's what lets a
broker ACL tie the topic to the connection.

## Run it

You'll need the leaf and the concentrator compiled in (both are off by
default) and libmosquitto's development files:

```bash
sudo dnf install mosquitto-devel      # Fedora
sudo apt install libmosquitto-dev     # Debian, Ubuntu
```

coreMQTT needs nothing installed. CMake fetches it, pinned to `v2.3.1`, when
it configures this example, and compiles it into the leaf binary only.

```bash
examples/stack/up.sh                  # collector, Tempo, Grafana
examples/leaf_mqtt/up-mqtt.sh         # Mosquitto on 127.0.0.1:1883

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON \
      -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_WITH_CONCENTRATOR=ON
cmake --build build --target microtel_example_leaf_mqtt_concentrator \
                    --target microtel_example_leaf_mqtt_leaf
```

If CMake prints `examples/leaf_mqtt: skipped: libmosquitto not found`, the
development package isn't installed, or it's somewhere CMake doesn't look;
point `CMAKE_PREFIX_PATH` at its prefix.

Start the concentrator, then two leaves from a second terminal. The first
uses the defaults. The second is another device, in boot-relative time mode,
and stops after three payloads.

```bash
# terminal 1
./build/examples/microtel_example_leaf_mqtt_concentrator
```
```bash
# terminal 2
./build/examples/microtel_example_leaf_mqtt_leaf &
./build/examples/microtel_example_leaf_mqtt_leaf boot gh-south-02 1883 3
```

The arguments:

```
microtel_example_leaf_mqtt_concentrator [endpoint] [broker-port] [config]

  [endpoint]     OTLP/gRPC collector, default $OTEL_EXPORTER_OTLP_ENDPOINT,
                 else http://localhost:4317
  [broker-port]  MQTT broker on 127.0.0.1, default 1883
  [config]       TOML with the [concentrator] table, default microtel.toml here

microtel_example_leaf_mqtt_leaf [stamped|sync|boot] [device-id] [broker-port] [payloads]

  defaults: stamped gh-north-01 1883 5
  device-id: 1-64 characters, no '/', '+' or '#' (it becomes one topic level)
```

The concentrator waits up to a minute for the first message, and exits once
none has arrived for three seconds while it's connected, so the example can
show it flushing and shutting down. A real gateway would run until stopped.

### What they print

This is a real run against the stack with podman on Fedora 44.

```
$ ./build/examples/microtel_example_leaf_mqtt_leaf
leaf gh-north-01 -> mqtt://127.0.0.1:1883, time mode stamped, leaf library 1.1.1
connected as client id gh-north-01, MQTT 3.1.1, QoS 1
payload 1: 457 bytes, 2 spans -> microtel/gh-north-01/traces  PUBACK
payload 2: 457 bytes, 2 spans -> microtel/gh-north-01/traces  PUBACK
payload 3: 457 bytes, 2 spans -> microtel/gh-north-01/traces  PUBACK
payload 4: 457 bytes, 2 spans -> microtel/gh-north-01/traces  PUBACK
payload 5: 457 bytes, 2 spans -> microtel/gh-north-01/traces  PUBACK
```

```
$ ./build/examples/microtel_example_leaf_mqtt_leaf boot gh-south-02 1883 3
leaf gh-south-02 -> mqtt://127.0.0.1:1883, time mode boot, leaf library 1.1.1
connected as client id gh-south-02, MQTT 3.1.1, QoS 1
payload 1: 490 bytes, 2 spans -> microtel/gh-south-02/traces  PUBACK
payload 2: 490 bytes, 2 spans -> microtel/gh-south-02/traces  PUBACK
payload 3: 490 bytes, 2 spans -> microtel/gh-south-02/traces  PUBACK
```

```
$ ./build/examples/microtel_example_leaf_mqtt_concentrator
concentrator: mqtt://127.0.0.1:1883 microtel/+/traces -> http://localhost:4317 (OTLP/gRPC)
config: /path/to/microtel/examples/leaf_mqtt/microtel.toml
libmosquitto 2.1.0
connected to the broker, subscribing to microtel/+/traces
gh-north-01  457 bytes  Accepted  spans_accepted=2
gh-south-02  490 bytes  Accepted  spans_accepted=2
gh-south-02  490 bytes  Accepted  spans_accepted=2
gh-north-01  457 bytes  Accepted  spans_accepted=2
gh-north-01  457 bytes  Accepted  spans_accepted=2
gh-south-02  490 bytes  Accepted  spans_accepted=2
gh-north-01  457 bytes  Accepted  spans_accepted=2
gh-north-01  457 bytes  Accepted  spans_accepted=2
payloads_accepted=8 payloads_rejected=0 leaves_tracked=2 time_fallbacks=0
ForceFlush: Completed
batches_sent=2 batches_failed=0
Shutdown: Completed
```

Each line of the concentrator's output is one message, one `Ingest` call and
its `IngestResult`. The payloads are byte-for-byte the size the UDP example
sends, because they're the same payloads; MQTT adds its own few bytes of
header around them on the wire, and the concentrator never sees those.

`PUBACK` on the leaf's side means the broker has the message, not that the
concentrator does. That distinction matters when the concentrator isn't
subscribed at the moment; see [Troubleshooting](#troubleshooting).

(`libmosquitto 2.1.0` is what `mosquitto_lib_version` reported for Fedora
44's `mosquitto-devel-2.1.2`, so don't read the patch level from it.)

## What the collector receives

Look the traces up by what the concentrator put on them:

```bash
$ curl -s -G http://localhost:3200/api/search \
    --data-urlencode 'q={ resource.service.name =~ "greenhouse-.*" }' | jq -r \
    '.traces[] | "\(.traceID)  \(.rootServiceName)  \(.rootTraceName)"'
85ea8428d49307658f59f0baff288808  greenhouse-north  greenhouse.cycle
c340b4bfa132a4ce7623be5f42d3ae82  greenhouse-north  greenhouse.cycle
395810b79083b55fae2ad8ab2a58a8fa  greenhouse-sensor  greenhouse.cycle
c2d835428f96f34a1d49f9abdbd8d5b  greenhouse-north  greenhouse.cycle
6881568ef760d32a7efc82fe083f95ae  greenhouse-sensor  greenhouse.cycle
831c37a2a3d78fdb8364929cc130eee6  greenhouse-north  greenhouse.cycle
c51b44f3fd371c7c9678683185914c99  greenhouse-sensor  greenhouse.cycle
fa8ab204b974f7d4b46881657a208f2f  greenhouse-north  greenhouse.cycle
```

Eight traces, one per payload: five from `greenhouse-north` and three from
`greenhouse-sensor`. The two devices run the same firmware, which calls itself
`greenhouse-sensor`; `microtel.toml` renames the one whose id is
`gh-north-01`. The Resources show where every key came from:

```bash
$ curl -s http://localhost:3200/api/traces/85ea8428d49307658f59f0baff288808 | \
    jq -c '.batches[0].resource.attributes | map({(.key): (.value | to_entries[0].value)}) | add'
{"deployment.environment":"example","device.id":"gh-north-01","greenhouse.zone":"north","service.version":"0.3.1","service.name":"greenhouse-north"}

$ curl -s http://localhost:3200/api/traces/395810b79083b55fae2ad8ab2a58a8fa | \
    jq -c '.batches[0].resource.attributes | map({(.key): (.value | to_entries[0].value)}) | add'
{"deployment.environment":"example","device.id":"gh-south-02","service.version":"0.3.1","service.name":"greenhouse-sensor"}
```

| Key | From |
|---|---|
| `deployment.environment` | `[concentrator.leaf_defaults.resource]` in `microtel.toml`: a default for every leaf |
| `service.version` | the leaf's own Resource, set in firmware |
| `service.name`, `greenhouse.zone` | `[concentrator.leaves."gh-north-01"]`: this device's entry overrides the firmware's `service.name` |
| `device.id` | the topic level, which the payload cannot choose or override |

`gh-south-02` has no entry, so it keeps its firmware's name, and
`unknown_leaf = "accept"` is why its payloads got in at all.

In Grafana (<http://localhost:3000>) the traces show up on the "microtel —
recent traces" dashboard within about ten seconds. In Explore → Tempo,
`{ resource.device.id = "gh-south-02" }` finds one device's traces. Each has a
`greenhouse.cycle` root with a 40 ms `sensor.read` child.

The three time modes work exactly as in the UDP example; its
[README](../leaf/README.md#time) explains them. One thing is specific to a
broker: in `stamped` mode the concentrator dates a payload by when it
*arrives*, and a broker that holds messages for a while (a persistent
session, below) makes them arrive late. Use `boot` or `sync` if the broker
may queue.

## The leaf half

The span-building and encoding code is `udp_leaf.c`'s. What's new is below the
payload.

coreMQTT declares `struct NetworkContext` and leaves its contents to the
application. On a device it would hold an lwIP or FreeRTOS+TCP socket, and a
TLS session for port 8883. Here it holds a file descriptor:

```c
struct NetworkContext
{
    int fd;
};

static int32_t transport_recv(NetworkContext_t* ctx, void* buffer, size_t len);
static int32_t transport_send(NetworkContext_t* ctx, const void* buffer, size_t len);
```

coreMQTT's contract for these two is strict and easy to get wrong. Both return
the bytes moved, `0` for "nothing right now, call again", or a negative value
for a dead connection, and **`0` must never mean closed**. So the shim turns
`recv()`'s orderly-shutdown `0` into `-1`. The receive also must not block for
long: it polls for 10 ms and returns `0`, and coreMQTT calls it again. Sends
use `MSG_NOSIGNAL`, so writing to a broker that has gone away is an error
return instead of a `SIGPIPE`.

Everything coreMQTT keeps is caller-allocated, like the leaf's: a 512-byte
network buffer (outgoing payloads are sent straight from the caller's memory,
so this only holds headers and what the broker sends back) and one
`MQTTPubAckInfo_t` record for the publish awaiting its acknowledgement.

```c
MQTT_Init(&mqtt, &transport, mqtt_now_ms, on_mqtt_event, &buffer);
MQTT_InitStatefulQoS(&mqtt, outgoing, 1, NULL, 0);
MQTT_Connect(&mqtt, &connect, NULL, CONNACK_TIMEOUT_MS, &session_present);
...
microtel_leaf_encode(&g_leaf, payload, sizeof(payload), &written);
MQTT_Publish(&mqtt, &publish, packet_id);         /* QoS 1 */
while (g_acked_packet_id != packet_id)
    MQTT_ProcessLoop(&mqtt);                      /* until the PUBACK */
```

**QoS 1**, because a payload holds finished spans the leaf has already
released. Lose it and those traces are gone, so the leaf waits for the
broker's `PUBACK` before sending the next one. The cost is at-least-once
delivery: if a connection drops after the broker got a message but before
its `PUBACK` arrived, a device that resends it makes the concentrator ingest
the same spans twice. QoS 2 would prevent that, for two more round trips per
payload, which a battery-powered device rarely wants to spend on telemetry.
QoS 0 is reasonable for a device that would rather drop telemetry than wait
for it.

This leaf is deliberately simple about failure. A publish that fails, or
isn't acknowledged within five seconds, ends the program with exit code 3. A
real device would reconnect with a persistent session (`cleanSession =
false`) and resend its unacknowledged publish with `MQTT_PublishToResend`.

A payload here has no MTU to fit under, so the leaf encodes into a 4 KiB
buffer instead of the UDP example's 1400 bytes. The concentrator's
`max_payload_bytes` (64 KiB by default) is the real limit.

## The concentrator half

```cpp
mosquitto* client = mosquitto_new("microtel-leaf-concentrator", true, &bridge);
mosquitto_connect_callback_set(client, OnConnect);      // subscribes
mosquitto_disconnect_callback_set(client, OnDisconnect);
mosquitto_message_callback_set(client, OnMessage);      // ingests
mosquitto_reconnect_delay_set(client, 1, 30, true);
mosquitto_connect_async(client, "127.0.0.1", 1883, 60);
mosquitto_loop_start(client);                           // the network thread
```

(The file wraps the client in a `std::unique_ptr` and the library's
init/cleanup in a scoped object.)

`OnMessage` does what the UDP example's receive loop did:

```cpp
const std::string_view leaf_id = LeafIdFromTopic(message->topic);  // "gh-north-01"
const microtel::IngestResult result = receiver->Ingest(microtel::IngestRequest{
    .leaf_id = leaf_id,
    .payload = std::span<const std::byte>(payload, size),
    .received_at = received_at,
});
```

**Threads.** `mosquitto_loop_start` runs the client on its own network
thread, and every callback, `OnMessage` included, runs there. So `Ingest` is
called from libmosquitto's thread, not from `main`. That's fine: `Ingest` is
thread-safe, and it never exports on the caller's thread, only decodes,
checks and enqueues, so it doesn't hold the network thread up for long.
`main` only waits on a condition variable until the link goes quiet, and
reads the receiver's counters after `mosquitto_loop_stop` has joined the
network thread, when no `Ingest` can be in flight.

`received_at` is read at the top of `OnMessage`, before anything else
happens to the message. It's the `R` that `stamped` and `boot` mode date
payloads by.

**Broker disconnects.** The network thread reconnects on its own, after 1 s
and then doubling up to 30 s. The session is clean, so the broker forgets the
subscription whenever the connection drops, and `OnConnect`, which runs after
every successful connection, subscribes again each time. The concentrator can
also start before the broker: the first connection attempt fails, and the
thread keeps trying. Here the broker was restarted with
`podman restart microtel-mqtt-example_mosquitto_1` between two leaf runs:

```
connected to the broker, subscribing to microtel/+/traces
gh-east-03  457 bytes  Accepted  spans_accepted=2
lost the broker (The connection was lost), reconnecting
connected to the broker, subscribing to microtel/+/traces
gh-east-03  457 bytes  Accepted  spans_accepted=2
```

The idle exit only counts time spent connected, so a broker outage doesn't
end the concentrator early.

You can see a rejection by publishing something that isn't a payload, with
the client the broker image ships:

```bash
podman exec microtel-mqtt-example_mosquitto_1 \
    mosquitto_pub -q 1 -t microtel/bogus/traces -m 'not otlp'
```

```
bogus  8 bytes  Malformed  spans_accepted=0
```

## Real deployments

The overlay's broker is plaintext and anonymous, and publishes its port on
127.0.0.1 only; `mosquitto.conf` says so at the top. A broker that devices
reach over a real network needs three more things. None of them changes the
microtel side.

- **TLS on 8883.** In Mosquitto, a second listener:

  ```
  listener 8883
  cafile   /mosquitto/certs/ca.crt
  certfile /mosquitto/certs/broker.crt
  keyfile  /mosquitto/certs/broker.key
  # require_certificate true    # for per-device client certificates (mTLS)
  ```

  The concentrator adds `mosquitto_tls_set(client, "ca.crt", nullptr, ...)`
  before connecting, and connects to 8883. On the leaf, the transport shim is
  where TLS goes: its `NetworkContext` holds a TLS session (mbedTLS, wolfSSL,
  or the modem's own TLS sockets) and `transport_recv`/`transport_send` read
  and write through it. coreMQTT doesn't change. The [`tls/`](../tls/) example
  has a script that generates a CA and certificates.
- **Authentication.** `allow_anonymous false`, and a `password_file` or client
  certificates, one identity per device.
- **An ACL tying each device to its own topic**, which is what makes the topic
  a trustworthy leaf id:

  ```
  # acl_file: a client may publish only under its own client id
  pattern write microtel/%c/traces
  user microtel-leaf-concentrator
  topic read microtel/+/traces
  ```

  With `%u` instead of `%c` the topic is tied to the authenticated user name
  instead.

A gateway that must not lose payloads while it restarts would also use a
persistent session: a fixed client id, `clean_session = false` in
`mosquitto_new`, and QoS 1 on the subscription (which it already has). The
broker then queues messages for the gateway while it's away, and delivers them
when it reconnects. Pair that with `boot` or `sync` time mode on the leaves,
since queued payloads arrive late (see [What the collector
receives](#what-the-collector-receives)). For the queue to survive a broker
restart as well, turn `persistence` on in the broker.

## Troubleshooting

**The leaf prints `connect to broker: Connection refused`.** The broker isn't
running. Start it with `examples/leaf_mqtt/up-mqtt.sh`. The overlay publishes
only on 127.0.0.1, so a leaf on another machine can't reach it either.

**The leaf gets its `PUBACK`s, but the concentrator prints nothing for them.**
The concentrator wasn't subscribed when they were published, and with a clean
session the broker drops a message nobody is subscribed to. Start the
concentrator first and wait for `subscribing to microtel/+/traces`. The same
happens just after a broker restart, until the concentrator's backoff brings
it back; that's what a persistent session is for. To watch what reaches the
broker, independently of the concentrator:

```bash
podman exec microtel-mqtt-example_mosquitto_1 mosquitto_sub -v -t 'microtel/#'
```

**The concentrator never prints `connected to the broker`.** It keeps retrying
for its first minute, then exits. Check the broker is up and on the port you
passed, and look at its log:
`podman-compose -f examples/leaf_mqtt/mqtt-compose.yaml -p microtel-mqtt-example logs`.

**Messages arrive but are `Malformed`.** Something other than a leaf is
publishing under `microtel/`. Every payload must be one encoded
`ExportTraceServiceRequest` from the leaf library.

**Messages arrive and are `Accepted`, but nothing reaches Grafana.** The
concentrator's `batches_failed` will be non-zero. The shared stack isn't
running; start it with `examples/stack/up.sh`. The broker overlay doesn't
need it, but the concentrator exports to its collector on 4317.

**CMake says libmosquitto isn't found.** Install the development package
(above). On Fedora, `mosquitto-devel` also pulls in `cjson-devel`, because
libmosquitto 2.1's `mosquitto.h` includes `<cjson/cJSON.h>`; a hand-extracted
or partial install without it fails to compile with `'cjson/cJSON.h' file not
found`.

**Port 1883 is already in use.** Another broker is running on the host
(`systemctl status mosquitto`). Stop it, or edit the host port in
`mqtt-compose.yaml` and pass the new one to both programs.

Tear down with `examples/leaf_mqtt/down-mqtt.sh`, then `examples/stack/down.sh`.

## Dependencies

Both MQTT libraries belong to the example alone. coreMQTT (MIT) is fetched by
CMake at configure time and compiled into the leaf binary; libmosquitto
(EPL-2.0 or EDL-1.0) is a system library linked into the concentrator binary.
Neither is linked into any microtel library, so microtel's runtime dependency
closure is unchanged, and neither is distributed with microtel.

## Exit codes

The concentrator returns `0` on success, `1` if `Build()` fails or the MQTT
client can't be started, and `2` if `ForceFlush` did not complete. The leaf
returns `0` on success, `1` if the leaf can't be set up or the broker can't be
reached, `2` for bad arguments, and `3` if an encode or a publish failed.
