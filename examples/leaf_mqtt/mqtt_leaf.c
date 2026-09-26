/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * mqtt_leaf.c: a POSIX program that plays a microtel leaf speaking MQTT.
 *
 * It builds spans in static memory with the C leaf library, encodes each batch
 * as one OTLP payload, and publishes the payload to the topic
 * microtel/<device-id>/traces with coreMQTT, the MQTT 3.1.1 client FreeRTOS
 * firmware uses. The concentrator (mqtt_concentrator.cpp) subscribes to
 * microtel/+/traces and names each leaf by the middle level of the topic.
 *
 * coreMQTT does no I/O of its own either: it calls a TransportInterface_t the
 * application supplies. The one below is a few lines of POSIX TCP, standing in
 * for the device's network stack (lwIP, FreeRTOS+TCP, a cellular modem's AT
 * socket API, or a TLS layer over any of them). Everything that touches a
 * socket or a clock is this program's, not microtel's.
 *
 *   microtel_example_leaf_mqtt_leaf [stamped|sync|boot] [device-id]
 *                                   [broker-port] [payloads]
 *
 * Defaults: stamped, gh-north-01, 1883, 5. The broker is on 127.0.0.1.
 *
 * Payloads are published at QoS 1. A payload is a batch of finished spans that
 * the leaf has already released, so losing one loses those traces for good;
 * QoS 1 has the broker acknowledge it (PUBACK) before the next one goes out.
 * The price is at-least-once delivery: a PUBACK lost to a dropped connection
 * can make the concentrator see the same spans twice. QoS 2 would rule that
 * out for two more round trips per payload, which a battery-powered device
 * rarely wants to spend on telemetry.
 */

#define _POSIX_C_SOURCE 200809L

#include "microtel/leaf.h"

#include "core_mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum
{
    DEFAULT_BROKER_PORT = 1883,
    DEFAULT_PAYLOADS = 5,
    MAX_PORT = 65535,
    /* A topic level is at most this long here; MQTT allows far more, but a
     * device id is an identifier, not a sentence. */
    MAX_DEVICE_ID = 64,
    TOPIC_BYTES = 96,
    /* Record buffer: two spans with a few attributes and an event each fit in
     * well under 1 KiB; 2 KiB leaves room. */
    RECORD_BUFFER_BYTES = 2048,
    /* One payload. MQTT has no MTU to fit under; the concentrator's
     * max_payload_bytes (64 KiB by default) is the real limit. */
    PAYLOAD_BYTES = 4096,
    /* coreMQTT's working buffer. Outgoing payloads are sent straight from the
     * caller's memory, so this only has to hold packet headers and what the
     * broker sends back (CONNACK, PUBACK, PINGRESP). */
    NETWORK_BUFFER_BYTES = 512,
    /* QoS 1 publishes awaiting a PUBACK. This leaf waits for each one. */
    OUTGOING_PUBLISH_RECORDS = 1,
    KEEP_ALIVE_SECONDS = 60,
    CONNACK_TIMEOUT_MS = 5000,
    PUBACK_TIMEOUT_MS = 5000,
    /* How long one transport read waits for bytes before reporting "none
     * yet". coreMQTT expects a receive that does not block for long. */
    RECV_POLL_MS = 10,
    SEND_POLL_MS = 1000,
    XS_A = 13,
    XS_B = 7,
    XS_C = 17
};

#define NS_PER_SEC 1000000000LL
#define NS_PER_MS 1000000LL
#define READ_TIME_NS 40000000L    /* the "sensor read" takes 40 ms */
#define PAYLOAD_GAP_NS 500000000L /* half a second between payloads */
#define BASE_TEMPERATURE_C 21.5
#define TEMPERATURE_STEP_C 0.25
#define BASE_HUMIDITY_PCT 48

static microtel_leaf_t g_leaf;
static uint8_t g_records[RECORD_BUFFER_BYTES];
static uint64_t g_rng;

/* The packet id of the last PUBACK coreMQTT handed to on_mqtt_event. */
static uint16_t g_acked_packet_id;

