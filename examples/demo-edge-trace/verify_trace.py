#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Assert that Tempo holds one trace spanning the gateway and the device.

Finds the most recent trace containing a span from service "mcu-sensor",
fetches it, and checks that:
  - it has spans from both "edge-gateway" and "mcu-sensor";
  - the device's sensor.read is a child of the gateway's device.read_sensor
    (the traceparent crossed the UART), and adc.sample a child of sensor.read.

Usage: verify_trace.py [tempo-url] [timeout-seconds]   (default http://localhost:3200 120)
Exit 0 on success, 1 otherwise. Prints the trace as a tree.
"""
from __future__ import annotations

import base64
import json
import sys
import time
import urllib.parse
import urllib.request

TEMPO = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:3200"
TIMEOUT = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0


def get(path: str) -> dict:
    with urllib.request.urlopen(TEMPO + path, timeout=10) as resp:
        return json.loads(resp.read().decode())


def hexid(v: str) -> str:
    """Tempo returns ids base64-encoded in OTLP JSON; normalise to hex."""
    if not v:
        return ""
    try:
        return base64.b64decode(v).hex()
    except Exception:
        return v.lower()


def spans_of(trace: dict) -> list[dict]:
    out = []
    for batch in trace.get("batches", trace.get("resourceSpans", [])):
        attrs = batch.get("resource", {}).get("attributes", [])
        svc = next((a["value"].get("stringValue") for a in attrs if a["key"] == "service.name"), "?")
        for ss in batch.get("scopeSpans", batch.get("instrumentationLibrarySpans", [])):
            for s in ss.get("spans", []):
                out.append({"service": svc, "name": s["name"], "id": hexid(s["spanId"]),
                            "parent": hexid(s.get("parentSpanId", ""))})
    return out


def check_once() -> tuple[bool, str]:
    q = urllib.parse.quote('{ resource.service.name = "mcu-sensor" }')
    found = get(f"/api/search?q={q}&limit=20").get("traces", [])
    if not found:
        return False, "no trace with service mcu-sensor yet"
    found.sort(key=lambda t: int(t.get("startTimeUnixNano", "0")), reverse=True)
    for t in found:
        spans = spans_of(get(f"/api/traces/{t['traceID']}"))
        services = {s["service"] for s in spans}
        by_name = {s["name"]: s for s in spans}
        call, read, sample = (by_name.get(n) for n in ("device.read_sensor", "sensor.read", "adc.sample"))
        if not ({"edge-gateway", "mcu-sensor"} <= services and call and read and sample):
            continue
        if read["parent"] != call["id"] or sample["parent"] != read["id"]:
            return False, f"trace {t['traceID']}: parent links wrong: {spans}"
        tree = "\n".join(f"  {s['service']:<13} {s['name']:<30} span={s['id']} parent={s['parent'] or '-'}"
                         for s in spans)
        return True, f"trace {t['traceID']} spans both services:\n{tree}"
    return False, "no trace yet with spans from both edge-gateway and mcu-sensor"


def main() -> int:
    deadline = time.monotonic() + TIMEOUT
    msg = ""
    while time.monotonic() < deadline:
        try:
            ok, msg = check_once()
        except Exception as exc:  # Tempo not ready yet
            ok, msg = False, f"tempo: {exc}"
        if ok:
            print("PASS: " + msg)
            return 0
        time.sleep(3)
    print("FAIL: " + msg)
    return 1


if __name__ == "__main__":
    sys.exit(main())
