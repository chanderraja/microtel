# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Bench driver entry point — python3 -m driver [options]."""

from __future__ import annotations

import argparse
import dataclasses
import sys
import time
from pathlib import Path
from typing import Any, Optional, Union

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
from .flamegraph import (
    FlameGraphRecorder,
    check_prerequisites,
    find_flamegraph_dir,
    perf_image_name,
)
from .profile import load as load_profile
from .registry import filter_b0
from .registry import get as get_sut
from .registry import load as load_registry
from .plots import write_plots
from .regression import check as check_regression
from .regression import format_report as format_regression_report
from .report import build_results, write_json, write_markdown
from .sink_client import CollectorSinkClient, SinkClient

_BENCH_DIR = Path(__file__).parent.parent
_PROFILES_DIR = _BENCH_DIR / "profiles"
_REGISTRY_PATH = _BENCH_DIR / "sut" / "registry.yaml"
_BLACKHOLE_BUILD_CONTEXT = _BENCH_DIR / "sink" / "blackhole"
_BLACKHOLE_IMAGE = "bench-sink-blackhole"
_COLLECTOR_BUILD_CONTEXT = _BENCH_DIR / "sink" / "collector"
_COLLECTOR_IMAGE = "bench-sink-collector"
_NET_NAME = "bench-net"

AnySinkClient = Union[SinkClient, CollectorSinkClient]


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
    p.add_argument("--sink", default="auto",
                   choices=("auto", "blackhole", "collector"),
                   help="Sink mode: auto uses profile default (default: auto)")
    p.add_argument("--regression-check", default=None, metavar="BASELINE",
                   help="Path to baseline results.json; exit 2 on regression")
    p.add_argument("--threshold", type=float, default=0.05,
                   help="Regression threshold as a fraction (default: 0.05 = 5%%)")
    p.add_argument("--allow-smt", action="store_true",
                   help="Suppress hyperthreading/SMT warning")
    p.add_argument("--verbose", action="store_true",
                   help="Show container build and run output")
    p.add_argument("--flamegraph", action="store_true",
                   help=(
                       "Generate SVG flamegraphs per SUT (B3). Builds :perf image "
                       "variants with linux-perf installed; runs one dedicated "
                       "profiling sample after measurements using --cap-add=PERFMON. "
                       "Requires perl on PATH and FlameGraph scripts "
                       "(set FLAMEGRAPH_DIR or pass --flamegraph-dir). "
                       "Normal benchmark runs never need this flag."
                   ))
    p.add_argument("--flamegraph-dir", default=None, metavar="DIR",
                   help="Path to FlameGraph scripts directory (overrides FLAMEGRAPH_DIR "
                        "env var and default search paths)")
    return p.parse_args(argv)


def _log(msg: str) -> None:
    print(f"[driver] {msg}", file=sys.stderr, flush=True)


def _binary_bytes(engine: str, image_name: str) -> Optional[int]:
    """Return the byte size of /emit_app inside the SUT image, or None on error."""
    import subprocess
    try:
        proc = subprocess.run(
            [engine, "run", "--rm", "--entrypoint", "/bin/sh",
             image_name, "-c", "wc -c < /emit_app"],
            capture_output=True, text=True, timeout=30,
        )
        if proc.returncode == 0:
            return int(proc.stdout.strip())
    except (subprocess.TimeoutExpired, ValueError, OSError):
        pass
    return None


def _otlp_endpoint(protocol: str) -> str:
    if protocol == "grpc":
        return "http://sink:4317"
    return "http://sink:4318"


def _build_sink_image(engine: str, sink_mode: str, verbose: bool) -> None:
    if sink_mode == "blackhole":
        _log("building blackhole-sink image ...")
        build_image(
            engine,
            dockerfile=str(_BLACKHOLE_BUILD_CONTEXT / "Dockerfile"),
            image_name=_BLACKHOLE_IMAGE,
            build_context=_BLACKHOLE_BUILD_CONTEXT,
            verbose=verbose,
        )
    else:
        _log("building collector-sink image ...")
        build_image(
            engine,
            dockerfile=str(_COLLECTOR_BUILD_CONTEXT / "Dockerfile"),
            image_name=_COLLECTOR_IMAGE,
            build_context=_COLLECTOR_BUILD_CONTEXT,
            verbose=verbose,
        )


