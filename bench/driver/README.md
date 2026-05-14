# bench/driver — Python benchmark orchestrator

Runs one or more SUT containers against the blackhole sink, collects
`RunResult` samples over the TCP control socket, and writes
`results.json` + `results.md` to the output directory.

## Invocation

```bash
# From the repo root via the shell wrapper:
./bench/bench.sh [options]

# Or directly from bench/:
cd bench
python3 -m driver [options]
```

## Options

```
--profile NAME    Workload profile (default: hot-loop-traces)
--sut NAME        Run only this SUT; default: all B0 SUTs in the profile
--reps N          Override sample count from the profile
--out DIR         Output directory (default: bench/results)
--no-build        Skip image builds; use cached images
--engine ENGINE   Container engine: auto|podman|docker (default: auto)
--allow-smt       Suppress the hyperthreading/SMT env-guard warning
--verbose         Show container build and run output
```

## Module layout

| File | Purpose |
|------|---------|
| `__main__.py` | CLI, image builds, container lifecycle, run loop |
| `profile.py` | Load `bench/profiles/*.yaml`; stdlib-only YAML parser |
| `registry.py` | Load `bench/sut/registry.yaml`; `Sut` dataclass |
| `env_fingerprint.py` | CPU/governor/HT snapshot for `results.json` |
| `container.py` | `build_image`, `Container`, `wait_tcp`, `wait_http` |
| `control.py` | TCP ndjson client for the emit-app control socket |
| `sink_client.py` | HTTP client for the blackhole-sink `/stats` and `/reset` |
| `report.py` | Statistics, `build_results()`, `write_json()`, `write_markdown()` |

## Output

- `results.json` — full structured results (schema version 1.0)
- `results.md` — human-readable summary table

## Dependencies

Stdlib only (Python 3.11+). No PyPI packages required.

## Tests

```bash
cd bench
python3 -m pytest driver/tests/ -q
```
