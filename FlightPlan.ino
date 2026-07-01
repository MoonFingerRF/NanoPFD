// ============================================================================
//  FlightPlan.ino — user flight plan (waypoint list) with flash persistence.
//
//  A short list of named waypoints (name + lat/lon), entered from the config
//  portal's Flight Plan tab (list + zoomable map editor). Persisted to LittleFS
//  so it survives power cycles, and rendered on the ND as a solid yellow route
//  line + a marker + name at each waypoint (see drawNavigationDisplay).
//
//  Concurrency: the web task (core 0) writes the plan on Save; the ND task
//  (core 0) reads it every frame. Writes publish the count LAST and bump gFplanRev,
//  so a reader sees a consistent-enough snapshot (a stale name for one frame is
//  harmless). Both run on the same core, so there is no true parallel write/read.
// ============================================================================
#include <LittleFS.h>

#define FPLAN_MAX   30
#define FPLAN_PATH  "/fplan.bin"
#define FPLAN_MAGIC 0x46504C31u                 // 'FPL1'

struct Waypoint { char name[10]; float lat, lon; };   // 18 bytes

static Waypoint          gFplan[FPLAN_MAX];
static volatile int      gFplanN   = 0;          // number of valid waypoints
volatile uint32_t        gFplanRev = 0;          // bumped on any change (ND reloads its cache)

int      fplanCount()      { return gFplanN; }
uint32_t fplanRev()        { return gFplanRev; }

// Copy waypoint i out (name into a caller buffer >= 10 chars). Safe for the ND reader.
bool fplanGet(int i, float *lat, float *lon, char *name10) {
  if (i < 0 || i >= gFplanN) return false;
  *lat = gFplan[i].lat; *lon = gFplan[i].lon;
  if (name10) { memcpy(name10, gFplan[i].name, 10); name10[9] = 0; }
  return true;
}

static void fplanPersist() {
  if (!LittleFS.begin(true)) return;
  File f = LittleFS.open(FPLAN_PATH, "w");
  if (!f) return;
  uint32_t magic = FPLAN_MAGIC; int n = gFplanN;
  f.write((uint8_t *)&magic, 4); f.write((uint8_t *)&n, 4);
  f.write((uint8_t *)gFplan, sizeof(Waypoint) * n);
  f.close();
}

void fplanLoad() {
  if (!LittleFS.begin(true)) return;
  if (!LittleFS.exists(FPLAN_PATH)) { USBSerial.println("[FPLAN] none saved"); return; }
  File f = LittleFS.open(FPLAN_PATH, "r");
  uint32_t magic = 0; int n = 0;
  f.read((uint8_t *)&magic, 4); f.read((uint8_t *)&n, 4);
  if (magic == FPLAN_MAGIC && n >= 0 && n <= FPLAN_MAX) {
    f.read((uint8_t *)gFplan, sizeof(Waypoint) * n);
    gFplanN = n; gFplanRev++;
  }
  f.close();
  USBSerial.printf("[FPLAN] loaded %d waypoints\n", gFplanN);
}

// Replace the whole plan from a tab-separated body: "name\tlat\tlon\n" per line.
// Returns the number of waypoints stored.
int fplanSetFromText(const String &body) {
  int n = 0, i = 0, len = body.length();
  while (i < len && n < FPLAN_MAX) {
    int nl = body.indexOf('\n', i); if (nl < 0) nl = len;
    String line = body.substring(i, nl); i = nl + 1;
    line.trim();
    if (line.length() == 0) continue;
    int t1 = line.indexOf('\t'); if (t1 < 0) continue;
    int t2 = line.indexOf('\t', t1 + 1); if (t2 < 0) continue;
    String nm = line.substring(0, t1);
    float la = line.substring(t1 + 1, t2).toFloat();
    float lo = line.substring(t2 + 1).toFloat();
    if (la == 0.0f && lo == 0.0f) continue;               // skip junk
    memset(gFplan[n].name, 0, 10);
    strncpy(gFplan[n].name, nm.c_str(), 9);
    gFplan[n].lat = la; gFplan[n].lon = lo;
    n++;
  }
  gFplanN = n;                 // publish count LAST
  gFplanRev++;
  fplanPersist();
  USBSerial.printf("[FPLAN] saved %d waypoints (body %d bytes)\n", n, (int)body.length());
  return n;
}