def _build_images(
    engine: str,
    sink_mode: str,
    active_suts: list,
    repo_root: Path,
    no_build: bool,
    verbose: bool,
    flamegraph: bool = False,
) -> dict[str, str]:
    """Build (or look up) all container images. Returns sut_name -> image_id.

    When flamegraph=True, additionally builds a :perf variant of each SUT image
    (with linux-perf installed) tagged as <image_name>:perf.  Normal images are
    also built so that measurement samples run against the unmodified binary.
    """
    sut_image_ids: dict[str, str] = {}
    if no_build:
        _log("--no-build: skipping image builds")
        for sut in active_suts:
            tag = perf_image_name(sut.image_name) if flamegraph else sut.image_name
            sut_image_ids[sut.name] = _image_id(engine, tag)
        return sut_image_ids

    _build_sink_image(engine, sink_mode, verbose)
    for sut in active_suts:
        _log(f"building {sut.name} image ...")
        sut_image_ids[sut.name] = build_image(
            engine,
            dockerfile=str(repo_root / sut.dockerfile),
            image_name=sut.image_name,
            build_context=repo_root,
            verbose=verbose,
        )
        if flamegraph:
            _log(f"building {sut.name}:perf image (INSTALL_PERF=true) ...")
            build_image(
                engine,
                dockerfile=str(repo_root / sut.dockerfile),
                image_name=perf_image_name(sut.image_name),
                build_context=repo_root,
                build_args={"INSTALL_PERF": "true"},
                verbose=verbose,
            )
    return sut_image_ids


def _start_sink(engine: str, sink_mode: str, verbose: bool) -> tuple[Any, AnySinkClient]:
    """Start the sink container and return (Container context, SinkClient)."""
    sink_c = Container(engine, "bench-sink", verbose=verbose)
    if sink_mode == "blackhole":
        sink_c.start(
            image=_BLACKHOLE_IMAGE,
            ports={8080: 8080},
            network=_NET_NAME,
            network_alias="sink",
        )
        _log("waiting for blackhole-sink /health ...")
        wait_http("http://127.0.0.1:8080/health", timeout=30.0)
        return sink_c, SinkClient("127.0.0.1", 8080)

    sink_c.start(
        image=_COLLECTOR_IMAGE,
        ports={8888: 8888, 13133: 13133},
        network=_NET_NAME,
        network_alias="sink",
    )
    _log("waiting for collector-sink health check ...")
    wait_http("http://127.0.0.1:13133/health/status", timeout=30.0)
    return sink_c, CollectorSinkClient("127.0.0.1")


