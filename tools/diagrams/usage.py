"""README "How you use it": the native C++ API, and the opentelemetry-cpp shim."""
from sketch import ACCENT, AMBER, BLUE, GREEN, GREY, INK, NOTE, OUTER, STRIKE, VIOLET, WHITE, Rough


def _card(g, x, y, w, h, fill, title, lines=(), tag=None, mono=()):
    g.box(x, y, w, h, OUTER, fill, width=2.2)
    cx = x + w / 2
    ty = y + 30
    if tag:
        g.text(x + 14, y + 22, tag, ACCENT, 13, "bold")
        ty = y + 44
    g.text(cx, ty, title, INK, 17, "bold", "middle")
    yy = ty + 26
    for s in lines:
        g.text(cx, yy, s, INK, 14, anchor="middle")
        yy += 20
    for s in mono:
        g.mono(cx, yy, s)
        yy += 19


def render_cpp_api():
    g = Rough(21)
    # Row 1: configuration -> builder -> provider
    for i, s in enumerate(("code: .With…()", "env: OTEL_*", "file: microtel.toml")):
        g.box(30, 52 + 46 * i, 190, 34, OUTER, GREY, width=1.6)
        g.text(125, 74 + 46 * i, s, INK, 14, anchor="middle")
    g.text(125, 34, "configuration", OUTER, 14, "bold", "middle")
    g.arrow(228, 118, 290, 118)

    _card(g, 300, 50, 270, 136, AMBER, "SdkBuilder", ["validates everything up front"],
          tag="① startup, once", mono=(".Build()", "→ Expected<Provider, Error>"))
    g.arrow(578, 118, 640, 118)

    _card(g, 650, 40, 300, 156, BLUE, "Provider", [], tag="②",
          mono=("GetTracer() · GetMeter()", "GetLogger()", "GetExporterHealth()",
                "ForceFlush() · Shutdown()"))

    # Row 2: your code -> microtel background -> collector
    g.arrow(760, 204, 330, 268)
    g.text(585, 258, "tracers, meters, loggers", OUTER, 14, style="italic", anchor="middle")

    _card(g, 30, 272, 330, 170, AMBER, "Your code, any thread", [], tag="③ hot path",
          mono=('span = tracer->StartSpan("GET /cart")', "span->SetAttribute(…)",
                "counter->Add(1, attrs)", "logger->Emit(record)"))
    g.text(195, 470, "noexcept · never waits on the network", OUTER, 14, style="italic", anchor="middle")
    g.arrow(368, 356, 430, 356)
    g.text(399, 340, "queue", OUTER, 13, anchor="middle")

    _card(g, 440, 272, 300, 170, GREEN, "microtel background threads", [
        "batch · sample · retry", "OTLP encode (upb)", "HTTP/2 + TLS (nghttp2)"], tag="④")
    g.arrow(748, 356, 810, 356)
    g.text(779, 340, "OTLP", OUTER, 14, "bold", "middle")
    g.text(779, 380, "gRPC /", OUTER, 13, anchor="middle")
    g.text(779, 396, "HTTP", OUTER, 13, anchor="middle")

    g.box(820, 306, 140, 100, OUTER, WHITE, width=2.2)
    g.text(890, 346, "Collector", INK, 17, "bold", "middle")
    g.text(890, 370, "or any OTLP", INK, 14, anchor="middle")
    g.text(890, 388, "backend", INK, 14, anchor="middle")
    return g.svg(990, 500, "Using microtel's C++ API: configuration feeds SdkBuilder, which builds a "
                 "Provider once at startup; your code gets tracers, meters and loggers from it and records "
                 "spans, metrics and logs on the hot path without blocking; microtel's background threads "
                 "batch, encode and send them over OTLP gRPC or HTTP to a collector.")


def render_otelcpp_shim():
    g = Rough(33)
    # Existing instrumented code, several libraries
    for off in (14, 7):
        g.box(30 + off, 60 - off, 300, 176, OUTER, AMBER, width=1.4)
    _card(g, 30, 60, 300, 176, AMBER, "Your instrumented code", ["unchanged, within the", "supported API subset"],
          mono=("tracer->StartSpan(…)", "meter->CreateUInt64Counter(…)", "logger->EmitLogRecord(…)"))
    g.arrow(338, 148, 400, 148)
    g.text(369, 132, "calls", OUTER, 13, anchor="middle")

    _card(g, 410, 70, 250, 156, GREY, "opentelemetry-cpp API", ["header-only, v1.28",
          "global providers:"], mono=("GetTracerProvider()", "GetMeterProvider() …"))
    g.arrow(668, 148, 730, 148)
    g.text(699, 132, "resolve to", OUTER, 13, anchor="middle")

    _card(g, 740, 60, 220, 176, VIOLET, "microtel shim", ["source-only adapter:",
          "tracer, meter, logger,", "attribute & context shims"],
          mono=("-DMICROTEL_BUILD_", "OTELCPP_SHIM=ON"))

    # Startup: the one source change
    _card(g, 410, 296, 250, 150, AMBER, "startup.cpp", ["the one source change,", "plus linking the shim"],
          mono=("SdkBuilder{}…Build()", "RegisterGlobally(*provider)"))
    g.arrow(540, 294, 540, 234)
    g.text(552, 272, "installs", OUTER, 13)

    g.arrow(850, 244, 850, 294)
    _card(g, 740, 300, 220, 110, BLUE, "microtel Provider", ["batch · retry", "upb encode · nghttp2"])
    g.arrow(850, 418, 850, 500)
    g.text(838, 466, "OTLP gRPC / HTTP", OUTER, 13, anchor="end")
    g.text(850, 528, "Collector or any OTLP backend", OUTER, 15, "bold", "middle")

    # What leaves the build
    g.box(30, 300, 300, 150, OUTER, GREY, width=1.8)
    g.text(180, 330, "no longer linked", INK, 17, "bold", "middle")
    for i, s in enumerate(("opentelemetry-cpp SDK + OTLP exporters", "gRPC · protobuf · abseil · libcurl")):
        g.text(180, 360 + 22 * i, s, NOTE, 14, anchor="middle")
    g.line(56, 390, 304, 346, STRIKE, 2.2, passes=1)
    g.line(56, 346, 304, 390, STRIKE, 2.2, passes=1)
    g.text(180, 428, "(rule 13, checked by CI)", NOTE, 13, anchor="middle", style="italic")
    g.text(30, 486, "Every target that includes an opentelemetry/ header links microtel_otelcpp_shim,",
           OUTER, 13, style="italic")
    g.text(30, 504, "so it builds with the same OPENTELEMETRY_STL_VERSION=2020 and ABI v1.",
           OUTER, 13, style="italic")
    return g.svg(990, 560, "Using microtel through the opentelemetry-cpp shim: existing call sites keep "
                 "calling the header-only opentelemetry-cpp API; startup.cpp builds a microtel Provider and "
                 "calls RegisterGlobally, so, within the supported subset of the API, the API's global "
                 "providers resolve to the microtel shim, which exports through microtel over OTLP. The "
                 "opentelemetry-cpp SDK, its exporters, gRPC, protobuf, abseil and libcurl are no longer linked.")
