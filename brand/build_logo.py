"""Build the Dialed logo masters (brand/master/*.svg) from exact geometry.

The mark is a geometric lowercase "d" whose bowl is an adjuster dial: one index tick, from the counter
across the ring to its edge. The primary logo is Fog on dark with the tick in lime; the one-colour
versions (Ink on light, Fog on dark, black, white) keep the same silhouette.
The wordmark "dialed" is drawn from the same parts (rings and bars), so no font is involved.
Every shape is a fill; there are no strokes, text, filters or masks.

    python -I brand/build_logo.py
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "master")

INK = "#11212D"       # palette step 2: the app tile, and the one-colour logo on light backgrounds
FOG = "#CCD0CF"       # palette step 6: the logo on dark backgrounds
NIGHT = "#06141B"     # palette step 1: the dashboard's dark page
LIME = "#C6F135"      # the dial's tick, in the colour versions only (on dark); never in the UI or a chart

# Letter geometry, in units: baseline y=0, x-height 100, ascender 140.
XH, ASC = 100, 140
T = 22                # stem width
OVERSHOOT = 1         # round forms pass the baseline and x-height by 1 unit
TICK_ANGLE = -135     # index tick at half past ten
TICK_LEN = 15         # how far the tick reaches into the counter (counter radius 29)

# Optical spacing between letters: straight|straight > straight|round > round|round.
SPACE = {"d-i": 26, "i-a": 22, "a-l": 26, "l-e": 22, "e-d": 20}


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


class Parts:
    """Geometry for one stroke weight. The reversed version uses a slightly thinner stroke (irradiation)."""

    def __init__(self, t=T, tick_w=None, tick_len=TICK_LEN):
        self.t = t
        self.R = XH / 2 + OVERSHOOT          # bowl outer radius, centre at y=-50
        self.r = self.R - t                  # same outer size at every weight; a thinner stroke opens the counter
        self.tick_w = tick_w or t * 0.9      # a stroke across the counter reads heavier: 10% thinner
        self.tick_len = tick_len

    def ring(self, cx, cy=-XH / 2):
        R, r = self.R, self.r
        return (f'<path fill-rule="evenodd" d="'
                f"M{f(cx+R)},{f(cy)}A{f(R)},{f(R)} 0 1 0 {f(cx-R)},{f(cy)}A{f(R)},{f(R)} 0 1 0 {f(cx+R)},{f(cy)}Z"
                f'M{f(cx+r)},{f(cy)}A{f(r)},{f(r)} 0 1 1 {f(cx-r)},{f(cy)}A{f(r)},{f(r)} 0 1 1 {f(cx+r)},{f(cy)}Z"/>')

    @staticmethod
    def bar(x, y, w, h):
        return f'<path d="M{f(x)},{f(y)}h{f(w)}v{f(h)}h{f(-w)}Z"/>'

    def stem_x(self, cx):
        """Left edge of a bowl letter's stem: its outer edge lines up with the bowl's."""
        return cx + self.R - self.t

    def tick(self, cx, cy=-XH / 2, fill=None):
        """The index mark: from inside the counter across the ring to its outer edge, the end following the circle.

        In one colour it merges with the ring, so only the part in the counter shows. In colour (`fill`),
        painted over the ring, it reads as the mark painted across an adjuster knob's rim.
        """
        a = math.radians(TICK_ANGLE)
        ux, uy = math.cos(a), math.sin(a)
        vx, vy = -uy, ux
        w, R = self.tick_w, self.R
        r_in, r_out = self.r - self.tick_len, math.sqrt(R * R - (w / 2) ** 2)
        pt = lambda rad, side: (cx + ux * rad + side * vx * w / 2, cy + uy * rad + side * vy * w / 2)
        p1, p2, p3, p4 = pt(r_in, 1), pt(r_out, 1), pt(r_out, -1), pt(r_in, -1)
        paint = f' fill="{fill}"' if fill else ""
        return (f'<path{paint} d="M{f(p1[0])},{f(p1[1])}L{f(p2[0])},{f(p2[1])}A{f(R)},{f(R)} 0 0 0 {f(p3[0])},{f(p3[1])}'
                f'L{f(p4[0])},{f(p4[1])}Z"/>')

    def e_shape(self, cx, cy=-XH / 2, open_deg=35):
        """The e as one outline, so the bar's end follows the outer circle instead of poking past it.

        Outer contour: from where the bar's underside meets the outer circle, round the top, left and bottom
        to the terminal at `open_deg` below 3 o'clock; in to the inner circle, back along it to the bar's
        underside, and along the underside to the start. The counter above the bar is a hole (evenodd).
        """
        R, r, h = self.R, self.r, self.t * 0.9 / 2           # h: half the bar's height
        p = lambda rad, deg: (cx + rad * math.cos(math.radians(deg)), cy + rad * math.sin(math.radians(deg)))
        a_out = math.degrees(math.asin(h / R))               # where the bar's edges meet the outer circle
        a_in = math.degrees(math.asin(h / r))                # ... and the inner one
        start, term_o, term_i = p(R, a_out), p(R, open_deg), p(r, open_deg)
        under_l = p(r, 180 - a_in)
        hole_r, hole_l = p(r, -a_in), p(r, 180 + a_in)
        pt = lambda q: f"{f(q[0])},{f(q[1])}"
        return (f'<path fill-rule="evenodd" d="M{pt(start)}A{f(R)},{f(R)} 0 1 0 {pt(term_o)}L{pt(term_i)}'
                f'A{f(r)},{f(r)} 0 0 1 {pt(under_l)}Z'
                f'M{pt(hole_r)}A{f(r)},{f(r)} 0 0 0 {pt(hole_l)}Z"/>')

    # ---- letters: each returns (elements, advance width) with the letter's left edge at x ----
    def d(self, x, dial=False, accent=None):
        cx = x + self.R
        els = self.ring(cx) + self.bar(self.stem_x(cx), -ASC, self.t, ASC)
        if dial:
            els += self.tick(cx, fill=accent)        # after the ring: a coloured tick is painted over it
        return els, 2 * self.R

    def i(self, x):
        dot_r = self.t * 0.6
        return self.bar(x, -XH, self.t, XH) + f'<circle cx="{f(x + self.t / 2)}" cy="{f(-ASC + dot_r)}" r="{f(dot_r)}"/>', self.t

    def a(self, x):
        cx = x + self.R
        return self.ring(cx) + self.bar(self.stem_x(cx), -XH, self.t, XH), 2 * self.R

    def l(self, x):
        return self.bar(x, -ASC, self.t, ASC), self.t

    def e(self, x):
        return self.e_shape(x + self.R), 2 * self.R

    def wordmark(self, dial=True, accent=None):
        """'dialed' with its left edge at 0 and baseline at 0. Returns (elements, width)."""
        out, x = "", 0
        seq = [("d", lambda x: self.d(x, dial, accent)), ("i", self.i), ("a", self.a), ("l", self.l), ("e", self.e), ("d", self.d)]
        for n, (name, fn) in enumerate(seq):
            els, w = fn(x)
            out += els
            x += w
            if n + 1 < len(seq):
                x += SPACE[f"{name}-{seq[n + 1][0]}"]
        return out, x


def svg(w, h, title, body, fill):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {f(w)} {f(h)}" width="{f(w)}" height="{f(h)}" '
            f'role="img" aria-labelledby="title"><title id="title">{title}</title><g fill="{fill}">{body}</g></svg>\n')


def write(name, w, h, title, body, fill):
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as fh:
        fh.write(svg(w, h, title, body, fill))


def symbol(p, size=256, height=218, nudge=-3, accent=None):
    """The dial d centred in a square; `nudge` lifts it a little above the geometric centre (optical centre)."""
    s = height / (ASC + OVERSHOOT)
    w = 2 * p.R * s
    els, _ = p.d(0, dial=True, accent=accent)
    x0 = (size - w) / 2
    y0 = (size + height) / 2 + nudge - OVERSHOOT * s
    return f'<g id="symbol" transform="translate({f(x0)},{f(y0)}) scale({f(s)})">{els}</g>'


def main():
    os.makedirs(OUT, exist_ok=True)
    reg, rev = Parts(), Parts(t=T - 1)              # reversed: 1 unit thinner (white on dark spreads)
    small = Parts(t=24, tick_len=13)                # favicon cut: heavier stroke, shorter tick, open counter

    # Integer canvases, sized from the regular weight so a master and its reversed twin swap in place.
    pad, s, s2, H = 24, 1.4, 0.62, 256
    W1 = math.ceil(reg.wordmark()[1] * s + 2 * pad)
    W2 = max(256, math.ceil(reg.wordmark(dial=False)[1] * s2 + 2 * pad))
    H2 = math.ceil(256 + 24 + ASC * s2 + 24)

    variants = (("", reg, INK, None, ""),                          # one colour, for light backgrounds
                ("-reversed", rev, FOG, None, " (on dark)"),       # one colour, for dark backgrounds
                ("-color", rev, FOG, LIME, " (on dark)"))          # the primary logo: Fog, the tick in lime
    for tag, p, fill, accent, note in variants:
        # symbol
        write(f"dialed-symbol{tag}.svg", 256, 256, f"Dialed{note}", symbol(p, accent=accent), fill)
        # the wordmark, its first d carrying the dial
        wm, ww = p.wordmark(dial=True, accent=accent)
        write(f"dialed-wordmark{tag}.svg", W1, H, f"Dialed{note}",
              f'<g id="wordmark" transform="translate({f((W1 - ww * s) / 2)},{f(H / 2 + (ASC * s) / 2)}) scale({s})">{wm}</g>',
              fill)
        # stacked: symbol over a plain wordmark (the dial appears once)
        wm2, ww2 = p.wordmark(dial=False)
        sym = symbol(p, size=256, accent=accent)
        body = (f'<g transform="translate({f((W2 - 256) / 2)},0)">{sym}</g>'
                f'<g id="wordmark" transform="translate({f((W2 - ww2 * s2) / 2)},{f(256 + 24 + ASC * s2)}) scale({s2})">{wm2}</g>')
        write(f"dialed-stacked{tag}.svg", W2, H2, f"Dialed{note}", body, fill)

    # small-size cut, one colour
    write("dialed-symbol-small.svg", 256, 256, "Dialed", symbol(small, height=232, nudge=-2), INK)
    # the tiles: the colour logo on Ink, so it shows in a light browser tab as well as a dark one
    tile = f'<rect width="256" height="256" rx="56" fill="{INK}"/>'
    write("dialed-app-icon.svg", 256, 256, "Dialed", tile + symbol(rev, height=170, nudge=-2, accent=LIME), FOG)
    tile = f'<rect width="256" height="256" rx="48" fill="{INK}"/>'                 # favicon: bigger d, small cut
    write("dialed-favicon.svg", 256, 256, "Dialed", tile + symbol(small, height=200, nudge=-2, accent=LIME), FOG)
    print("masters written to", OUT)


if __name__ == "__main__":
    main()
