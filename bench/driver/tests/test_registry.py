# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.registry import load, filter_b0, get

REGISTRY = Path(__file__).parent.parent.parent / "sut" / "registry.yaml"


def test_load_registry():
    suts = load(REGISTRY)
    names = [s.name for s in suts]
    assert "microtel" in names
    assert "otelcpp-grpc" in names


def test_b0_filter():
    suts = load(REGISTRY)
    b0 = filter_b0(suts)
    names = [s.name for s in b0]
    assert "microtel" in names
    assert "otelcpp-grpc" in names
    assert "otelcpp-http" not in names


def test_library_transport_derived():
    suts = load(REGISTRY)
    microtel = get(suts, "microtel")
    assert microtel is not None
    assert microtel.library == "microtel"
    assert microtel.transport == "http"

    otelcpp = get(suts, "otelcpp-grpc")
    assert otelcpp is not None
    assert otelcpp.library == "otelcpp"
    assert otelcpp.transport == "grpc"


def test_get_missing_returns_none():
    suts = load(REGISTRY)
    assert get(suts, "nonexistent") is None


def test_control_port():
    suts = load(REGISTRY)
    for s in suts:
        assert s.ports.control == 9090
