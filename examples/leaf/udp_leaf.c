/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * udp_leaf.c: a POSIX program that plays a microtel leaf.
 *
 * It does what a small device would: build spans in static memory with the C
 * leaf library, encode them as one OTLP payload, and hand the bytes to its own
 * transport, here one UDP datagram per payload to the concentrator
 * (udp_concentrator.cpp). The leaf library does no I/O; everything below that
 * touches a socket or a clock is this program's, standing in for firmware.
 *
 *   microtel_example_leaf_udp_leaf [stamped|sync|boot] [concentrator-port]
 *                                  [source-port] [payloads] [command]
 *
 * Defaults: stamped, 9310, 9311, 5. Both ports are on 127.0.0.1. The source
 * port matters: the concentrator names each leaf by its address:port, and
 * microtel.toml configures 127.0.0.1:9311 by name.
 *
 * With the literal word `command` as the fifth argument, the leaf does not
 * measure on its own schedule. It waits on its source port for a command
 * datagram from the controller (udp_full_node with a leaf port), reads the
 * W3C traceparent in it, and starts its sensor.read span with
 * microtel_leaf_span_start_remote, so the span is a child of the controller's
 * span in the controller's trace. Here the soil sensor never answers, so every
 * commanded read is slow and ends with status ERROR. One datagram per command,
 * no reply and no retry: a lost command is simply a read that never happens.
 */

#define _POSIX_C_SOURCE 200809L

#include "microtel/leaf.h"

#include <arpa/inet.h>
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
    DEFAULT_CONCENTRATOR_PORT = 9310,
    DEFAULT_SOURCE_PORT = 9311,
    DEFAULT_PAYLOADS = 5,
    MAX_PORT = 65535,
    /* Record buffer: two spans with a few attributes and an event each fit in
     * well under 1 KiB; 2 KiB leaves room. */
    RECORD_BUFFER_BYTES = 2048,
    /* One datagram, kept under a typical 1500-byte MTU. */
    DATAGRAM_BYTES = 1400,
    XS_A = 13,
    XS_B = 7,
    XS_C = 17,
    /* A command is "irrigation.check traceparent=00-<32 hex>-<16 hex>-01". */
    COMMAND_BYTES = 128,
    COMMAND_WAIT_MS = 30000,
    TRACE_ID_BYTES = 16,
    SPAN_ID_BYTES = 8,
    HEX_RADIX = 16,
    HEX_LETTER_BASE = 10,
    /* The simulated soil-moisture sensor: its I2C address, how long the leaf
     * waits for it, and how often it asks. */
    SOIL_SENSOR_ADDRESS = 0x36,
    SOIL_SENSOR_TIMEOUT_MS = 250,
    SOIL_SENSOR_ATTEMPTS = 3
};

#define NS_PER_SEC 1000000000LL
#define NS_PER_MS 1000000L
#define READ_TIME_NS 40000000L    /* the "sensor read" takes 40 ms */
#define PAYLOAD_GAP_NS 500000000L /* half a second between payloads */
#define BASE_TEMPERATURE_C 21.5
#define TEMPERATURE_STEP_C 0.25
#define BASE_HUMIDITY_PCT 48

static microtel_leaf_t g_leaf;
static uint8_t g_records[RECORD_BUFFER_BYTES];
static uint64_t g_rng;

/* The leaf's clock: CLOCK_MONOTONIC, which on Linux counts from boot. */
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

/* xorshift64, seeded per run. A device would use its hardware RNG, or at
 * least its unique chip id and a boot counter (leaf.h). */
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
        /* A device would learn the wall time from the concentrator, GPS or
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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + HEX_LETTER_BASE;
    }
    return -1;
}

/* Lower-case hex, as W3C Trace Context requires, into `len` bytes. */
static int parse_hex(const char* text, uint8_t* out, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
    {
        const int hi = hex_digit(text[2u * i]);
        const int lo = hex_digit(text[2u * i + 1u]);
        if (hi < 0 || lo < 0)
        {
            return -1;
        }
        out[i] = (uint8_t)(hi * HEX_RADIX + lo);
    }
    return 0;
}