/* ---- Clock and randomness, as in udp_leaf.c ---------------------------- */

static uint64_t read_clock(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * (uint64_t)NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

static uint64_t leaf_now_ns(void* ctx)
{
    (void)ctx;
    return read_clock(CLOCK_MONOTONIC);
}

/* coreMQTT's clock: milliseconds from any epoch, allowed to wrap. */
static uint32_t mqtt_now_ms(void)
{
    return (uint32_t)(read_clock(CLOCK_MONOTONIC) / (uint64_t)NS_PER_MS);
}

/* xorshift64, seeded per run. A device would use its hardware RNG. */
static void leaf_random(void* ctx, uint8_t* out, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; ++i)
    {
        g_rng ^= g_rng << XS_A;
        g_rng ^= g_rng >> XS_B;
        g_rng ^= g_rng << XS_C;
        out[i] = (uint8_t)g_rng;
    }
}

static void sleep_ns(long ns)
{
    struct timespec ts;
    ts.tv_sec = ns / NS_PER_SEC;
    ts.tv_nsec = ns % NS_PER_SEC;
    nanosleep(&ts, NULL);
}

/* ---- The transport shim ------------------------------------------------ */

/*
 * coreMQTT declares struct NetworkContext and leaves its contents to the
 * application. On a device it would hold an lwIP or FreeRTOS+TCP socket, and
 * a TLS session for port 8883; here it is a POSIX file descriptor.
 */
struct NetworkContext
{
    int fd;
};

/* Returns the bytes read, 0 for "nothing yet, call again", or -1 when the
 * connection is gone. coreMQTT forbids returning 0 for a closed connection,
 * which is why recv()'s 0 (orderly shutdown) becomes -1. */
static int32_t transport_recv(NetworkContext_t* ctx, void* buffer, size_t len)
{
    struct pollfd pfd;
    ssize_t n;
    int ready;

    pfd.fd = ctx->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ready = poll(&pfd, 1, RECV_POLL_MS);
    if (ready <= 0)
    {
        return ready == 0 || errno == EINTR ? 0 : -1;
    }
    n = recv(ctx->fd, buffer, len, 0);
    if (n > 0)
    {
        return (int32_t)n;
    }
    return n < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : -1;
}

/* Returns the bytes written, 0 if the socket could take none right now, or -1
 * on error. MSG_NOSIGNAL turns a write to a closed socket into EPIPE instead of
 * a SIGPIPE that would end the process. */
