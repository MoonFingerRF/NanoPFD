#!/usr/bin/env bash
# Regenerate docs/{pfd,nd,combined}.svg from the REAL PFD/ND renderers.
#
# svggen.cpp compiles instrument_drawer.ino on the host against a tiny Arduino
# shim and an SVG-recording GFX canvas, so the README illustrations are produced
# by the exact same code that runs on the device — not drawn by hand.
#
# Usage:  tools/svggen/build.sh [path-to-Adafruit_GFX_Library]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GFX="${1:-$HOME/Documents/Arduino/libraries/Adafruit_GFX_Library}"

if [ ! -f "$GFX/Adafruit_GFX.cpp" ]; then
  echo "Adafruit_GFX not found at: $GFX" >&2
  echo "Pass the library path as the first argument." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Render the combined single-panel layout (BOARD_C) without touching config.h.
sed -e 's/#define BOARD_A 1/#define BOARD_A 0/' \
    -e 's/#define BOARD_C 0/#define BOARD_C 1/' \
    "$ROOT/config.h" > "$TMP/config.h"

# Array (not a string) so paths containing spaces survive word-splitting.
FLAGS=(-std=c++17 -DARDUINO=10819 -DSVG_RENDER -O1
       -I"$HERE/shim" -I"$TMP" -I"$ROOT" -I"$GFX")
g++ "${FLAGS[@]}" -c "$GFX/Adafruit_GFX.cpp" -o "$TMP/gfx.o"
g++ "${FLAGS[@]}" "$HERE/svggen.cpp" "$TMP/gfx.o" -o "$TMP/svggen"

( cd "$ROOT" && "$TMP/svggen" )
echo "Regenerated $ROOT/docs/{pfd,nd,combined,nd_legend}.svg"