/* Finds "traceparent=00-<trace-id>-<span-id>-<flags>" in a command. The flags
 * are not read: a leaf has no sampler, and the controller only sends
 * commands from spans it records. microtel_leaf_span_start_remote refuses
 * all-zero ids. */
static int parse_traceparent(const char* command,
                             uint8_t trace_id[TRACE_ID_BYTES],
                             uint8_t span_id[SPAN_ID_BYTES])
{
    static const char kKey[] = "traceparent=00-";
    /* Offsets after the version: the dash after the trace id, the span id,
     * and the dash after the span id. */
    const size_t trace_dash = (size_t)2 * TRACE_ID_BYTES;
    const size_t span_at = trace_dash + 1u;
    const size_t span_dash = span_at + (size_t)2 * SPAN_ID_BYTES;
    const char* text = strstr(command, kKey);
    if (text == NULL)
    {
        return -1;
    }
    text += sizeof(kKey) - 1u;
    if (strlen(text) <= span_dash || text[trace_dash] != '-' || text[span_dash] != '-')
    {
        return -1;
    }
    if (parse_hex(text, trace_id, TRACE_ID_BYTES) != 0)
    {
        return -1;
    }
    return parse_hex(text + span_at, span_id, SPAN_ID_BYTES);
}

/* Waits up to COMMAND_WAIT_MS for one datagram on `fd`, as a C string. */
static int wait_for_command(int fd, char* out, size_t out_size)
{
    struct pollfd pfd;
    ssize_t n;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, COMMAND_WAIT_MS) <= 0)
    {
        fprintf(stderr, "no command within %d ms\n", COMMAND_WAIT_MS);
        return -1;
    }
    n = recv(fd, out, out_size - 1u, 0);
    if (n < 0)
    {
        perror("recv");
        return -1;
    }
    out[n] = '\0';
    return 0;
}

/* A commanded read of the soil sensor, which does not answer. The span's
 * parent is the controller's span, from the command's traceparent. */
static microtel_leaf_status_t record_commanded_read(const uint8_t trace_id[TRACE_ID_BYTES],
                                                    const uint8_t span_id[SPAN_ID_BYTES])
{
    static const char kRead[] = "sensor.read";
    static const char kTimeout[] = "sensor.timeout";
    static const char kFault[] = "soil-moisture sensor did not answer";
    microtel_leaf_span_t read = 0;
    const microtel_leaf_kv_t read_attrs[] = {
        str_kv("sensor.kind", "soil-moisture"),
        str_kv("sensor.bus", "i2c"),
        int_kv("sensor.address", SOIL_SENSOR_ADDRESS),
    };
    const microtel_leaf_kv_t timeout_attrs[] = {
        int_kv("timeout.ms", SOIL_SENSOR_TIMEOUT_MS),
        int_kv("attempts", SOIL_SENSOR_ATTEMPTS),
    };
    size_t i;
    microtel_leaf_status_t st = microtel_leaf_span_start_remote(&g_leaf,
                                                                &read,
                                                                kRead,
                                                                sizeof(kRead) - 1u,
                                                                MICROTEL_LEAF_SPAN_KIND_SERVER,
                                                                trace_id,
                                                                span_id);
    for (i = 0; st == MICROTEL_LEAF_OK && i < sizeof(read_attrs) / sizeof(read_attrs[0]); ++i)
    {
        st = microtel_leaf_span_set_attribute(&g_leaf, read, &read_attrs[i]);
    }
    sleep_ns((long)SOIL_SENSOR_TIMEOUT_MS * NS_PER_MS);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_add_event(&g_leaf,
                                          read,
                                          kTimeout,
                                          sizeof(kTimeout) - 1u,
                                          timeout_attrs,
                                          sizeof(timeout_attrs) / sizeof(timeout_attrs[0]));
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_status(
            &g_leaf, read, MICROTEL_LEAF_STATUS_ERROR, kFault, sizeof(kFault) - 1u);
    }
    return st == MICROTEL_LEAF_OK ? microtel_leaf_span_end(&g_leaf, read) : st;
}

