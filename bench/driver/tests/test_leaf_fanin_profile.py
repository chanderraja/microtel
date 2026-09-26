# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""The leaf-fanin profile: the concentrator throughput bench of
docs/leaf-concentrator-design.md §7.9. One iteration ingests one leaf payload
of EMIT_LEAF_SPANS_PER_PAYLOAD spans; --sweep-leaves runs one virtual SUT per
leaf count."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.__main__ import _expand_sweep_leaves, _export_requests, _parse_args, _spans_per_iteration
from driver.profile import load
from driver.registry import load as load_registry

BENCH_DIR = Path(__file__).parent.parent.parent
PROFILES_DIR = BENCH_DIR / "profiles"
REPO_ROOT = BENCH_DIR.parent


def test_load_leaf_fanin():
    p = load(PROFILES_DIR, "leaf-fanin")
    assert p.name == "leaf-fanin"
    assert p.env["EMIT_WORKLOAD"] == "leaf_fanin"
    assert p.sink_mode == "blackhole"
    assert p.suts == ["microtel-concentrator"]


def test_registry_has_the_concentrator_sut():
    suts = {s.name: s for s in load_registry(BENCH_DIR / "sut" / "registry.yaml")}
    sut = suts["microtel-concentrator"]
    assert sut.protocol == "grpc"
    assert (REPO_ROOT / sut.dockerfile).is_file()


def test_spans_per_iteration_is_the_payload_size():
    env = {"EMIT_WORKLOAD": "leaf_fanin", "EMIT_LEAF_SPANS_PER_PAYLOAD": "25"}
    assert _spans_per_iteration(env) == 25


def test_spans_per_iteration_leaf_default():
    assert _spans_per_iteration({"EMIT_WORKLOAD": "leaf_fanin"}) == 10


def test_spans_per_iteration_other_workloads_unchanged():
    assert _spans_per_iteration({"EMIT_WORKLOAD": "realistic_request"}) == 3
    assert _spans_per_iteration({"EMIT_WORKLOAD": "hot_loop"}) == 1


def test_sweep_leaves_expands_one_sut_per_count():
    sut = load_registry(BENCH_DIR / "sut" / "registry.yaml")
    base = [s for s in sut if s.name == "microtel-concentrator"]
    ids = {"microtel-concentrator": "sha256:abc"}
    expanded = _expand_sweep_leaves(base, "1,10,100", ids)
    assert [s.name for s in expanded] == [
        "microtel-concentrator-1l",
        "microtel-concentrator-10l",
        "microtel-concentrator-100l",
    ]
    assert [s.env["EMIT_LEAVES"] for s in expanded] == ["1", "10", "100"]
    assert ids["microtel-concentrator-100l"] == "sha256:abc"


def test_sweep_leaves_rejects_non_integers():
    base = load_registry(BENCH_DIR / "sut" / "registry.yaml")[:1]
    assert _expand_sweep_leaves(base, "1,many", {}) is None


def test_sweep_leaves_flag_parses():
    assert _parse_args(["--sweep-leaves", "1,10"]).sweep_leaves == "1,10"


def test_export_requests_sums_both_protocols():
    assert _export_requests({"http_requests_received": 2, "grpc_requests_received": 3}) == 5
    assert _export_requests({}) == 0
