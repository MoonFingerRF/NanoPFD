# svggen — README illustrations straight from the renderer

The SVGs in [`../../docs`](../../docs) are **not hand-drawn**. They are produced by
running the actual `drawHorizonDisplay()` / `drawNavigationDisplay()` code from
[`instrument_drawer.ino`](../../instrument_drawer.ino) on the host.

## How it works

`svggen.cpp` defines an **`SvgCanvas`** — a `MyCanvas8` subclass that overrides the GFX
drawing calls and records each as a **clean SVG vector element**: `drawLine` → `<line>`
(endpoints run through the rotation matrix so the compass ticks stay vectors),
`drawCircle`/`fillCircle` → `<circle>`, `drawTriangle`/`fillTriangle` → `<polygon>`,
`fillRect`/`drawRect`/`drawFastH`/`VLine` → `<rect>`, and `write()` → `<text>`. The map's
per-pixel `mapLine()` / dotted rings are intercepted at the source (via `SVG_RENDER` hooks)
and emitted as clean `<line>`/`<circle>` clipped to the radar circle. So **nothing is
rasterised** — except the one thing that genuinely is pixels:

The **attitude indicator** is texture-sampled straight into the framebuffer (the `inc_map`
loop in `drawHorizonDisplay`), so `svggen` captures it *with pixels* — snapshotting the
buffer before and after that loop (two `SVG_RENDER` hooks) and emitting the difference as
palette-coloured rectangles. The sample state keeps the horizon level, so the attitude is
pixelated but not aliased. Adjacent same-colour rectangles are coalesced.

`drawCircle`/`fillCircle`/`drawTriangle`/`fillTriangle` are non-virtual in `Adafruit_GFX`,
so [`MyCanvas8.h`](../../MyCanvas8.h) re-declares them as virtual **only under `SVG_RENDER`**
(plain non-virtual passthroughs on the device build) so the canvas can intercept them.

It also emits `docs/nd_legend.svg` — the annotated ND. The drawers contain a handful of
`SVG_RENDER`-guarded `svgLandmark()` calls that report a representative screen position for
each map feature (towered/non-towered airport, navaid, city, distance ring, river, road,
state line, ground-track / home lines, battery readout). `svggen` dims the rendered ND and
draws leader lines + labels to those exact points, so every callout lands on a real feature.

## Regenerate

```bash
tools/svggen/build.sh [path-to-Adafruit_GFX_Library]
```

Defaults to `~/Documents/Arduino/libraries/Adafruit_GFX_Library`. It compiles twice — the
single-panel config (BOARD_C) for `docs/{pfd,nd,combined,nd_legend}.svg`, and the
dual-display config (BOARD_A) for `docs/dual.svg` (two rounded-corner viewports).

The `shim/` directory is a minimal Arduino compatibility layer (just enough of
`Arduino.h` / `Print.h` for `Adafruit_GFX` and the drawers to compile off-target);
it has nothing to do with the firmware build.
