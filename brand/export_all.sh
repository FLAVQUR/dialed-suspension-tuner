#!/bin/sh
# Rebuild everything in brand/: masters, exports, web icons, presentation board.
# Needs the logo-design skill (https://github.com/kaankiziltug/logo-design-skill) in ~/.claude/skills/logo-design.
# Run from the repository root:  sh brand/export_all.sh
set -e
PY=${PY:-~/.platformio/penv/Scripts/python.exe}
S=${S:-~/.claude/skills/logo-design/scripts}
INK="#11212D"; FOG="#CCD0CF"
M=brand/master
E=brand/export

"$PY" -I brand/build_logo.py
rm -rf "$E"

# One-colour versions: black, white, Ink (light backgrounds), Fog (dark backgrounds)
"$PY" -I "$S/export_variants.py" $M/dialed-symbol.svg --out-dir $E/one-colour --name dialed-symbol --title "Dialed" \
    --only black white mono square --mono "$INK" --mono "$FOG" --png 512 1024
for m in wordmark stacked; do
    "$PY" -I "$S/export_variants.py" $M/dialed-$m.svg --out-dir $E/one-colour --name dialed-$m --title "Dialed" \
        --only black white mono --mono "$INK" --mono "$FOG" --png 1200
done

# Colour versions (the primary logo, for dark backgrounds): copies of the masters plus PNGs
mkdir -p $E/colour
for m in symbol-color wordmark-color stacked-color app-icon; do cp $M/dialed-$m.svg $E/colour/; done
"$PY" -I "$S/render_png.py" $M/dialed-symbol-color.svg -o $E/colour/dialed-symbol-color-1024.png --size 1024
"$PY" -I "$S/render_png.py" $M/dialed-app-icon.svg -o $E/colour/dialed-app-icon-1024.png --size 1024
"$PY" -I "$S/render_png.py" $M/dialed-wordmark-color.svg -o $E/colour/dialed-wordmark-color-1200.png --width 1200 --height 364
"$PY" -I "$S/render_png.py" $M/dialed-stacked-color.svg -o $E/colour/dialed-stacked-color-1200.png --width 1200 --height 1170
"$PY" -I "$S/render_png.py" $M/dialed-wordmark-color.svg -o $E/colour/dialed-wordmark-color-on-night-1200.png \
    --width 1200 --height 400 --padding 0.08 --bg "#06141B"

# Web icons: the favicon is the colour logo on an Ink tile (shows in light and dark tabs);
# the app icons are full-bleed Ink squares, the platform rounds the corners itself.
W=$E/web
mkdir -p $W
cp $M/dialed-favicon.svg $W/favicon.svg
for s in 16 32 48; do "$PY" -I "$S/render_png.py" $M/dialed-favicon.svg -o $W/favicon-$s.png --size $s; done
"$PY" -I "$S/render_png.py" $M/dialed-favicon.svg --ico $W/favicon.ico --ico-sizes 16 32 48
"$PY" -I "$S/render_png.py" $M/dialed-symbol-color.svg -o $W/apple-touch-icon.png --size 180 --bg "$INK" --padding 0.1
"$PY" -I "$S/render_png.py" $M/dialed-symbol-color.svg -o $W/icon-192.png --size 192 --bg "$INK" --padding 0.1
"$PY" -I "$S/render_png.py" $M/dialed-symbol-color.svg -o $W/icon-512.png --size 512 --bg "$INK" --padding 0.1
"$PY" -I "$S/render_png.py" $M/dialed-symbol-color.svg -o $W/maskable-512.png --size 512 --bg "$INK" --padding 0.22
cat > $W/site.webmanifest <<EOF
{
  "name": "Dialed",
  "icons": [
    {"src": "/icon-192.png", "sizes": "192x192", "type": "image/png"},
    {"src": "/icon-512.png", "sizes": "512x512", "type": "image/png"},
    {"src": "/maskable-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable"}
  ],
  "theme_color": "$INK",
  "background_color": "$INK",
  "display": "standalone"
}
EOF
cat > $W/head-snippet.html <<EOF
<link rel="icon" href="/favicon.ico" sizes="48x48">
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<link rel="manifest" href="/site.webmanifest">
<meta name="theme-color" content="$INK">
EOF

# Presentation board (its browser-tab mockup prints https://<brand>.com: point it at the device instead)
B=$(mktemp -d)
sed 's#https://{handle}.com#http://dialedtuner.local#' "$S/presentation_board.py" > "$B/presentation_board.py"
rm -f brand/board/slides/*.png
(cd brand && PYTHONPATH="$S" "$PY" "$B/presentation_board.py" board/spec.json -o board/board.html --png-dir board/slides)
rm -rf "$B"

# The dashboard's icons: favicon (SVG, and ICO for browsers without SVG favicons) and the iOS home-screen icon.
# Each has its own route in serverInit() (src/web/server.cpp); a new one needs a route too.
cp $W/favicon.svg $W/favicon.ico $W/apple-touch-icon.png data/
echo "brand/ rebuilt, icons copied into data/"
