# `tools/diagrams/` — documentation diagrams

The hand-drawn SVGs under [`docs/images/`](../../docs/images/) are generated,
not drawn by hand. Edit the script, then regenerate; don't edit the SVGs.

```bash
python3 tools/diagrams/generate.py            # rewrite every SVG
python3 tools/diagrams/generate.py --check    # exit 1 if any SVG is out of date
```

Python 3 standard library only. The output is deterministic: each diagram
seeds its own jitter, so an unchanged script writes byte-identical SVG.

| Script | Writes | Used in |
|---|---|---|
| `leaf_concentrator.py` | `leaf-concentrator.svg` | README, leaf and concentrator section |
| `usage.py` | `usage-cpp-api.svg`, `usage-otelcpp-shim.svg` | README "How you use it"; the shim one also in `docs/migration-from-otel-cpp.md` |
| `architecture_layers.py` | `architecture-layers.svg` | `docs/architecture.md` §2 |
| `sketch.py` | — | shared drawing code, fonts and colours |
| `generate.py` | — | runs them all |

## Conventions

- **One SVG for both GitHub themes.** Boxes have opaque pastel fills with dark
  text; everything outside a box uses the mid grey `OUTER` (about 3.9:1 on
  white, 4.9:1 on GitHub's dark background). Don't use `<picture>` with
  `prefers-color-scheme`: it follows the OS setting, not the page's theme.
- **System fonts only.** GitHub serves README images in a way that blocks web
  fonts, so `FONT` is a stack of system sans-serif faces and text renders a
  little differently per OS. Leave slack in box widths.
- **Alt text.** Each `svg(...)` call carries an `aria-label`; copy the same
  description into the `alt` of the `<img>` that embeds it.
- **Adding a diagram:** write a `render()` in a new module, register its output
  name in `generate.py`, then check it on both backgrounds, for example
  `magick -background '#0d1117' docs/images/<name>.svg /tmp/dark.png`.
- Changing a seed, or drawing strokes in a different order, re-jitters the
  whole diagram. That's harmless, but it makes the SVG diff noisy.
