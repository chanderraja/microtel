"""README: devices running microtel-leaf -> concentrator gateway -> backend."""
from sketch import ACCENT, AMBER, BLUE, GREEN, INK, NOTE, OUTER, WHITE, Rough

OUTPUT = "leaf-concentrator.svg"


def render():
    g = Rough(7)

    # Devices: a stack of cards, x N
    for off in (16, 8):
        g.box(30 + off, 70 - off, 250, 180, OUTER, AMBER, width=1.4)
    g.box(30, 70, 250, 180, OUTER, AMBER, width=2.2)
    g.text(155, 100, "Device firmware", INK, 19, "bold", "middle")
    g.text(155, 132, "microtel-leaf (C11)", ACCENT, 17, "bold", "middle")
    g.text(155, 160, "no heap · no threads · no I/O", INK, 15, anchor="middle")
    g.text(155, 186, "builds spans, encodes", INK, 15, anchor="middle")
    g.text(155, 208, "OTLP payload bytes", INK, 15, anchor="middle")
    g.text(155, 235, "~9.3 KB flash on Cortex-M4", NOTE, 13, anchor="middle", style="italic")
    g.text(155, 285, "× N devices", OUTER, 17, "bold", "middle")
    g.text(155, 307, "Cortex-M, small Linux boards", OUTER, 14, anchor="middle")

    # Link
    g.harrow(292, 160, 392, 160, OUTER, 2.6)
    g.text(342, 142, "your link", OUTER, 15, "bold", "middle")
    g.text(342, 188, "UART · CAN", OUTER, 14, anchor="middle")
    g.text(342, 207, "BLE · UDP", OUTER, 14, anchor="middle")
    g.text(342, 226, "MQTT …", OUTER, 14, anchor="middle")

    # Gateway
    g.box(402, 40, 280, 240, OUTER, BLUE, width=2.2)
    g.text(542, 70, "Gateway (Linux)", INK, 19, "bold", "middle")
    g.box(422, 86, 240, 62, INK, WHITE, width=1.6)
    g.text(542, 110, "your receive loop", INK, 15, anchor="middle")
    g.text(542, 136, "→ LeafReceiver::Ingest", ACCENT, 16, "bold", "middle")
    g.text(542, 178, "microtel Provider", INK, 17, "bold", "middle")
    g.text(542, 204, "per-device Resource", INK, 15, anchor="middle")
    g.text(542, 226, "clock correction", INK, 15, anchor="middle")
    g.text(542, 248, "sample · batch · retry · export", INK, 15, anchor="middle")
    g.text(542, 307, "microtel opens no socket: the link is yours", OUTER, 14, anchor="middle", style="italic")

    # Export
    g.harrow(694, 160, 774, 160, OUTER, 2.6)
    g.text(734, 142, "OTLP", OUTER, 15, "bold", "middle")
    g.text(734, 188, "gRPC / HTTP", OUTER, 14, anchor="middle")

    # Backend
    g.box(786, 95, 150, 130, OUTER, GREEN, width=2.2)
    g.text(861, 130, "Backend", INK, 19, "bold", "middle")
    g.text(861, 162, "OTel Collector", INK, 15, anchor="middle")
    g.text(861, 186, "Tempo, Jaeger,", INK, 15, anchor="middle")
    g.text(861, 208, "…", INK, 15, anchor="middle")

    return g.svg(960, 340, "Devices running microtel-leaf send OTLP payload bytes over their own link "
                 "to a gateway running a microtel Provider with LeafReceiver, which exports to an "
                 "OpenTelemetry backend")
