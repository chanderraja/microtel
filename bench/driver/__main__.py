# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Bench driver entry point — python3 -m driver [options]."""

from __future__ import annotations

import argparse
import dataclasses
import sys
import time
from pathlib import Path

from .container import (
    Container,
    _image_id,
    build_image,
    create_network,
    detect_engine,
    remove_network,
    wait_http,
    wait_tcp,
)
from .control import ControlClient
from .env_fingerprint import capture as capture_env
from .env_fingerprint import warnings as env_warnings
from .profile import load as load_profile
from .registry import filter_b0
from .registry import get as get_sut
from .registry import load as load_registry
from .report import build_results, write_json, write_markdown
from .sink_client import SinkClient

_BENCH_DIR = Path(__file__).parent.parent
_PROFILES_DIR = _BENCH_DIR / "profiles"
_REGISTRY_PATH = _BENCH_DIR / "sut" / "registry.yaml"
_SINK_BUILD_CONTEXT = _BENCH_DIR / "sink" / "blackhole"
_SINK_IMAGE = "bench-sink-blackhole"
_NET_NAME = "bench-net"


def _parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="python3 -m driver",
        description="microtel benchmark driver",
    )
    p.add_argument("--profile", default="hot-loop-traces",
                   help="Profile name (default: hot-loop-traces)")
    p.add_argument("--sut", default=None,
                   help="Run only this SUT (default: all B0 SUTs in profile)")
    p.add_argument("--reps", type=int, default=None,
                   help="Override number of samples (default: from profile)")
    p.add_argument("--out", default=str(_BENCH_DIR / "results"),
                   help="Output directory (default: bench/results)")
    p.add_argument("--no-build", action="store_true",
                   help="Skip image builds (use cached images)")
    p.add_argument("--engine", default="auto",
                   choices=("auto", "podman", "docker"),
                   help="Container engine (default: auto)")
    p.add_argument("--seed", type=int, default=0,
                   help="Random seed placeholder (unused in B0)")
    p.add_argument("--allow-smt", action="store_true",
                   help="Suppress hyperthreading/SMT warning")
    p.add_argument("--verbose", action="store_true",
                   help="Show container build and run output")
    return p.parse_args(argv)


def _log(msg: str) -> None:
    print(f"[driver] {msg}", file=sys.stderr, flush=True)


def _otlp_endpoint(protocol: str) -> str:
    if protocol == "grpc":
        return "http://sink:4317"
    return "http://sink:4318"


def _run_sut(
    engine: str,
    sut,
    sink_client: SinkClient,
    n_samples: int,
    warmup_spans: int,
    spans_per_sample: int,
    verbose: bool,
) -> list[dict]:
    """Start a SUT container, run warmup + N timed samples, return samples."""
    samples = []
    with Container(engine, f"bench-sut-{sut.name}", verbose=verbose) as c:
        env = dict(sut.env)
        env["OTEL_EXPORTER_OTLP_ENDPOINT"] = _otlp_endpoint(sut.protocol)
        c.start(
            image=sut.image_name,
            ports={sut.ports.control: sut.ports.control},
            env=env,
            network=_NET_NAME,
        )
        _log(f"  waiting for {sut.name} control port ...")
        wait_tcp("127.0.0.1", sut.ports.control, timeout=30.0)

        with ControlClient("127.0.0.1", sut.ports.control) as ctrl:
            _log(f"  warmup ({warmup_spans} spans) ...")
            ctrl.run(warmup_spans)
            time.sleep(0.5)
            sink_client.reset()

            for i in range(n_samples):
                sink_client.reset()
                result = ctrl.run(spans_per_sample)
                time.sleep(0.5)
                sink_snap = sink_client.stats()
                _log(
                    f"  sample {i + 1}/{n_samples}: "
                    f"emitted={result['spans_emitted']} "
                    f"p50={result['latency_p50_ns']}ns"
                )
                samples.append({
                    "spans_emitted":  result["spans_emitted"],
                    "spans_dropped":  result["spans_dropped"],
                    "bytes_sent":     result.get("bytes_sent", 0),
                    "duration_ns":    result.get("duration_ns", 0),
                    "latency_p50_ns": result["latency_p50_ns"],
                    "latency_p95_ns": result["latency_p95_ns"],
                    "latency_p99_ns": result["latency_p99_ns"],
                    "latency_min_ns": result["latency_min_ns"],
                    "latency_max_ns": result["latency_max_ns"],
                    "sink": {
                        "spans_received": sink_snap["spans_received"],
                        "bytes_received": sink_snap["bytes_received"],
                    },
                })

            ctrl.quit()

    return samples