static int32_t transport_send(NetworkContext_t* ctx, const void* buffer, size_t len)
{
    struct pollfd pfd;
    ssize_t n;
    int ready;

    pfd.fd = ctx->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    ready = poll(&pfd, 1, SEND_POLL_MS);
    if (ready <= 0)
    {
        return ready == 0 || errno == EINTR ? 0 : -1;
    }
    n = send(ctx->fd, buffer, len, MSG_NOSIGNAL);
    if (n >= 0)
    {
        return (int32_t)n;
    }
    return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

static int tcp_connect_loopback(int port)
{
    struct sockaddr_in broker;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }
    memset(&broker, 0, sizeof(broker));
    broker.sin_family = AF_INET;
    broker.sin_port = htons((uint16_t)port);
    broker.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (const struct sockaddr*)&broker, sizeof(broker)) != 0)
    {
        perror("connect to broker");
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- MQTT -------------------------------------------------------------- */

static void on_mqtt_event(MQTTContext_t* mqtt,
                          MQTTPacketInfo_t* packet,
                          MQTTDeserializedInfo_t* info)
{
    (void)mqtt;
    if (packet->type == MQTT_PACKET_TYPE_PUBACK)
    {
        g_acked_packet_id = info->packetIdentifier;
    }
}

/* MQTT_Init and MQTT_Connect. The client id is the device id, which is what
 * lets a broker ACL tie the topic to the client (README, "Identity"). */
static MQTTStatus_t mqtt_connect(MQTTContext_t* mqtt,
                                 NetworkContext_t* network,
                                 const char* device_id)
{
    static uint8_t network_buffer[NETWORK_BUFFER_BYTES];
    static MQTTPubAckInfo_t outgoing[OUTGOING_PUBLISH_RECORDS];
    TransportInterface_t transport;
    MQTTFixedBuffer_t buffer;
    MQTTConnectInfo_t connect;
    bool session_present = false;
    MQTTStatus_t st;

    memset(&transport, 0, sizeof(transport));
    transport.recv = transport_recv;
    transport.send = transport_send;
    transport.pNetworkContext = network;
    buffer.pBuffer = network_buffer;
    buffer.size = sizeof(network_buffer);

    st = MQTT_Init(mqtt, &transport, mqtt_now_ms, on_mqtt_event, &buffer);
    if (st == MQTTSuccess)
    {
        /* QoS 1 needs somewhere to track the unacknowledged publish. */
        st = MQTT_InitStatefulQoS(mqtt, outgoing, OUTGOING_PUBLISH_RECORDS, NULL, 0);
    }
    if (st != MQTTSuccess)
    {
        return st;
    }
    memset(&connect, 0, sizeof(connect));
    connect.cleanSession = true;
    connect.keepAliveSeconds = KEEP_ALIVE_SECONDS;
    connect.pClientIdentifier = device_id;
    connect.clientIdentifierLength = (uint16_t)strlen(device_id);
    return MQTT_Connect(mqtt, &connect, NULL, CONNACK_TIMEOUT_MS, &session_present);
}

/* Publishes one payload at QoS 1 and drives coreMQTT until its PUBACK comes
 * back. A device with more to do would call MQTT_ProcessLoop from its main
 * loop instead of spinning here. */
static MQTTStatus_t publish_and_wait(MQTTContext_t* mqtt,
                                     const char* topic,
                                     const uint8_t* payload,
                                     size_t len)
{
    MQTTPublishInfo_t publish;
    uint16_t packet_id;
    uint32_t started;
    MQTTStatus_t st;

    memset(&publish, 0, sizeof(publish));
    publish.qos = MQTTQoS1;
    publish.pTopicName = topic;
    publish.topicNameLength = (uint16_t)strlen(topic);
    publish.pPayload = payload;
    publish.payloadLength = len;

    packet_id = MQTT_GetPacketId(mqtt);
    st = MQTT_Publish(mqtt, &publish, packet_id);
    started = mqtt_now_ms();
    while ((st == MQTTSuccess || st == MQTTNeedMoreBytes) && g_acked_packet_id != packet_id)
    {
        if ((uint32_t)(mqtt_now_ms() - started) > (uint32_t)PUBACK_TIMEOUT_MS)
        {
            return MQTTRecvFailed;
        }
        st = MQTT_ProcessLoop(mqtt);
    }
    return st == MQTTNeedMoreBytes ? MQTTSuccess : st;
}

/* ---- The leaf, as in udp_leaf.c ---------------------------------------- */

static microtel_leaf_kv_t str_kv(const char* key, const char* value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_STRING;
    kv.value.s.ptr = value;
    kv.value.s.len = strlen(value);
    return kv;
}

static microtel_leaf_kv_t int_kv(const char* key, int64_t value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_INT64;
    kv.value.i = value;
    return kv;
}

static microtel_leaf_kv_t double_kv(const char* key, double value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_DOUBLE;
    kv.value.d = value;
    return kv;
}

static int parse_mode(const char* arg, microtel_leaf_time_mode_t* mode)
{
    if (strcmp(arg, "stamped") == 0)
    {
        *mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    }
    else if (strcmp(arg, "sync") == 0)
    {
        *mode = MICROTEL_LEAF_TIME_SYNC_RELATIVE;
    }
    else if (strcmp(arg, "boot") == 0)
    {
        *mode = MICROTEL_LEAF_TIME_BOOT_RELATIVE;
    }
    else
    {
        return -1;
    }
    return 0;
}

static int init_leaf(microtel_leaf_time_mode_t mode)
{
    const microtel_leaf_kv_t resource[] = {
        str_kv("service.name", "greenhouse-sensor"),
        str_kv("service.version", "0.3.1"),
    };
    static const char kScope[] = "greenhouse.firmware";
    microtel_leaf_config_t config;
    microtel_leaf_status_t st;

    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.time_mode = mode;
    config.now_ns = leaf_now_ns;
    config.random_bytes = leaf_random;
    config.resource = resource;
    config.resource_count = sizeof(resource) / sizeof(resource[0]);
    config.scope_name = kScope;
    config.scope_name_len = sizeof(kScope) - 1u;
    config.boot_id = (uint32_t)g_rng | 1u;

    st = microtel_leaf_init(&g_leaf, sizeof(g_leaf), &config, g_records, sizeof(g_records));
    if (st != MICROTEL_LEAF_OK)
    {
        fprintf(stderr, "microtel_leaf_init failed: %d\n", (int)st);
        return -1;
    }
    if (mode == MICROTEL_LEAF_TIME_SYNC_RELATIVE)
    {
        /* A device would learn the wall time from the concentrator, SNTP or
         * an RTC; this one reads the host's clock once. */
        st = microtel_leaf_clock_sync(
            &g_leaf, read_clock(CLOCK_REALTIME), read_clock(CLOCK_MONOTONIC));
        if (st != MICROTEL_LEAF_OK)
        {
            fprintf(stderr, "microtel_leaf_clock_sync failed: %d\n", (int)st);
            return -1;
        }
    }
    return 0;
}

/* One measurement cycle: a root span with a child for the sensor read. */
static microtel_leaf_status_t record_cycle(int cycle)
{
    static const char kCycle[] = "greenhouse.cycle";
    static const char kRead[] = "sensor.read";
    static const char kSample[] = "sample.taken";
    microtel_leaf_span_t root = 0;
    microtel_leaf_span_t read = 0;
    const microtel_leaf_kv_t cycle_kv = int_kv("cycle", cycle);
    const microtel_leaf_kv_t read_attrs[] = {
        str_kv("sensor.kind", "bme280"),
        double_kv("temperature.celsius", BASE_TEMPERATURE_C + TEMPERATURE_STEP_C * cycle),
        int_kv("humidity.percent", BASE_HUMIDITY_PCT + cycle),
    };
    size_t i;
    microtel_leaf_status_t st = microtel_leaf_span_start(
        &g_leaf, &root, kCycle, sizeof(kCycle) - 1u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_attribute(&g_leaf, root, &cycle_kv);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_start(
            &g_leaf, &read, kRead, sizeof(kRead) - 1u, MICROTEL_LEAF_SPAN_KIND_CLIENT, &root);
    }
    for (i = 0; st == MICROTEL_LEAF_OK && i < sizeof(read_attrs) / sizeof(read_attrs[0]); ++i)
    {
        st = microtel_leaf_span_set_attribute(&g_leaf, read, &read_attrs[i]);
    }
    sleep_ns(READ_TIME_NS);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_add_event(&g_leaf, read, kSample, sizeof(kSample) - 1u, NULL, 0);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(&g_leaf, read);
    }
    return st == MICROTEL_LEAF_OK ? microtel_leaf_span_end(&g_leaf, root) : st;
}

