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
    g.box(x, 660, 280, 150, OUTER, VIOLET, width=2.2)
    g.text(x + 140, 690, title, INK, 18, "bold", "middle")
    for i, s in enumerate(lines):
        g.text(x + 24, 720 + 22 * i, "– " + s, INK, 15)


def render():
    g = Rough(11)
    _layer(g, 20, 50, GREY, "Application (C++; Python optional)", [])
    g.arrow(340, 74, 340, 98)
    _layer(g, 100, 72, BLUE, "OpenTelemetry Trace API (v1)",
           ["Tracer · Span · Context · W3C Propagators"],
           notes=("include/microtel/", "src/api/  (Track A)"))
    g.arrow(340, 176, 340, 198)
    _layer(g, 200, 94, BLUE, "SDK (minimal, v1)",
           ["Resource · AlwaysOn / AlwaysOff / TraceIdRatio / ParentBased",
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
    g.text(664, 594, "one interface, two implementations", OUTER, 14)

    g.arrow(180, 656, 280, 616)
    g.arrow(500, 656, 400, 616)
    _codec(g, 40, "OTLP/HTTP Codec",
           ["application/x-protobuf", "POST /v1/traces", "Retry-After"])
    _codec(g, 360, "OTLP/gRPC Codec",
           ["5-byte length-prefix framing", ":path: /<svc>/<method>", "te: trailers",
            "parses grpc-status / RetryInfo"])
    g.text(664, 714, "src/wire/http/  (Track B)", OUTER, 14)
    g.text(664, 734, "src/wire/grpc/  (Track C)", OUTER, 14)

    g.arrow(180, 814, 300, 858)
    g.arrow(500, 814, 380, 858)
    _layer(g, 860, 94, GREEN, "Transport: HTTP/2 (nghttp2)",
           ["One connection per endpoint/protocol tuple (default)",
            "TLS via OpenSSL · epoll / kqueue I/O loop"],
           notes=("src/transport/  (Track D)",))
    g.arrow(340, 958, 340, 996)
    g.text(340, 1024, "OTel Collector (any)", OUTER, 17, "bold", "middle")
    g.text(340, 1048, "or any OTLP-compatible backend", OUTER, 14, anchor="middle")

    return g.svg(980, 1070, "microtel layered structure")
