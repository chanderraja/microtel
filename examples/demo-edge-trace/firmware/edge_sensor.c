/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The demo's device: bare-metal Cortex-M4 firmware using the microtel leaf
 * library (C11, static memory, no I/O of its own).
 *
 * Line protocol on UART0, one command and one answer per request:
 *
 *   gateway -> device   "CMD <W3C traceparent>"
 *   device  -> gateway  "LOG <text>"          (any number, informational)
 *   device  -> gateway  "PAYLOAD <hex>"       (the encoded OTLP leaf payload)
 *
 * The device parses the traceparent, starts `sensor.read` as a child of the
 * gateway's span with microtel_leaf_span_start_remote, records a child
 * `adc.sample`, encodes the batch, and sends it back hex-encoded. The gateway
 * hands the bytes to its concentrator (LeafReceiver::Ingest).
 */
#include "board.h"

#include "microtel/leaf.h"

#include <stdint.h>
#include <string.h>

enum
{
    RECORD_BUFFER_BYTES = 2048,
    PAYLOAD_BYTES = 1400,
    ENCODE_SCRATCH_BYTES = 8192,
    LINE_BYTES = 160,
    TRACE_ID_BYTES = 16,
    SPAN_ID_BYTES = 8,
    HEX_RADIX = 16,
    HEX_LETTER_BASE = 10,
    ADC_SAMPLE_MS = 12,
    SENSOR_SETTLE_MS = 25,
    BASE_TEMP_CENTI = 2150
};

static microtel_leaf_t g_leaf;
static uint8_t g_records[RECORD_BUFFER_BYTES];
static uint8_t g_scratch[ENCODE_SCRATCH_BYTES] __attribute__((aligned(8)));
static uint8_t g_payload[PAYLOAD_BYTES];
static uint32_t g_rng = 0x9e3779b9u;

static uint64_t leaf_now_ns(void* ctx)
{
    (void)ctx;
    return board_now_ns();
}

/* xorshift32 mixed with the tick. A real device would use its TRNG. */
static void leaf_random(void* ctx, uint8_t* out, size_t len)
{
    size_t i;
    (void)ctx;
    g_rng ^= (uint32_t)board_now_ns();
    for (i = 0; i < len; ++i)
    {
        g_rng ^= g_rng << 13u;
        g_rng ^= g_rng >> 17u;
        g_rng ^= g_rng << 5u;
        out[i] = (uint8_t)g_rng;
    }
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

/* "00-<32 hex trace id>-<16 hex span id>-<2 hex flags>" */
static int parse_traceparent(const char* text, uint8_t* trace_id, uint8_t* span_id)
{
    if (strlen(text) < 55u || text[0] != '0' || text[1] != '0' || text[2] != '-' ||
        text[35] != '-' || text[52] != '-')
    {
        return -1;
    }
    if (parse_hex(text + 3, trace_id, TRACE_ID_BYTES) != 0)
    {
        return -1;
    }
    return parse_hex(text + 36, span_id, SPAN_ID_BYTES);
}

static void put_hex_bytes(const uint8_t* data, size_t len)
{
    static const char kDigits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; ++i)
    {
        board_uart_putc(kDigits[data[i] >> 4u]);
        board_uart_putc(kDigits[data[i] & 0xfu]);
    }
}

static void put_uint(uint32_t value)
{
    char text[11];
    size_t n = sizeof(text) - 1u;
    text[n] = '\0';
    do
    {
        text[--n] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && n > 0u);
    board_uart_puts(&text[n]);
}

