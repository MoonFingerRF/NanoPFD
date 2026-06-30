# NanoPFD config portal & flight log

NanoPFD has a built-in Wi-Fi **config portal** — a small web page served from the device — so
you can change settings and review a flight log from your phone, with **no reflashing**.

---

## Config mode vs flight mode

The Wi-Fi access point and the **full** frame rate can't both have the internal memory they
want at the same time (the AP needs the RAM the fast display canvas would use), so the firmware
runs in one of two modes, chosen at boot:

| Mode | What runs | Display |
|------|-----------|---------|
| **Flight** (default) | Remote ID only (no AP) | **full fps** |
| **Config** | Wi-Fi AP + web portal, **and** Remote ID per your toggles | **lower fps** (canvas in PSRAM) |

**Both modes run the real instruments** (PFD + ND + sensors + logging) — config mode just renders
slower. So **you can fly in config mode with the AP on**; you only give up frame rate (≈6 fps on
the 2.8B / BOARD_C, better on BOARD_D). Switch to flight mode when you want full fps and don't
need the AP in the air.

**Default is flight mode** (full fps, AP off) — so the panel is fast out of the box.

**Enter config mode (turn the AP on):** hold the **BOOT** button for ~3 seconds (the device reboots
into config mode). The BOOT button also tap-adjusts the altimeter; only a long hold switches mode.

**Exit config mode (back to full fps):** tap **EXIT TO FLIGHT MODE** on the WiFi tab, or hold BOOT ~3 s.

---

## Connecting

1. Enter **config mode**: hold the **BOOT** button ~3 s (the default is flight mode, AP off).
2. On your phone, join the Wi-Fi network **`NanoPFD`**.
   - It is an **open** network out of the box (the 7-character default password is below the
     WPA2 minimum). Set an 8–63 character password on the **WiFi** tab to switch it to WPA2.
3. A **captive portal** should pop the settings page automatically. If not, open
   **`http://192.168.4.1`** in a browser.

Only one phone connects at a time.

---

## The tabs

Most controls **apply live** — you'll see the panel change as you adjust them. A few are
**save-only** (they need a reboot): the AP password and the Remote ID toggles. **SAVE TO FLASH**
persists everything.

### Attitude
- **IMU orientation** — *Upside down*, *Reverse roll*, *Reverse pitch*, *Swap roll/pitch*. These
  handle the gross 90° mounting orientations. One setting drives whichever IMU is installed.
- **Heading offset** — rotates the compass so magnetic north lines up (0–359°). Default 0.
- **Mount trim** + **Set level** — see [Mounting the IMU at any angle](#mounting-the-imu-at-any-angle).

### Display
A color picker for each of the 15 palette entries (Sky, Ground, Traffic, Water, Roads, …). The
swatch is the chosen color; the panel updates instantly. *Note:* the LilyGO AMOLED (BOARD_D)
quantizes to 8-bit color, so its on-screen shade is coarser than the picker (most visible on blues).

### Nav
- **Map zoom** — sets the moving-map range (same ladder as the on-screen pinch/tap zoom).

### Air
- **Local pressure** — the altimeter (Kollsman) setting in inHg, 28.00–31.00.

### Tune
Runtime tuning, no reflash:
- **Smoothing** (0–1, smaller = smoother but slower): attitude, g-force, altitude, vertical speed,
  airspeed.
- **Instrument scales**: VSI full-scale (ft/s) and g-meter full-scale (g).

### Log
The flight logger — see [Flight log](#flight-log).

### WiFi
- **Remote ID receiver** — enable/disable **Bluetooth LE** and **WiFi** drone detection (see
  [Remote ID](#remote-id)). *Applies after the next reboot / mode switch.*
- **AP password** — 8–63 chars for WPA2; shorter runs the AP open. *Applies next config boot.*

---

## Mounting the IMU at any angle

You don't have to mount the IMU square to the airframe:

1. On the **Attitude** tab, set the orientation flips so the horizon moves the right way (sky up,
   bank/pitch in the correct direction). These cover the 90°-step orientations.
2. Mount the device however it fits. With the **aircraft level**, tap **SET LEVEL (CAPTURE)** —
   this records the current tilt as *level* (into Pitch trim / Roll trim) so the horizon reads flat.
3. Fine-tune **Pitch trim** / **Roll trim** by hand if needed.
4. Set the **Heading offset** so the compass points north.

Trims default to 0 (no change), so an already-square mount needs nothing here.

---

## Remote ID

NanoPFD passively receives FAA Remote ID / OpenDroneID broadcasts and plots nearby drones as
orange dots (with altitude) on the compass. Two radios, each independently toggleable on the
**WiFi** tab:

- **Bluetooth LE** — most consumer drones and standalone RID modules (BT4-legacy).
- **WiFi** — drones that beacon over WiFi.

How they behave per mode:

- **Config mode (AP on):** BLE RID and WiFi RID both run **alongside the AP**. WiFi RID rides the
  AP's radio on the AP's channel (no channel hopping), so it sees drones on that channel while the
  portal is live.
- **Flight mode:** BLE RID runs normally. WiFi RID, if enabled, runs a channel-hopping monitor —
  this needs extra memory, so it moves the PFD canvas to PSRAM and **lowers the frame rate**. Leave
  WiFi RID off for full-fps flight unless you specifically want WiFi drone detection in the air.

Changes to the RID toggles take effect after the next reboot / mode switch.

---

## Flight log

A circular buffer continuously records **GPS ground speed, indicated airspeed, MSL altitude, and
load factor (g)** at **10 Hz**, keeping the **last 30 minutes** (in PSRAM). It is saved to flash
when you switch modes and reloaded at boot, so a flight recorded in flight mode survives the
switch to config mode for review.

On the **Log** tab:
- **Session peaks** — top GPS speed, top airspeed, max altitude, max g (since power-on / last reset).
- **History plot** — pick a metric (GPS / ASI / Alt / G); **drag to pan**, and use **− / + / fit**
  to zoom the time axis.
- **DOWNLOAD CSV** — the full 10 Hz log as `flightlog.csv` (`t_s,gps_kt,asi_mph,alt_ft,g`).
- **RESET LOG** — clears the buffer and peaks.

To review a flight: fly in flight mode, land, hold **BOOT** ~3 s to enter config mode (the log is
saved across the reboot), join `NanoPFD`, open the **Log** tab.

---

## What you still need to reflash for

The portal covers runtime settings. **Hardware and structural options remain build-time** in
[`config.h`](../config.h) (change them and re-run `build.sh`):

- **Board select** (`BOARD_A` / `BOARD_C` / `BOARD_D`)
- **GPIO pins** (display, I²C, GPS UART) and **display/QSPI/RGB timing**
- **I²C bus clocks**, **FreeRTOS task cores / priorities / stacks**
- **Partition table** (`partitions.csv`), **Remote ID channel-dwell timing**
- The power-on **defaults** for the runtime settings (`ALPHA_*`, `VSI_FULL_SCALE`, `GMETER_FS`,
  `RID_USE_BLE/WIFI`, orientation, heading) — handy if you want a different out-of-box state.
