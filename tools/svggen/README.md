# svggen — README illustrations straight from the renderer

The SVGs in [`../../docs`](../../docs) are **not hand-drawn**. They are produced by
running the actual `drawHorizonDisplay()` / `drawNavigationDisplay()` code from
[`instrument_drawer.ino`](../../instrument_drawer.ino) on the host.

## How it works

`svggen.cpp` defines an **`SvgCanvas`** — a `MyCanvas8` subclass that overrides the
GFX drawing primitives (`drawPixel`, `drawFastHLine`/`VLine`, `fillRect`, `drawRect`,
`drawLine`, `fillScreen`) and records each call as an SVG vector element instead of
just writing pixels. The real drawers are then run against it for a representative
in-flight state.

The one part that isn't a GFX primitive is the **attitude indicator**: it is
texture-sampled straight into the framebuffer for speed (see the `inc_map` loop in
`drawHorizonDisplay`). `svggen` captures it as the user suggested — *with pixels* —
by snapshotting the buffer immediately before and after that loop (via two
`SVG_RENDER`-guarded hooks in the drawer) and emitting the difference as
palette-coloured rectangles. So the geometry is true vectors and the attitude is a
faithful raster of the exact bytes the device would show.

Adjacent same-colour runs are coalesced to keep the files reasonable.

It also emits `docs/nd_legend.svg` — the annotated ND. The drawers contain a handful of
`SVG_RENDER`-guarded `svgLandmark()` calls that report a representative screen position for
each map feature (towered/non-towered airport, navaid, city, distance ring, river, road,
state line, ground-track / home lines, battery readout). `svggen` dims the rendered ND and
draws leader lines + labels to those exact points, so every callout lands on a real feature.

## Regenerate

```bash
tools/svggen/build.sh [path-to-Adafruit_GFX_Library]
```

Defaults to `~/Documents/Arduino/libraries/Adafruit_GFX_Library`. Writes
`docs/pfd.svg`, `docs/nd.svg` and `docs/combined.svg`.

The `shim/` directory is a minimal Arduino compatibility layer (just enough of
`Arduino.h` / `Print.h` for `Adafruit_GFX` and the drawers to compile off-target);
it has nothing to do with the firmware build.