def main(argv=None) -> int:
    args = _parse_args(argv)
    out_dir = Path(args.out)
    repo_root = _BENCH_DIR.parent

    profile = load_profile(_PROFILES_DIR, args.profile)
    all_suts = load_registry(_REGISTRY_PATH)
    b0_suts = filter_b0(all_suts)

    if args.sut:
        sut = get_sut(all_suts, args.sut)
        if sut is None:
            _log(f"ERROR: SUT {args.sut!r} not found in registry")
            return 1
        active_suts = [sut]
    else:
        active_suts = [s for s in b0_suts if s.name in profile.suts]

    if not active_suts:
        _log("ERROR: no SUTs to run (check profile.suts and registry b0 flags)")
        return 1

    n_samples = args.reps if args.reps is not None else profile.samples
    engine = detect_engine(args.engine)

    _log(f"engine: {engine}")
    _log(
        f"profile: {profile.name} — "
        f"{profile.spans_per_sample} spans/sample x {n_samples} samples"
    )
    _log(f"SUTs: {[s.name for s in active_suts]}")

    fp = capture_env(engine)
    warns = env_warnings(fp, args.allow_smt)
    for w in warns:
        _log(f"WARNING: {w}")

    sut_image_ids: dict[str, str] = {}
    if args.no_build:
        _log("--no-build: skipping image builds")
        sink_image_id = _image_id(engine, _SINK_IMAGE)
        for sut in active_suts:
            sut_image_ids[sut.name] = _image_id(engine, sut.image_name)
    else:
        _log("building blackhole-sink image ...")
        sink_image_id = build_image(
            engine,
            dockerfile=str(_SINK_BUILD_CONTEXT / "Dockerfile"),
            image_name=_SINK_IMAGE,
            build_context=_SINK_BUILD_CONTEXT,
            verbose=args.verbose,
        )
        for sut in active_suts:
            _log(f"building {sut.name} image ...")
            sut_image_ids[sut.name] = build_image(
                engine,
                dockerfile=str(repo_root / sut.dockerfile),
                image_name=sut.image_name,
                build_context=repo_root,
                verbose=args.verbose,
            )

    create_network(engine, _NET_NAME)
    sut_results = []
    try:
        _log("starting blackhole-sink ...")
        with Container(engine, "bench-sink", verbose=args.verbose) as sink_c:
            sink_c.start(
                image=_SINK_IMAGE,
                ports={8080: 8080},
                network=_NET_NAME,
                network_alias="sink",
            )
            _log("waiting for sink /health ...")
            wait_http("http://127.0.0.1:8080/health", timeout=30.0)
            sink_client = SinkClient("127.0.0.1", 8080)

            for sut in active_suts:
                _log(f"--- SUT: {sut.name} ---")
                try:
                    samples = _run_sut(
                        engine=engine,
                        sut=sut,
                        sink_client=sink_client,
                        n_samples=n_samples,
                        warmup_spans=profile.warmup_spans,
                        spans_per_sample=profile.spans_per_sample,
                        verbose=args.verbose,
                    )
                except Exception as exc:
                    _log(f"ERROR running {sut.name}: {exc}")
                    samples = []

                sut_results.append({
                    "name":                sut.name,
                    "library":             sut.library,
                    "transport":           sut.transport,
                    "library_version":     "",
                    "library_build_flags": "",
                    "image_tag":           sut.image_name,
                    "image_id":            sut_image_ids.get(sut.name, ""),
                    "samples":             samples,
                })
    finally:
        remove_network(engine, _NET_NAME)

    env_data = dataclasses.asdict(fp)
    profile_data = dataclasses.asdict(profile)
    doc = build_results(profile_data, env_data, sut_results, warns)

    json_path = write_json(doc, out_dir)
    md_path = write_markdown(doc, out_dir)
    _log(f"results written to {json_path}")
    _log(f"report written to {md_path}")

    for sr in doc["suts"]:
        summary = sr.get("summary", {})
        p50 = summary.get("latency_p50_ns", {})
        _log(
            f"{sr['name']}: p50={p50.get('median', 0):.0f}ns  "
            f"drop_rate={summary.get('drop_rate_pct', 0)}%  "
            f"samples={summary.get('reps', 0)}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
