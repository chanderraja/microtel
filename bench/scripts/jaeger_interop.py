#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Standalone Jaeger OTLP interop test.

Starts Jaeger all-in-one, runs microtel (HTTP) and microtel-grpc (gRPC) SUTs
against it, and verifies that user service names appear in the Jaeger query API.

Usage (from repo root):
    python3 bench/scripts/jaeger_interop.py [--engine docker|podman]

Exit codes:
    0   PASS — Jaeger received spans from both SUTs
    1   FAIL — spans not received or setup error
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

_BENCH_DIR = Path(__file__).parent.parent
_REPO_ROOT = _BENCH_DIR.parent
_NET = "jaeger-interop-net"
_JAEGER_CONTAINER = "jaeger-interop"
_JAEGER_IMAGE = "jaegertracing/all-in-one:latest"
_EMIT_SPANS = 5000

_SUTS = [
    {
        "name": "microtel",
        "dockerfile": "bench/sut/microtel/Dockerfile",
        "image": "bench-sut-microtel",
        "protocol": "http",
        "endpoint": "http://jaeger:4318",
        "control_port": 19090,
    },
    {
        "name": "microtel-grpc",
        "dockerfile": "bench/sut/microtel-grpc/Dockerfile",
        "image": "bench-sut-microtel-grpc",
        "protocol": "grpc",
        "endpoint": "http://jaeger:4317",
        "control_port": 19090,
    },
]


def _log(msg: str) -> None:
    print(f"[jaeger-interop] {msg}", flush=True)


def _run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def _detect_engine(preference: str) -> str:
    if preference in ("docker", "podman"):
        if shutil.which(preference):
            return preference
        raise RuntimeError(f"{preference} not found on PATH")
    for e in ("podman", "docker"):
        if shutil.which(e):
            return e
    raise RuntimeError("neither podman nor docker found on PATH")


def _wait_http(url: str, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=3) as resp:
                if resp.status == 200:
                    return
        except Exception:
            pass
        time.sleep(1.0)
    raise RuntimeError(f"timed out waiting for {url}")


def _wait_tcp(host: str, port: int, timeout: float = 30.0) -> None:
    import socket
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2.0):
                return
        except OSError:
            pass
        time.sleep(1.0)
    raise RuntimeError(f"timed out waiting for {host}:{port}")


def _create_network(engine: str) -> None:
    result = _run([engine, "network", "create", _NET], check=False)
    if result.returncode != 0 and "already exists" not in result.stderr:
        raise RuntimeError(f"network create failed: {result.stderr.strip()}")


def _remove_network(engine: str) -> None:
    _run([engine, "network", "rm", _NET], check=False)


def _start_jaeger(engine: str) -> None:
    _log("starting Jaeger all-in-one ...")
    _run([
        engine, "run", "-d",
        "--name", _JAEGER_CONTAINER,
        "--network", _NET,
        "--network-alias", "jaeger",
        "-p", "16686:16686",
        "-e", "COLLECTOR_OTLP_ENABLED=true",
        _JAEGER_IMAGE,
    ])
    _log("waiting for Jaeger query API ...")
    _wait_http("http://127.0.0.1:16686/api/services")
    _log("Jaeger ready.")


def _stop_jaeger(engine: str) -> None:
    _run([engine, "rm", "-f", _JAEGER_CONTAINER], check=False)


def _build_sut_image(engine: str, sut: dict) -> None:
    _log(f"building {sut['name']} ...")
    _run([
        engine, "build",
        "-f", str(_REPO_ROOT / sut["dockerfile"]),
        "-t", sut["image"],
        str(_REPO_ROOT),
    ])


def _run_sut(engine: str, sut: dict) -> None:
    """Start a SUT container, emit spans, flush, then stop it."""
    from bench.driver.control import ControlClient

    container = f"jaeger-interop-sut-{sut['name']}"
    _log(f"  starting {sut['name']} ...")
    _run([
        engine, "run", "-d",
        "--name", container,
        "--network", _NET,
        "-p", f"{sut['control_port']}:{sut['control_port']}",
        "-e", f"OTEL_EXPORTER_OTLP_ENDPOINT={sut['endpoint']}",
        sut["image"],
    ])
    try:
        _log(f"  waiting for {sut['name']} control port ...")
        _wait_tcp("127.0.0.1", sut["control_port"])

        with ControlClient("127.0.0.1", sut["control_port"]) as ctrl:
            _log(f"  emitting {_EMIT_SPANS} spans ...")
            result = ctrl.run(_EMIT_SPANS, threads=1)
            _log(f"  emitted={result['spans_emitted']} dropped={result['spans_dropped']}")
            _log("  flushing ...")
            ctrl.flush()
            time.sleep(1.0)
            ctrl.quit()
    finally:
        _run([engine, "rm", "-f", container], check=False)


def _verify_jaeger() -> bool:
    """Check that at least one non-jaeger-internal service appears."""
    time.sleep(2)  # give batch processor a moment to flush
    url = "http://127.0.0.1:16686/api/services"
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read().decode())
    except Exception as exc:
        _log(f"FAIL: could not query Jaeger services API: {exc}")
        return False

    services = data.get("data", [])
    user_services = [s for s in services if s != "jaeger-query"]
    if not user_services:
        _log(f"FAIL: no user services in Jaeger — got: {services}")
        return False

    _log(f"PASS: Jaeger has user services: {user_services}")
    return True


def main() -> int:
    p = argparse.ArgumentParser(description="Jaeger OTLP interop test")
    p.add_argument("--engine", default="auto", choices=("auto", "docker", "podman"))
    args = p.parse_args()

    engine = _detect_engine(args.engine)
    _log(f"engine: {engine}")

    # Make driver importable when running from repo root.
    sys.path.insert(0, str(_BENCH_DIR))

    _create_network(engine)
    try:
        _start_jaeger(engine)
        try:
            for sut in _SUTS:
                _build_sut_image(engine, sut)
                _run_sut(engine, sut)

            return 0 if _verify_jaeger() else 1
        finally:
            _stop_jaeger(engine)
    finally:
        _remove_network(engine)


if __name__ == "__main__":
    sys.exit(main())
