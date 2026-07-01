#!/usr/bin/env bash
# Regenerate docs/*.svg from the REAL PFD/ND renderers.
#
# svggen.cpp compiles instrument_drawer.ino on the host against a tiny Arduino
# shim and an SVG-recording GFX canvas, so the README illustrations are produced
# by the exact same code that runs on the device — not drawn by hand. Everything
# except the attitude texture (inc_map) comes out as clean SVG vectors.
#
# It is compiled twice: once for the single-panel config (BOARD_C) which emits
# pfd/nd/combined/nd_legend, and once for the dual-display config (BOARD_A) which
# emits dual.svg (two rounded-corner viewports).
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
COMMON=(-std=c++17 -DARDUINO=10819 -DSVG_RENDER -O1 -I"$HERE/shim")

# Adafruit_GFX itself is board/SVG-agnostic — compile it once and reuse.
g++ "${COMMON[@]}" -I"$ROOT" -I"$GFX" -c "$GFX/Adafruit_GFX.cpp" -o "$TMP/gfx.o"

board_config() {   # $1 = A|C|D  -> write a config.h with only that board selected
  sed -e "s/#define BOARD_A [01]/#define BOARD_A $([ "$1" = A ] && echo 1 || echo 0)/" \
      -e "s/#define BOARD_C [01]/#define BOARD_C $([ "$1" = C ] && echo 1 || echo 0)/" \
      -e "s/#define BOARD_D [01]/#define BOARD_D $([ "$1" = D ] && echo 1 || echo 0)/" \
      "$ROOT/config.h"
}

# ---- single-panel config (BOARD_C): pfd / nd / combined / nd_legend ----
mkdir -p "$TMP/c"; board_config C > "$TMP/c/config.h"
g++ "${COMMON[@]}" -I"$TMP/c" -I"$ROOT" -I"$GFX" "$HERE/svggen.cpp" "$TMP/gfx.o" -lz -o "$TMP/svggen_c"
( cd "$ROOT" && "$TMP/svggen_c" )

# ---- dual-display config (BOARD_A): dual.svg ----
mkdir -p "$TMP/a"; board_config A > "$TMP/a/config.h"
g++ "${COMMON[@]}" -I"$TMP/a" -I"$ROOT" -I"$GFX" "$HERE/svggen.cpp" "$TMP/gfx.o" -lz -o "$TMP/svggen_a"
( cd "$ROOT" && "$TMP/svggen_a" )

# ---- ND with a demo user flight plan overlaid (single-panel): nd-route.svg ----
mkdir -p "$TMP/c"; board_config C > "$TMP/c/config.h"
g++ "${COMMON[@]}" -I"$TMP/c" -I"$ROOT" -I"$GFX" -DFPLAN_DEMO \
    "$HERE/svggen.cpp" "$TMP/gfx.o" -lz -o "$TMP/svggen_route"
( cd "$ROOT" && "$TMP/svggen_route" )

# ---- annotated ND + PFD legends (single-panel, real national chart centred on PHL) ----
# Uses the real chart_data.h (per-LOD pyramid) so the legend map is the actual data.
g++ "${COMMON[@]}" -I"$TMP/c" -I"$ROOT" -I"$GFX" -I"$HERE" \
    -DLEGEND_BUILD \
    "$HERE/svggen.cpp" "$TMP/gfx.o" -lz -o "$TMP/svggen_legend"
( cd "$ROOT" && "$TMP/svggen_legend" )

echo "Regenerated $ROOT/docs/{pfd,nd,combined,nd-route,nd_legend,pfd_legend,dual}.svg"
