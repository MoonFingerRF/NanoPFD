// ============================================================================
//  FlightLog.ino — circular flight-data logger.
//
//  Logs GPS ground speed, indicated airspeed, MSL altitude, and load factor (g) at
//  10 Hz into a ring buffer holding the last 30 min (18000 samples) in PSRAM. Session
//  peaks (top speed / airspeed / alt / g) are tracked separately and survive ring wrap.
//
//  The ring is persisted to a LittleFS file on the config-mode toggle (flightLogSave(),
//  called just before the reboot in InstrumentPanel.ino) and reloaded at boot, so a
//  flight recorded in flight mode can be reviewed on the web page after switching to
//  config mode (the toggle reboots, which would otherwise clear the RAM ring).
//
//  The config portal (WebConfig.ino) reads it back: stats + a downsampled series for the
//  plot, and the full 10 Hz data as a CSV download.
// ============================================================================
#include "State.h"
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include <sys/time.h>

#define FLOG_HZ    10
#define FLOG_MIN   30
#define FLOG_CAP   (FLOG_HZ * 60 * FLOG_MIN)     // 18000 samples
#define FLOG_MAGIC1 0x464C4731u                  // 'FLG1' (no timestamps — legacy persisted logs)
#define FLOG_MAGIC2 0x464C4732u                  // 'FLG2' (carries the newest-sample epoch + source)
#define FLOG_PATH  "/flightlog.bin"

struct FlogSample { float gs, asi, alt, g; };    // 16 bytes/sample -> 288 KB ring

static FlogSample       *gFlog     = nullptr;    // PSRAM ring
static volatile uint32_t gFlogHead = 0;          // monotonic write count (newest = head-1)
static bool              gFlogFsOk = false;
// Session peaks (since power-on / last reset), readable by the config portal.
volatile float gFlogMaxGs = 0, gFlogMaxAsi = 0, gFlogMaxAlt = 0, gFlogMaxG = 0;

// Wall-clock time of the NEWEST sample (epoch ms) + its source. Every sample is FLOG_HZ apart, so
// the portal back-counts each sample's time from this. tSrc: 0=none, 1=system(relative/uptime),
// 2=GPS UTC. When GPS sets the clock mid-log, the newest epoch becomes real UTC and the whole plot
// snaps to actual time (the back-count uses the corrected newest epoch).
extern volatile bool gGpsTimeSet;                // set by GPS.ino once the system clock is GPS-UTC
static int64_t       gFlogEpochMs = 0;
static uint8_t       gFlogTimeSrc = 0;
static int64_t flogNowMs() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
uint32_t flightLogLastEpochSec() { return (uint32_t)(gFlogEpochMs / 1000); }   // 0 if never stamped
uint8_t  flightLogTimeSrc()      { return gFlogTimeSrc; }

// Number of valid samples currently in the ring (caps at FLOG_CAP once it wraps).
uint32_t flightLogCount() { return gFlogHead < (uint32_t)FLOG_CAP ? gFlogHead : (uint32_t)FLOG_CAP; }

// Read sample j (j = 0 is the OLDEST retained sample). Out-params avoid sharing FlogSample.
void flightLogGet(uint32_t j, float *gs, float *asi, float *alt, float *g) {
  uint32_t base = (gFlogHead < (uint32_t)FLOG_CAP) ? 0 : (gFlogHead % FLOG_CAP);
  FlogSample &e = gFlog[(base + j) % FLOG_CAP];
  *gs = e.gs; *asi = e.asi; *alt = e.alt; *g = e.g;
}