static int send_payloads(MQTTContext_t* mqtt, const char* topic, int payloads)
{
    static uint8_t payload[PAYLOAD_BYTES];
    int cycle;

    for (cycle = 1; cycle <= payloads; ++cycle)
    {
        size_t written = 0;
        MQTTStatus_t mst;
        microtel_leaf_status_t st = record_cycle(cycle);
        if (st == MICROTEL_LEAF_OK)
        {
            st = microtel_leaf_encode(&g_leaf, payload, sizeof(payload), &written);
        }
        if (st != MICROTEL_LEAF_OK)
        {
            fprintf(stderr, "payload %d: leaf error %d\n", cycle, (int)st);
            return -1;
        }
        mst = publish_and_wait(mqtt, topic, payload, written);
        if (mst != MQTTSuccess)
        {
            fprintf(stderr, "payload %d: publish failed: %s\n", cycle, MQTT_Status_strerror(mst));
            return -1;
        }
        printf("payload %d: %zu bytes, 2 spans -> %s  PUBACK\n", cycle, written, topic);
        sleep_ns(PAYLOAD_GAP_NS);
    }
    return 0;
}

/* ---- main -------------------------------------------------------------- */

static int parse_port(const char* arg, int fallback)
{
    const long value = arg != NULL ? strtol(arg, NULL, 10) : fallback;
    return value > 0 && value <= MAX_PORT ? (int)value : -1;
}