/* Waits for a command and records the read it asks for. */
static microtel_leaf_status_t serve_command(int fd, int cycle)
{
    char command[COMMAND_BYTES];
    uint8_t trace_id[TRACE_ID_BYTES];
    uint8_t span_id[SPAN_ID_BYTES];
    if (wait_for_command(fd, command, sizeof(command)) != 0)
    {
        return MICROTEL_LEAF_ERR_STATE;
    }
    printf("command %d: \"%s\"\n", cycle, command);
    if (parse_traceparent(command, trace_id, span_id) != 0)
    {
        fprintf(stderr, "command %d: no valid traceparent\n", cycle);
        return MICROTEL_LEAF_ERR_ARG;
    }
    return record_commanded_read(trace_id, span_id);
}

static int open_socket(int source_port)
{
    struct sockaddr_in local;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)source_port);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr*)&local, sizeof(local)) != 0)
    {
        perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

/* `commands`: wait for a command before each payload instead of measuring on
 * the leaf's own schedule. */
static int send_payloads(int fd, int concentrator_port, int payloads, int commands)
{
    uint8_t datagram[DATAGRAM_BYTES];
    struct sockaddr_in to;
    int cycle;
    const int spans = commands ? 1 : 2;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)concentrator_port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (cycle = 1; cycle <= payloads; ++cycle)
    {
        size_t written = 0;
        microtel_leaf_status_t st = commands ? serve_command(fd, cycle) : record_cycle(cycle);
        if (st == MICROTEL_LEAF_OK)
        {
            st = microtel_leaf_encode(&g_leaf, datagram, sizeof(datagram), &written);
        }
        if (st != MICROTEL_LEAF_OK)
        {
            fprintf(stderr, "payload %d: leaf error %d\n", cycle, (int)st);
            return -1;
        }
        if (sendto(fd, datagram, written, 0, (const struct sockaddr*)&to, sizeof(to)) < 0)
        {
            perror("sendto");
            return -1;
        }
        printf("payload %d: %zu bytes, %d span%s -> 127.0.0.1:%d\n",
               cycle,
               written,
               spans,
               spans == 1 ? "" : "s",
               concentrator_port);
        if (!commands)
        {
            sleep_ns(PAYLOAD_GAP_NS);
        }
    }
    return 0;
}

static int parse_port(const char* arg, int fallback)
{
    const long value = arg != NULL ? strtol(arg, NULL, 10) : fallback;
    return value > 0 && value <= MAX_PORT ? (int)value : -1;
}

int main(int argc, char** argv)
{
    microtel_leaf_time_mode_t mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    const int concentrator_port = parse_port(argc > 2 ? argv[2] : NULL, DEFAULT_CONCENTRATOR_PORT);
    const int source_port = parse_port(argc > 3 ? argv[3] : NULL, DEFAULT_SOURCE_PORT);
    const int payloads = argc > 4 ? atoi(argv[4]) : DEFAULT_PAYLOADS;
    const int commands = argc > 5 && strcmp(argv[5], "command") == 0;
    int fd;
    int rc;

    if ((argc > 1 && parse_mode(argv[1], &mode) != 0) || concentrator_port < 0 || source_port < 0 ||
        payloads < 1 || (argc > 5 && !commands))
    {
        fprintf(stderr,
                "usage: %s [stamped|sync|boot] [concentrator-port] [source-port] [payloads] "
                "[command]\n",
                argv[0]);
        return 2;
    }

    g_rng = read_clock(CLOCK_REALTIME) ^ ((uint64_t)getpid() << 32u);
    if (init_leaf(mode) != 0)
    {
        return 1;
    }
    fd = open_socket(source_port);
    if (fd < 0)
    {
        microtel_leaf_free(&g_leaf);
        return 1;
    }
    printf("leaf 127.0.0.1:%d, time mode %s, leaf library %u.%u.%u\n",
           source_port,
           argc > 1 ? argv[1] : "stamped",
           (unsigned)(microtel_leaf_version() >> 16u),
           (unsigned)((microtel_leaf_version() >> 8u) & 0xffu),
           (unsigned)(microtel_leaf_version() & 0xffu));
    if (commands)
    {
        printf("waiting for commands on 127.0.0.1:%d\n", source_port);
    }
    rc = send_payloads(fd, concentrator_port, payloads, commands);
    close(fd);
    microtel_leaf_free(&g_leaf);
    return rc == 0 ? 0 : 3;
}