static int init_leaf(void)
{
    const microtel_leaf_kv_t resource[] = {
        str_kv("service.name", "mcu-sensor"),
        str_kv("service.version", "1.0.0"),
        str_kv("device.model.identifier", "mps2-an386"),
        str_kv("host.arch", "arm32"),
        str_kv("cpu.model", "cortex-m4"),
    };
    static const char kScope[] = "edge.firmware";
    microtel_leaf_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.time_mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    config.now_ns = leaf_now_ns;
    config.random_bytes = leaf_random;
    config.resource = resource;
    config.resource_count = sizeof(resource) / sizeof(resource[0]);
    config.scope_name = kScope;
    config.scope_name_len = sizeof(kScope) - 1u;
    config.boot_id = 1u;
    config.scratch = g_scratch;
    config.scratch_size = sizeof(g_scratch);
    return microtel_leaf_init(&g_leaf, sizeof(g_leaf), &config, g_records, sizeof(g_records)) ==
                   MICROTEL_LEAF_OK
               ? 0
               : -1;
}

/* sensor.read (SERVER, child of the gateway's span) -> adc.sample. */
static microtel_leaf_status_t serve_read(const uint8_t* trace_id,
                                         const uint8_t* span_id,
                                         uint32_t request)
{
    static const char kRead[] = "sensor.read";
    static const char kSample[] = "adc.sample";
    static const char kDone[] = "sample.ready";
    microtel_leaf_span_t read = 0;
    microtel_leaf_span_t sample = 0;
    const int32_t temp_centi = BASE_TEMP_CENTI + (int32_t)(request % 8u) * 15;
    const microtel_leaf_kv_t read_attrs[] = {
        str_kv("sensor.kind", "bme280"),
        str_kv("sensor.bus", "i2c"),
        int_kv("request.seq", (int64_t)request),
    };
    const microtel_leaf_kv_t sample_attrs[] = {
        int_kv("adc.channel", 3),
        double_kv("temperature.celsius", (double)temp_centi / 100.0),
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
    board_delay_ms(SENSOR_SETTLE_MS);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_start(&g_leaf,
                                      &sample,
                                      kSample,
                                      sizeof(kSample) - 1u,
                                      MICROTEL_LEAF_SPAN_KIND_INTERNAL,
                                      &read);
    }
    for (i = 0; st == MICROTEL_LEAF_OK && i < sizeof(sample_attrs) / sizeof(sample_attrs[0]); ++i)
    {
        st = microtel_leaf_span_set_attribute(&g_leaf, sample, &sample_attrs[i]);
    }
    board_delay_ms(ADC_SAMPLE_MS);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(&g_leaf, sample);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_add_event(&g_leaf, read, kDone, sizeof(kDone) - 1u, NULL, 0);
    }
    return st == MICROTEL_LEAF_OK ? microtel_leaf_span_end(&g_leaf, read) : st;
}

int main(void)
{
    static char line[LINE_BYTES];
    uint32_t request = 0;

    if (init_leaf() != 0)
    {
        board_uart_puts("LOG microtel_leaf_init failed\n");
        return 1;
    }
    board_uart_puts("LOG edge-sensor ready: cortex-m4 (mps2-an386), microtel leaf\n");

    for (;;)
    {
        uint8_t trace_id[TRACE_ID_BYTES];
        uint8_t span_id[SPAN_ID_BYTES];
        size_t written = 0;
        microtel_leaf_status_t st;

        (void)board_uart_getline(line, sizeof(line));
        if (strncmp(line, "CMD ", 4) != 0)
        {
            continue;
        }
        ++request;
        if (parse_traceparent(line + 4, trace_id, span_id) != 0)
        {
            board_uart_puts("LOG bad traceparent\n");
            board_uart_puts("PAYLOAD \n");
            continue;
        }
        st = serve_read(trace_id, span_id, request);
        if (st == MICROTEL_LEAF_OK)
        {
            st = microtel_leaf_encode(&g_leaf, g_payload, sizeof(g_payload), &written);
        }
        if (st != MICROTEL_LEAF_OK)
        {
            board_uart_puts("LOG leaf error\n");
            board_uart_puts("PAYLOAD \n");
            continue;
        }
        board_uart_puts("LOG request ");
        put_uint(request);
        board_uart_puts(": 2 spans, ");
        put_uint((uint32_t)written);
        board_uart_puts(" bytes\n");
        board_uart_puts("PAYLOAD ");
        put_hex_bytes(g_payload, written);
        board_uart_putc('\n');
    }
}
