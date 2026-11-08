"""Draws the dashboard's palette, one SVG per theme, from web/style.css.

    ~/.platformio/penv/Scripts/python.exe -I tools/palette_swatches.py

Writes docs/media/palette-dark.svg and docs/media/palette-light.svg, which
docs/design-system.md shows. style.css is the source of truth: the colours
are read from its :root block (the dark theme) and its [data-theme="light"]
block (the light theme, inheriting whatever it doesn't set), so run this
again after any colour change. Only plain hex tokens are drawn; the glass,
glow and tint tokens (rgba) are effects, not palette.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS = os.path.join(ROOT, 'web', 'style.css')
OUT = os.path.join(ROOT, 'docs', 'media')

# What is drawn, in order: (group title, [(token, job)], what the contrast
# figure is measured against). The jobs are short on purpose; the rules are
# in docs/design-system.md.
GROUPS = [
    ('Neutrals — the base palette', [
        ('--bg', 'the page'),
        ('--surface', 'cards, dialogs'),
        ('--surface-alt', 'inputs, chips, secondary buttons'),
        ('--border', 'borders, gridlines'),
        ('--text-muted', 'labels, secondary text, axes'),
        ('--text', 'text'),
        ('--accent', 'interaction: primary button, active tab'),
    ], 'surface'),
    ('Status — never data', [
        ('--good', 'a check passing'),
        ('--warn', 'waiting; data not to be trusted'),
        ('--bad', 'a fault; destructive (fills, dots)'),
        ('--bad-text', 'red as text'),
    ], 'surface'),
    ('Data — one job each', [
        ('--series-front', 'the fork'),
        ('--series-rear', 'the shock'),
        ('--series-motion', 'GPS: speed, altitude'),
        ('--series-imu', 'IMU: lean, pitch, G-force'),
        ('--series-highlight', 'this one: Run B, technical'),
        ('--series-neutral', 'set against: Run A, reference'),
        ('--series-faster', 'Compare: B faster here'),
        ('--series-slower', 'Compare: B slower here'),
    ], 'surface'),
    ('Map — over the light tiles, in both themes', [
        ('--map-ink', 'Run A, markers'),
        ('--map-faster', 'B faster, over a white casing'),
        ('--map-slower', 'B slower, over a white casing'),
        ('--map-even', 'B even or not compared; 3.3:1 on its casing'),
    ], 'tile'),
]

TILE = '#F2EFE9'        # OpenStreetMap's land colour, what map lines sit on

COLS = 4
CELL_W, CELL_H = 236, 62
PAD = 28
CARD_PAD = 20
GROUP_GAP = 18
HEAD_H = 26


def parse_tokens(css):
    """The hex tokens of the dark (:root) and light themes."""
    def block(selector):
        m = re.search(re.escape(selector) + r'\s*\{(.*?)\n\}', css, re.S)
        if not m:
            sys.exit(f'{selector} block not found in style.css')
        return dict(re.findall(r'(--[\w-]+):\s*(#[0-9A-Fa-f]{6})\b', m.group(1)))

    dark = block(':root')
    light = dict(dark)
    light.update(block('[data-theme="light"]'))
    return dark, light


def luminance(hex_colour):
    channels = [int(hex_colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    lin = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def esc(text):
    return text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def draw(tokens, theme_name):
    bg, surface, border = tokens['--bg'], tokens['--surface'], tokens['--border']
    text, muted = tokens['--text'], tokens['--text-muted']

    width = PAD * 2 + CARD_PAD * 2 + COLS * CELL_W
    parts = []
    y = PAD + 34     # under the title

    for title, items, against in GROUPS:
        rows = (len(items) + COLS - 1) // COLS
        card_h = CARD_PAD * 2 + HEAD_H + rows * CELL_H
        parts.append(f'<rect x="{PAD}" y="{y}" width="{width - PAD * 2}" height="{card_h}" rx="14" '
                     f'fill="{surface}" stroke="{border}"/>')
        parts.append(f'<text x="{PAD + CARD_PAD}" y="{y + CARD_PAD + 12}" font-size="12" font-weight="700" '
                     f'letter-spacing="0.05em" fill="{muted}">{esc(title.upper())}</text>')

        for i, (token, job) in enumerate(items):
            if token not in tokens:
                sys.exit(f'{token} not found in style.css')

            colour = tokens[token]
            cx = PAD + CARD_PAD + (i % COLS) * CELL_W
            cy = y + CARD_PAD + HEAD_H + (i // COLS) * CELL_H

            # The swatch. Map colours sit on a patch of tile colour, as they
            # do on the page; a swatch as light as its card gets an outline.
            if against == 'tile':
                parts.append(f'<rect x="{cx}" y="{cy}" width="44" height="44" rx="10" fill="{TILE}" stroke="{border}"/>')
                parts.append(f'<rect x="{cx + 8}" y="{cy + 8}" width="28" height="28" rx="6" fill="{colour}"/>')
                ground = TILE
            else:
                parts.append(f'<rect x="{cx}" y="{cy}" width="44" height="44" rx="10" fill="{colour}" '
                             f'stroke="{border if contrast(colour, surface) < 1.6 else "none"}"/>')
                ground = surface

            if token == '--accent':
                parts.append(f'<text x="{cx + 22}" y="{cy + 27}" font-size="13" font-weight="700" text-anchor="middle" '
                             f'fill="{tokens["--accent-text"]}">Aa</text>')

            # A surface against the card says nothing; the border's figure
            # does (how faint a gridline is), so it keeps one.
            surfaces = ('--bg', '--surface', '--surface-alt')
            ratio = '' if token in surfaces else f' · {contrast(colour, ground):.1f}:1'
            parts.append(f'<text x="{cx + 54}" y="{cy + 13}" font-size="13" font-weight="600" fill="{text}">{esc(token)}</text>')
            parts.append(f'<text x="{cx + 54}" y="{cy + 29}" font-size="12" fill="{muted}" '
                         f'font-family="ui-monospace, Consolas, monospace">{colour.upper()}{esc(ratio)}</text>')
            parts.append(f'<text x="{cx + 54}" y="{cy + 44}" font-size="11" fill="{muted}">{esc(job)}</text>')

        y += card_h + GROUP_GAP

    height = y - GROUP_GAP + PAD + 18
    note = 'contrast against the card; map colours against the map tiles'

    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
            f'font-family="system-ui, -apple-system, Segoe UI, Roboto, sans-serif">\n'
            f'<rect width="{width}" height="{height}" rx="16" fill="{bg}"/>\n'
            f'<text x="{PAD}" y="{PAD + 18}" font-size="20" font-weight="700" fill="{text}">Dialed — {theme_name} theme</text>\n'
            f'<text x="{width - PAD}" y="{PAD + 18}" font-size="12" text-anchor="end" fill="{muted}">{esc(note)}</text>\n'
            + '\n'.join(parts) +
            f'\n<text x="{PAD}" y="{height - PAD + 6}" font-size="11" fill="{muted}">'
            f'Generated from web/style.css by tools/palette_swatches.py</text>\n</svg>\n')


def main():
    with open(CSS, encoding='utf-8') as f:
        dark, light = parse_tokens(f.read())

    os.makedirs(OUT, exist_ok=True)
    for name, tokens in (('dark', dark), ('light', light)):
        path = os.path.join(OUT, f'palette-{name}.svg')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(draw(tokens, name.capitalize()))
        print('wrote', os.path.relpath(path, ROOT))


if __name__ == '__main__':
    main()