// Allocate the ring (PSRAM) and reload a persisted log from flash, if present.
void flightLogInit() {
  gFlog = (FlogSample *)heap_caps_malloc((size_t)sizeof(FlogSample) * FLOG_CAP,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gFlog) { USBSerial.println("[FLOG] PSRAM alloc failed"); return; }
  memset(gFlog, 0, (size_t)sizeof(FlogSample) * FLOG_CAP);
  gFlogFsOk = LittleFS.begin(true);                       // format the new partition on first boot
  if (gFlogFsOk && LittleFS.exists(FLOG_PATH)) {
    File f = LittleFS.open(FLOG_PATH, "r");
    uint32_t magic = 0; f.read((uint8_t *)&magic, 4);
    if (magic == FLOG_MAGIC1 || magic == FLOG_MAGIC2) {
      f.read((uint8_t *)&gFlogHead, 4);
      f.read((uint8_t *)&gFlogMaxGs, 4);  f.read((uint8_t *)&gFlogMaxAsi, 4);
      f.read((uint8_t *)&gFlogMaxAlt, 4); f.read((uint8_t *)&gFlogMaxG, 4);
      if (magic == FLOG_MAGIC2) {                         // FLG2 also carries the saved time base
        f.read((uint8_t *)&gFlogEpochMs, 8);
        f.read((uint8_t *)&gFlogTimeSrc, 1);
      }
      f.read((uint8_t *)gFlog, (size_t)sizeof(FlogSample) * FLOG_CAP);
    }
    f.close();
  }
  USBSerial.printf("[FLOG] init cap=%d fs=%d head=%u\n", FLOG_CAP, (int)gFlogFsOk, (unsigned)gFlogHead);
}

// Append one sample. Call frequently; self-throttles to FLOG_HZ.
void flightLogTick(const state *s) {
  if (!gFlog) return;
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < (uint32_t)(1000 / FLOG_HZ)) return;
  last = now;
  FlogSample &e = gFlog[gFlogHead % FLOG_CAP];
  e.gs = s->ground_speed; e.asi = s->air_speed; e.alt = s->alt; e.g = s->g;
  gFlogHead++;
  gFlogEpochMs = flogNowMs();                  // wall-clock time of this (newest) sample
  gFlogTimeSrc = gGpsTimeSet ? 2 : 1;          // 2=GPS UTC, 1=system clock (relative until GPS lock)
  if (s->ground_speed > gFlogMaxGs)  gFlogMaxGs  = s->ground_speed;
  if (s->air_speed    > gFlogMaxAsi) gFlogMaxAsi = s->air_speed;
  if (s->alt          > gFlogMaxAlt) gFlogMaxAlt = s->alt;
  if (s->g            > gFlogMaxG)   gFlogMaxG    = s->g;
}

// Persist the ring + peaks to flash (called before the config-mode reboot).
void flightLogSave() {
  if (!gFlog || !gFlogFsOk) return;
  File f = LittleFS.open(FLOG_PATH, "w");
  if (!f) return;
  uint32_t magic = FLOG_MAGIC2, head = gFlogHead;
  float mg = gFlogMaxGs, ma = gFlogMaxAsi, ml = gFlogMaxAlt, mgg = gFlogMaxG;
  int64_t ep = gFlogEpochMs; uint8_t src = gFlogTimeSrc;
  f.write((uint8_t *)&magic, 4); f.write((uint8_t *)&head, 4);
  f.write((uint8_t *)&mg, 4); f.write((uint8_t *)&ma, 4);
  f.write((uint8_t *)&ml, 4); f.write((uint8_t *)&mgg, 4);
  f.write((uint8_t *)&ep, 8); f.write((uint8_t *)&src, 1);
  f.write((uint8_t *)gFlog, (size_t)sizeof(FlogSample) * FLOG_CAP);
  f.close();
}

// Clear the log + peaks (web 'reset' button).
void flightLogReset() {
  gFlogHead = 0; gFlogMaxGs = gFlogMaxAsi = gFlogMaxAlt = gFlogMaxG = 0;
  gFlogEpochMs = 0; gFlogTimeSrc = 0;
  if (gFlog) memset(gFlog, 0, (size_t)sizeof(FlogSample) * FLOG_CAP);
  if (gFlogFsOk) LittleFS.remove(FLOG_PATH);
}
