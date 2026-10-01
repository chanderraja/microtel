#!/usr/bin/env python3
"""Regenerate the hand-drawn SVG diagrams under docs/images/.

    python3 tools/diagrams/generate.py            # rewrite every SVG
    python3 tools/diagrams/generate.py --check    # exit 1 if any SVG is stale
"""
import sys
from pathlib import Path

import architecture_layers
import leaf_concentrator
import usage

IMAGES = Path(__file__).resolve().parents[2] / "docs" / "images"

DIAGRAMS = {
    leaf_concentrator.OUTPUT: leaf_concentrator.render,
    architecture_layers.OUTPUT: architecture_layers.render,
    "usage-cpp-api.svg": usage.render_cpp_api,
    "usage-otelcpp-shim.svg": usage.render_otelcpp_shim,
}


def main(argv):
    check = "--check" in argv[1:]
    stale = []
    for name, render in DIAGRAMS.items():
        path = IMAGES / name
        svg = render()
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != svg:
                stale.append(name)
        else:
            path.write_text(svg, encoding="utf-8")
            print(f"wrote {path}")
    for name in stale:
        print(f"stale: docs/images/{name}; run tools/diagrams/generate.py", file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
