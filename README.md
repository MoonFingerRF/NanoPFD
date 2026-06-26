<div align="center">

# NanoPFD

**A pocket-sized glass cockpit — a Primary Flight Display + moving-map Navigation Display that runs on a single ESP32-S3 and a small Waveshare or LilyGO screen.**

<img src="docs/combined.svg" width="280" alt="NanoPFD combined PFD + ND display"/>

</div>

NanoPFD turns a ~$15 ESP32-S3 dev display and a handful of I²C sensors into a self-contained
**EFIS** (Electronic Flight Instrument System): a real attitude indicator with airspeed and
altitude tapes on top, and a heading-up moving map of nearby airports, navaids and airspace
below — all rendered from scratch on the microcontroller, no PC or phone required.

> ⚠️ **Experimental / educational project.** NanoPFD is a hobby build, **not** a certified
> instrument. Do **not** rely on it for actual flight, navigation, or any safety-of-life use.

---

## What it shows

| Primary Flight Display | Navigation Display |
|:---:|:---:|
| <img src="docs/pfd.svg" width="380" alt="PFD"/> | <img src="docs/nd.svg" width="380" alt="ND"/> |

**PFD** — sky/ground attitude with a pitch ladder and bank scale, a fixed aircraft reference,
roll-stabilised airspeed (left) and altitude (right) tapes, digital heading, vertical-speed
indicator, a g-meter, and a turn-coordinator with slip/skid ball.

**ND** — a heading-up compass rose over a moving map: towered/non-towered airports, navaids,
runways, Class B/D and restricted airspace rings, rivers, coastlines, roads and state lines,
with decluttered labels — all centered on the live GPS position (with the last fix saved to
flash so it persists across power cycles).

On the single-screen boards both stack into one portrait panel (PFD on top, ND below); on the
dual-display board they drive two separate screens.

---

## Supported hardware

NanoPFD builds for three board configurations — pick one in [`config.h`](config.h):

| Board | Display(s) | Interface | Typical FPS |
|---|---|---|---|
| **BOARD_A** | 2× ST7789 (≈240×280) | dual SPI | PFD **≈38**, ND **≈14** |
| **BOARD_C** | Waveshare ESP32-S3-Touch-LCD-2.8B, ST7701S 480×640 IPS | RGB-parallel (LCD_CAM) | **≈13** (combined) |
| **BOARD_D** | LilyGO T4-S3, RM690B0 2.41″ AMOLED 450×600 | QSPI | **≈20** (combined) |

**Sensors** (all I²C, plus one UART):

- **BNO08x** — 9-DOF fused IMU: gravity vector → attitude, rotation vector → tilt-compensated
  heading, accelerometer → g-meter. (QMI8658 / ICM-20948 supported as fallbacks.)
- **BMP390** — barometric pressure → pressure altitude + vertical speed.
- **MS4525DO** — differential pressure (pitot) → indicated airspeed.
- **u-blox M10** GPS over UART (UBX binary) → position, ground speed, ground track.

The whole thing runs on the ESP32-S3's two cores with octal PSRAM.

---

## How it works

NanoPFD is a from-scratch renderer tuned to squeeze a smooth glass cockpit out of a tiny MCU.

- **8-bit indexed canvases.** Everything draws into `GFXcanvas8`/`MyCanvas8` buffers using a
  12-entry colour palette ([`color_index[]`](InstrumentPanel.ino)) — half the memory of RGB565,
  and the per-board conversion to the panel's real format is cheap.
- **Two-core pipeline (FreeRTOS).** A sensor task publishes a lock-protected `state` snapshot;
  the PFD and ND draw + push their pixels in parallel on separate cores, double-buffered so the
  draw of one frame overlaps the transfer of the previous one.
- **A render path per panel kind:**
  - *SPI (BOARD_A)* — index→RGB565 converted on the fly during a DMA blit to each ST7789.
  - *RGB-parallel (BOARD_C)* — composited into a 600 KB RGB565 framebuffer that the S3's
    **LCD_CAM** peripheral scans out to the panel continuously.
  - *QSPI (BOARD_D)* — a self-contained RM690B0 driver pushes **RGB332** (1 byte/pixel) with a
    **per-line RLE codec** ([`RLE332.h`](RLE332.h), word-at-a-time run detection) so the frame
    compresses ~10× before the QSPI transfer.