def _run_sut(
    engine: str,
    sut: Any,
    sink_client: AnySinkClient,
    n_samples: int,
    warmup_spans: int,
    spans_per_sample: int,
    profile_env: dict,
    threads: int,
    rate_hz: int,
    verbose: bool,
    flamegraph_dir: Optional[Path] = None,
    out_dir: Optional[Path] = None,
) -> tuple[list[dict], Optional[Path]]:
    """Start a SUT container, run warmup + N timed samples, return (samples, svg_path).

    When flamegraph_dir is provided the container is started with the :perf image
    and --cap-add=PERFMON.  After all measurement samples, one dedicated profiling
    sample is recorded with perf; the resulting SVG is written to out_dir.
    """
    samples: list[dict] = []
    svg_path: Optional[Path] = None

    image = perf_image_name(sut.image_name) if flamegraph_dir else sut.image_name
    cap_add = ["PERFMON"] if flamegraph_dir else None
    container_name = f"bench-sut-{sut.name}"

    with Container(engine, container_name, verbose=verbose) as c:
        env = {**sut.env, **profile_env}
        env["OTEL_EXPORTER_OTLP_ENDPOINT"] = _otlp_endpoint(sut.protocol)
        c.start(
            image=image,
            ports={sut.ports.control: sut.ports.control},
            env=env,
            network=_NET_NAME,
            cap_add=cap_add,
        )
        _log(f"  waiting for {sut.name} control port ...")
        wait_tcp("127.0.0.1", sut.ports.control, timeout=30.0)

        with ControlClient("127.0.0.1", sut.ports.control) as ctrl:
            _log(f"  warmup ({warmup_spans} spans) ...")
            ctrl.run(warmup_spans, threads=threads, rate_hz=rate_hz)
            time.sleep(0.5)
            sink_client.reset()

            for i in range(n_samples):
                sink_client.reset()
                result = ctrl.run(spans_per_sample, threads=threads, rate_hz=rate_hz)
                flush_result = ctrl.flush()
                time.sleep(0.5)
                sink_snap = sink_client.stats()
                sink_errors = sink_snap.get("errors", 0)
                sink_http_req = sink_snap.get("http_requests_received", 0)
                sink_grpc_req = sink_snap.get("grpc_requests_received", 0)
                _log(
                    f"  sample {i + 1}/{n_samples}: "
                    f"emitted={result['spans_emitted']} "
                    f"sink_received={sink_snap['spans_received']} "
                    f"(http={sink_http_req} grpc={sink_grpc_req}) "
                    f"p50={result['latency_p50_ns']}ns "
                    f"flush={flush_result.get('flush_ns', 0)}ns"
                )
                if sink_errors > 0:
                    _log(
                        f"  WARNING: sink errors={sink_errors} "
                        f"last_error={sink_snap.get('last_error', '')!r}"
                    )
                elif sink_snap["spans_received"] == 0 and sink_http_req == 0 and sink_grpc_req == 0:
                    _log(
                        "  WARNING: sink received 0 requests — "
                        "check endpoint URL, port, and container network"
                    )
                dur_ns = result.get("duration_ns", 0)
                bytes_rx = sink_snap.get("bytes_received")
                throughput_mbps = (
                    bytes_rx * 8000.0 / dur_ns
                    if (bytes_rx is not None and dur_ns > 0)
                    else None
                )
                samples.append({
                    "spans_emitted":  result["spans_emitted"],
                    "spans_dropped":  result["spans_dropped"],
                    "bytes_sent":     result.get("bytes_sent", 0),
                    "duration_ns":    dur_ns,
                    "throughput_mbps": throughput_mbps,
                    "latency_p50_ns":    result["latency_p50_ns"],
                    "latency_p95_ns":    result["latency_p95_ns"],
                    "latency_p99_ns":    result["latency_p99_ns"],
                    "latency_min_ns":    result["latency_min_ns"],
                    "latency_max_ns":    result["latency_max_ns"],
                    "latency_histogram": result.get("latency_histogram", []),
                    "flush_ns":          flush_result.get("flush_ns", 0),
                    "sink": {
                        "mode":                   sink_snap["mode"],
                        "spans_received":         sink_snap["spans_received"],
                        "bytes_received":         sink_snap["bytes_received"],
                        "http_requests_received": sink_http_req,
                        "grpc_requests_received": sink_grpc_req,
                        "errors":                 sink_errors,
                        "last_error":             sink_snap.get("last_error", ""),
                    },
                })

            if flamegraph_dir and out_dir is not None:
                _log(f"  flamegraph: recording dedicated profiling sample ...")
                recorder = FlameGraphRecorder(engine, container_name)
                recorder.start()
                ctrl.run(spans_per_sample, threads=threads, rate_hz=rate_hz)
                svg_path = recorder.finish(sut.name, out_dir, flamegraph_dir)
                if svg_path:
                    _log(f"  flamegraph: SVG written to {svg_path}")
                else:
                    _log(f"  flamegraph: SVG generation failed for {sut.name}")

            ctrl.quit()

    return samples, svg_path


