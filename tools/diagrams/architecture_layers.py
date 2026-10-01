"""docs/architecture.md section 2: the runtime's layered structure."""
from sketch import AMBER, BLUE, GREEN, GREY, INK, OUTER, VIOLET, WHITE, Rough

OUTPUT = "architecture-layers.svg"


def _layer(g, y, h, fill, title, lines, x=40, w=600, notes=()):
    g.box(x, y, w, h, OUTER, fill, width=2.2)
    cx = x + w / 2
    g.text(cx, y + 30, title, INK, 18, "bold", "middle")
    for i, s in enumerate(lines):
        g.text(cx, y + 56 + 22 * i, s, INK, 15, anchor="middle")
    for i, s in enumerate(notes):
        g.text(x + w + 24, y + 34 + 20 * i, s, OUTER, 14)


def _codec(g, x, title, lines):
    g.box(x, 660, 190, 150, OUTER, VIOLET, width=2.2)
    g.text(x + 95, 690, title, INK, 17, "bold", "middle")
    for i, s in enumerate(lines):
        g.text(x + 95, 720 + 22 * i, s, INK, 14, anchor="middle")


def render():
    g = Rough(11)
    _layer(g, 20, 50, GREY, "Application (C++)", [])
    g.arrow(340, 74, 340, 98)
    _layer(g, 100, 72, BLUE, "OpenTelemetry API",
           ["Tracer · Span · Context · W3C Propagators · Meter · Logger"],
           notes=("include/microtel/", "src/api/  (Track A)"))
    g.arrow(340, 176, 340, 198)
    _layer(g, 200, 94, BLUE, "SDK",
           ["Resource · head samplers and rule chains (in StartSpan)",
            "BatchSpanProcessor · ForceFlush · Shutdown"],
           notes=("src/sdk/  (Track A)",))
    g.arrow(340, 298, 340, 320)
    _layer(g, 322, 72, BLUE, "Exporter (protocol-agnostic)",
           ["Batching · retry orchestration · drop accounting"],
           notes=("src/exporter/  (Track A)",))
    g.arrow(340, 398, 340, 420)
    _layer(g, 422, 94, AMBER, "OTLP Wire Encoder",
           ["upb (vendored, pinned) + OTel .proto definitions",
            "C accessors wrapped behind a thin C++ API"],
           notes=("src/wire/encoder/  (Track F)",))
    g.arrow(340, 520, 340, 562)
    g.text(356, 546, "EncodedPayload (bytes)", OUTER, 14, style="italic")

    g.box(220, 564, 240, 48, OUTER, WHITE, width=2.2)
    g.text(340, 594, "IWireCodec", INK, 18, "bold", "middle")
    g.text(664, 594, "one interface, three implementations", OUTER, 14)

    g.arrow(135, 656, 290, 616)
    g.arrow(345, 656, 340, 616)
    g.arrow(555, 656, 400, 616)
    _codec(g, 40, "OTLP/HTTP",
           ["application/x-protobuf", "POST /v1/traces", "Retry-After"])
    _codec(g, 250, "OTLP/gRPC",
           ["5-byte length prefix", ":path /<svc>/<method>", "grpc-status, RetryInfo"])
    _codec(g, 460, "Custom transport",
           ["no framing or headers;", "hands the bytes to", "your ExportTransport"])
    g.text(664, 704, "src/wire/http/  (Track B)", OUTER, 14)
    g.text(664, 724, "src/wire/grpc/  (Track C)", OUTER, 14)
    g.text(664, 744, "src/wire/custom/  (ICP 0036)", OUTER, 14)

    g.arrow(135, 814, 200, 858)
    g.arrow(345, 814, 280, 858)
    g.arrow(555, 814, 560, 858)
    _layer(g, 860, 94, GREEN, "Transport: HTTP/2 (nghttp2)",
           ["One connection per endpoint/protocol (default)",
            "TLS via OpenSSL · epoll I/O loop (Linux)"],
           w=400, notes=())
    g.box(470, 860, 180, 94, OUTER, AMBER, width=2.2)
    g.text(560, 890, "ExportTransport", INK, 17, "bold", "middle")
    g.text(560, 916, "your code: UART, CAN,", INK, 14, anchor="middle")
    g.text(560, 936, "UDP, MQTT …", INK, 14, anchor="middle")
    g.text(664, 894, "HTTP/2: src/transport/  (Track D)", OUTER, 14)
    g.text(664, 914, "yours: WithExportTransport()", OUTER, 14)

    g.arrow(240, 958, 240, 996)
    g.text(240, 1024, "OTel Collector (any)", OUTER, 17, "bold", "middle")
    g.text(240, 1048, "or any OTLP-compatible backend", OUTER, 14, anchor="middle")
    g.arrow(560, 958, 560, 996)
    g.text(560, 1024, "your link", OUTER, 17, "bold", "middle")
    g.text(560, 1048, "e.g. to a concentrator", OUTER, 14, anchor="middle")

    return g.svg(980, 1070, "microtel layered structure: Application, API, SDK, Exporter and OTLP wire encoder, then one IWireCodec with three implementations: OTLP/HTTP and OTLP/gRPC over the nghttp2 HTTP/2 transport to a collector, and a custom codec that hands the bytes to an application ExportTransport")