- **Auto-generated aeronautical map.** [`tools/build_chart_data.py`](tools/build_chart_data.py)
  fetches OurAirports + Natural Earth data, simplifies it, assigns level-of-detail tiers, and
  emits [`chart_data.h`](chart_data.h). The renderer projects it equirectangularly, rotates it
  heading-up, and clips it to the compass circle — with range-driven LOD so a 30 km view is dense
  and a national view stays clean.
- **Fresh sensor data.** The IMU FIFO is drained every loop, so attitude is always the latest
  sample — the sensors publish at **~380 Hz**, far above any display's frame rate.

---

## Building & flashing

NanoPFD builds with [`arduino-cli`](https://arduino.github.io/arduino-cli/) and the Espressif
ESP32 core (3.3.x). **Octal PSRAM must be enabled** (`PSRAM=opi`).

```bash
# 1. ESP32 core
arduino-cli core install esp32:esp32

# 2. Libraries
arduino-cli lib install "Adafruit BNO08x" "Adafruit BMP3XX Library" \
  "Adafruit GPS Library" "Adafruit GFX Library" "Adafruit BusIO" \
  "Adafruit Unified Sensor" "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
# Displays: GFX Library for Arduino (moononournation/Arduino_GFX)
arduino-cli lib install "GFX Library for Arduino"

# 3. Pick your board in config.h  (set exactly one of BOARD_A / BOARD_C / BOARD_D to 1)

# 4. Compile + upload
arduino-cli compile --upload \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M" \
  --port /dev/cu.usbmodemXXXX .
```

A convenience wrapper, [`build.sh`](build.sh), reproduces the author's exact toolchain (a pinned
Arduino_GFX clone + isolated library set) and flashes in one step:

```bash
./build.sh /dev/cu.usbmodemXXXX
```

To regenerate the map for your area:

```bash
python3 tools/build_chart_data.py --lat 39.10 --lon -84.51 --radius-km 120
```

---

## Configuration

Most knobs live in [`config.h`](config.h):

- **Board select** — `BOARD_A` / `BOARD_C` / `BOARD_D` (exactly one = 1).
- **Pins** — display, I²C, and GPS UART pins per board.
- **`MAP_RANGE_M`** — moving-map range (center → radar edge); drives the LOD tier at compile time.
- **`MAP_DEFAULT_LAT/LON`** — fallback map center when GPS is lost (and no saved fix exists).
- SPI clocks, layout offsets, task priorities/cores.

---

## Repository layout

```
InstrumentPanel.ino     entry point: tasks, shared state, colour palette, telemetry
config.h                board selection, pins, layout, tuning
instrument_drawer.ino   the PFD + ND renderers (drawHorizonDisplay / drawNavigationDisplay)
IMU.ino  GPS.ino  ASI.ino  ICM.ino    sensor drivers + fusion
CombinedDisplay.ino       BOARD_C  — ST7701S RGB panel (LCD_CAM)
CombinedDisplayAmoled.ino BOARD_D  — RM690B0 QSPI AMOLED (self-contained driver)
MyCanvas8.h  RLE332.h  layout.h  State.h    rendering primitives + helpers
chart_data.h            generated aeronautical chart data
tools/build_chart_data.py    map data generator (OurAirports + Natural Earth)
tools/svggen/           regenerates docs/*.svg by running the real renderer on the host
tests/                  host unit tests (layout math, RLE codec)
docs/                   the SVG illustrations in this README (generated, not hand-drawn)
```

> The README illustrations are produced by [`tools/svggen`](tools/svggen) — it compiles
> the actual PFD/ND drawers off-target against an SVG-recording canvas, so the pictures
> are exactly what the firmware renders. See [`tools/svggen/README.md`](tools/svggen/README.md).

---

<div align="center">
<sub>Built for the ESP32-S3 · rendered entirely on-device · <strong>experimental, not for real-world flight</strong></sub>
</div>