/* A device id becomes one topic level and the MQTT client id, so it may not
 * contain the level separator or either wildcard. */
static int valid_device_id(const char* id)
{
    const size_t len = strlen(id);
    return len > 0 && len <= MAX_DEVICE_ID && strpbrk(id, "/+#") == NULL;
}

static void print_banner(const char* device_id, int port, const char* mode)
{
    const uint32_t v = microtel_leaf_version();
    printf("leaf %s -> mqtt://127.0.0.1:%d, time mode %s, leaf library %u.%u.%u\n",
           device_id,
           port,
           mode,
           (unsigned)(v >> 16u),
           (unsigned)((v >> 8u) & 0xffu),
           (unsigned)(v & 0xffu));
}

/* Connects, publishes, disconnects. Returns 0, or 1 if the broker could not
 * be reached, or 3 if a publish failed. */
static int run(const char* device_id, int port, int payloads)
{
    MQTTContext_t mqtt;
    NetworkContext_t network;
    char topic[TOPIC_BYTES];
    MQTTStatus_t st;
    int rc;

    snprintf(topic, sizeof(topic), "microtel/%s/traces", device_id);
    network.fd = tcp_connect_loopback(port);
    if (network.fd < 0)
    {
        fprintf(stderr, "is the broker up? examples/leaf_mqtt/up-mqtt.sh\n");
        return 1;
    }
    memset(&mqtt, 0, sizeof(mqtt));
    st = mqtt_connect(&mqtt, &network, device_id);
    if (st != MQTTSuccess)
    {
        fprintf(stderr, "MQTT connect failed: %s\n", MQTT_Status_strerror(st));
        close(network.fd);
        return 1;
    }
    printf("connected as client id %s, MQTT 3.1.1, QoS 1\n", device_id);
    rc = send_payloads(&mqtt, topic, payloads);
    /* DISCONNECT tells the broker this was on purpose. */
    MQTT_Disconnect(&mqtt);
    close(network.fd);
    return rc == 0 ? 0 : 3;
}

int main(int argc, char** argv)
{
    microtel_leaf_time_mode_t mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    const char* device_id = argc > 2 ? argv[2] : "gh-north-01";
    const int port = parse_port(argc > 3 ? argv[3] : NULL, DEFAULT_BROKER_PORT);
    const int payloads = argc > 4 ? atoi(argv[4]) : DEFAULT_PAYLOADS;
    int rc;

    if ((argc > 1 && parse_mode(argv[1], &mode) != 0) || !valid_device_id(device_id) || port < 0 ||
        payloads < 1)
    {
        fprintf(stderr,
                "usage: %s [stamped|sync|boot] [device-id] [broker-port] [payloads]\n"
                "  device-id: 1-%d characters, no '/', '+' or '#'\n",
                argv[0],
                MAX_DEVICE_ID);
        return 2;
    }

    g_rng = read_clock(CLOCK_REALTIME) ^ ((uint64_t)getpid() << 32u);
    if (init_leaf(mode) != 0)
    {
        return 1;
    }
    print_banner(device_id, port, argc > 1 ? argv[1] : "stamped");
    rc = run(device_id, port, payloads);
    microtel_leaf_free(&g_leaf);
    return rc;
}