def _collect_sut_results(
    engine: str,
    active_suts: list,
    sink_client: AnySinkClient,
    sut_image_ids: dict[str, str],
    n_samples: int,
    profile: Any,
    verbose: bool,
    flamegraph_dir: Optional[Path] = None,
    out_dir: Optional[Path] = None,
) -> list[dict]:
    sut_results = []
    for sut in active_suts:
        _log(f"--- SUT: {sut.name} ---")
        try:
            samples, svg_path = _run_sut(
                engine=engine,
                sut=sut,
                sink_client=sink_client,
                n_samples=n_samples,
                warmup_spans=profile.warmup_spans,
                spans_per_sample=profile.spans_per_sample,
                profile_env=profile.env,
                threads=profile.threads,
                rate_hz=profile.target_rate_hz,
                verbose=verbose,
                flamegraph_dir=flamegraph_dir,
                out_dir=out_dir,
            )
        except Exception as exc:
            _log(f"ERROR running {sut.name}: {exc}")
            samples, svg_path = [], None

        image_tag = (
            perf_image_name(sut.image_name) if flamegraph_dir else sut.image_name
        )
        binary_size = _binary_bytes(engine, image_tag)
        if binary_size is not None:
            _log(f"  binary size: {binary_size:,} bytes")
        sut_results.append({
            "name":                sut.name,
            "library":             sut.library,
            "transport":           sut.transport,
            "library_version":     "",
            "library_build_flags": "",
            "image_tag":           image_tag,
            "image_id":            sut_image_ids.get(sut.name, ""),
            "binary_bytes":        binary_size,
            "samples":             samples,
            "flamegraph_svg":      str(svg_path) if svg_path else None,
        })
    return sut_results


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
    sink_mode = args.sink if args.sink != "auto" else profile.sink_mode

    # Resolve flamegraph settings early so we can fail fast before building images.
    flamegraph_dir: Optional[Path] = None
    if args.flamegraph:
        flamegraph_dir = (
            Path(args.flamegraph_dir) if args.flamegraph_dir else find_flamegraph_dir()
        )
        problems = check_prerequisites(flamegraph_dir)
        if problems:
            for p in problems:
                _log(f"ERROR (--flamegraph): {p}")
            return 1
        _log(f"flamegraph: scripts at {flamegraph_dir}")
        _log("flamegraph: NOTE — measurement samples run inside :perf image with "
             "--cap-add=PERFMON; a separate profiling sample is recorded afterward")

    _log(f"engine: {engine}")
    _log(
        f"profile: {profile.name} — "
        f"{profile.spans_per_sample} spans/sample x {n_samples} samples"
    )
    _log(f"SUTs: {[s.name for s in active_suts]}")
    _log(f"sink: {sink_mode}")

    fp = capture_env(engine)
    warns = env_warnings(fp, args.allow_smt)
    for w in warns:
        _log(f"WARNING: {w}")

    sut_image_ids = _build_images(
        engine, sink_mode, active_suts, repo_root,
        args.no_build, args.verbose, flamegraph=args.flamegraph,
    )

    create_network(engine, _NET_NAME)
    sut_results = []
    try:
        sink_c, sink_client = _start_sink(engine, sink_mode, args.verbose)
        with sink_c:
            sut_results = _collect_sut_results(
                engine, active_suts, sink_client, sut_image_ids,
                n_samples, profile, args.verbose,
                flamegraph_dir=flamegraph_dir,
                out_dir=out_dir,
            )
    finally:
        remove_network(engine, _NET_NAME)

    if sink_mode == "collector":
        warns = list(warns) + [
            "Sink mode 'collector' has higher span-count variance than 'blackhole'. "
            "Wire-bytes metrics are unavailable in collector mode."
        ]

    env_data = dataclasses.asdict(fp)
    profile_data = dataclasses.asdict(profile)
    doc = build_results(profile_data, env_data, sut_results, warns)

    json_path = write_json(doc, out_dir)
    md_path = write_markdown(doc, out_dir)
    plots_path = write_plots(doc, out_dir)
    _log(f"results written to {json_path}")
    _log(f"report written to {md_path}")
    if plots_path:
        _log(f"plots written to {plots_path}")

    for sr in doc["suts"]:
        summary = sr.get("summary", {})
        p50 = summary.get("latency_p50_ns", {})
        _log(
            f"{sr['name']}: p50={p50.get('median', 0):.0f}ns  "
            f"drop_rate={summary.get('drop_rate_pct', 0)}%  "
            f"samples={summary.get('reps', 0)}"
        )

    if args.regression_check:
        return _run_regression_check(args.regression_check, doc, args.threshold)

    return 0


def _run_regression_check(baseline_path: str, current: dict, threshold: float) -> int:
    import json
    from pathlib import Path as _Path

    try:
        baseline = json.loads(_Path(baseline_path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        _log(f"ERROR: could not load baseline {baseline_path!r}: {exc}")
        return 1

    regressions = check_regression(baseline, current, threshold)
    report = format_regression_report(regressions)
    for line in report.splitlines():
        _log(line)

    if regressions:
        _log(f"FAIL: {len(regressions)} regression(s) detected "
             f"(threshold {threshold * 100:.0f}%)")
        return 2

    _log(f"PASS: no regressions vs {baseline_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
