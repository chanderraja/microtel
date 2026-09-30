"""Shared drawing code for the hand-drawn documentation diagrams.

Every stroke is drawn twice with a small seeded jitter, which gives the
sketched look while keeping the output deterministic: the same script always
writes the same SVG.

Colours are chosen so one SVG reads on both GitHub themes: boxes are opaque
pastel with dark ink, and everything outside a box is a mid grey (#768390,
about 3.9:1 on white and 4.9:1 on #0d1117).
"""
import math
import random

FONT = "'Avenir Next','Segoe UI Variable','Segoe UI',Inter,'Helvetica Neue',Helvetica,Arial,sans-serif"
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"

INK = "#1f2328"      # text and strokes inside boxes
OUTER = "#768390"    # strokes and text outside boxes
NOTE = "#57606a"     # secondary text inside boxes
ACCENT = "#b45309"   # highlighted names inside boxes
STRIKE = "#cf222e"

GREY = "#f6f8fa"
AMBER = "#fff4d6"
BLUE = "#dbeafe"
GREEN = "#dcfce7"
VIOLET = "#ede9fe"
WHITE = "#ffffff"


def _escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Rough:
    """Accumulates SVG elements drawn with a seeded hand-drawn jitter."""

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []

    def j(self, a=1.6):
        return self.r.uniform(-a, a)

    def line(self, x1, y1, x2, y2, stroke, width=2.0, passes=2):
        for _ in range(passes):
            ax, ay = x1 + self.j(), y1 + self.j()
            bx, by = x2 + self.j(), y2 + self.j()
            mx = (ax + bx) / 2 + self.j(2.2)
            my = (ay + by) / 2 + self.j(2.2)
            self.out.append(
                f'<path d="M{ax:.1f} {ay:.1f} Q{mx:.1f} {my:.1f} {bx:.1f} {by:.1f}" '
                f'fill="none" stroke="{stroke}" stroke-width="{width}" stroke-linecap="round"/>'
            )

    def box(self, x, y, w, h, stroke, fill=None, width=2.0):
        if fill:
            pts = [(x + self.j(3), y + self.j(3)), (x + w + self.j(3), y + self.j(3)),
                   (x + w + self.j(3), y + h + self.j(3)), (x + self.j(3), y + h + self.j(3))]
            d = " ".join(f"{px:.1f},{py:.1f}" for px, py in pts)
            self.out.append(f'<polygon points="{d}" fill="{fill}"/>')
        o = 3  # corners overshoot a little, like a pen stroke
        self.line(x - o, y, x + w + o, y, stroke, width)
        self.line(x + w, y - o, x + w, y + h + o, stroke, width)
        self.line(x + w + o, y + h, x - o, y + h, stroke, width)
        self.line(x, y + h + o, x, y - o, stroke, width)

    def harrow(self, x1, y1, x2, y2, stroke, width=2.2):
        """A horizontal arrow with a fixed-size head."""
        self.line(x1, y1, x2, y2, stroke, width)
        dx = 1 if x2 > x1 else -1
        self.line(x2, y2, x2 - 13 * dx, y2 - 8, stroke, width)
        self.line(x2, y2, x2 - 13 * dx, y2 + 8, stroke, width)

    def arrow(self, x1, y1, x2, y2, width=2.4, color=OUTER):
        """An arrow at any angle."""
        self.line(x1, y1, x2, y2, color, width)
        a = math.atan2(y2 - y1, x2 - x1)
        for s in (-1, 1):
            self.line(x2, y2, x2 - 13 * math.cos(a + s * 0.55), y2 - 13 * math.sin(a + s * 0.55), color, width)

    def text(self, x, y, s, fill, size=16, weight="normal", anchor="start", style="normal"):
        self.out.append(
            f'<text x="{x}" y="{y}" fill="{fill}" font-size="{size}" font-weight="{weight}" '
            f'font-style="{style}" text-anchor="{anchor}">{_escape(s)}</text>'
        )

    def mono(self, x, y, s):
        self.out.append(
            f'<text x="{x}" y="{y}" fill="{INK}" font-size="13" text-anchor="middle" '
            f'font-family="{MONO}">{_escape(s)}</text>'
        )

    def svg(self, w, h, label):
        body = "\n  ".join(self.out)
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'role="img" aria-label="{label}">\n<g font-family="{FONT}">\n  {body}\n</g>\n</svg>\n'
        )
