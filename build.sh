#!/usr/bin/env bash
# ============================================================================
#  build.sh — reproducible build/flash for the InstrumentPanel EFIS.
#
#  Why this exists (don't just call arduino-cli directly):
#   1. The board has 8 MB OCTAL PSRAM — it only turns on with PSRAM=opi.
#   2. The user's ~/Documents/Arduino Arduino_GFX copies are too old for ESP32
#      core 3.3.7, so we build against a clean clone in an ISOLATED library set.
#   3. That clone is PATCHED so the SPI blit YIELDS the CPU during each DMA
#      transfer (lets the sensor task + a second display run) instead of
#      busy-spinning. This is what lets two panels blit without starving sensors.
#
#  Usage:  ./build.sh [serial-port]
# ============================================================================
set -e

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
SKETCH="$(cd "$(dirname "$0")" && pwd)"
GFX=/tmp/Arduino_GFX_fixed
SKB=/tmp/clean_sketchbook
USERLIB="$HOME/Documents/Arduino/libraries"

# --- 1. Clean Arduino_GFX clone (the user's installed copies are too old) -----
if [ ! -f "$GFX/src/Arduino_GFX_Library.h" ]; then
  git clone --depth 1 https://github.com/moononournation/Arduino_GFX "$GFX"
fi

# --- 2. Patch the DMA bus so the pixel blit YIELDS instead of busy-waiting -----
#   (a) spi_device_polling_* (busy spin) -> spi_device_transmit (sleeps the task
#       during the DMA, freeing the core for sensors / the other display).
#   (b) bigger transfer chunk so the per-frame context-switch overhead is small.
#       4096 px = 16 KB DMA buffers/bus; TWO buses (PFD + ND) then fit in internal
#       SRAM. (8192 was fine for one bus but two would overflow internal RAM.)
#       Both patches are idempotent.
CPP="$GFX/src/databus/Arduino_ESP32SPIDMA.cpp"
H="$GFX/src/databus/Arduino_ESP32SPIDMA.h"
# SURGICAL: only the bulk pixel blit (the 16-bit "<< 4" transfers in
# writeIndexedPixels/writePixels) yields. The command/data path (writeBytes etc.)
# stays on polling — yielding it broke the NV3030B gamma/VCOM register upload
# (multi-byte private-register writes silently failed -> washed-out blacks).
perl -0pi -e 's/(_spi_tran\.length = [^;]+<< 4;\s*\n\s*_spi_tran\.flags = 0;\s*\n\s*)POLL_START\(\);\s*\n\s*POLL_END\(\);/${1}spi_device_transmit(_handle, &_spi_tran);/g' "$CPP"
sed -i '' 's/#define ESP32SPIDMA_MAX_PIXELS_AT_ONCE [0-9]*/#define ESP32SPIDMA_MAX_PIXELS_AT_ONCE 4096/' "$H"

# --- 3. Isolated library set (symlinks; avoids the stale user GFX copies) ------
mkdir -p "$SKB/libraries"
ln -sfn "$GFX" "$SKB/libraries/Arduino_GFX"
for L in Adafruit_BMP3XX_Library Adafruit_BNO08x Adafruit_BusIO \
         Adafruit_GFX_Library Adafruit_GPS_Library Adafruit_Unified_Sensor; do
  ln -sfn "$USERLIB/$L" "$SKB/libraries/$L"
done
ln -sfn "$USERLIB/SparkFun_9DoF_IMU_Breakout_-_ICM_20948_-_Arduino_Library" \
        "$SKB/libraries/SparkFun_ICM20948"
# BOARD_D (LilyGO T4-S3 AMOLED) uses a SELF-CONTAINED RM690B0 QSPI driver
# (CombinedDisplayAmoled.ino) — no LilyGo-AMOLED-Series / SensorLib / XPowersLib.

# --- 4. Compile + upload (PSRAM=opi is REQUIRED for the 8 MB octal PSRAM) ------
ARDUINO_DIRECTORIES_USER="$SKB" arduino-cli compile --upload \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M" \
  --port "$PORT" "$SKETCH"
