// ============================================================================
//  GPS.ino — u-blox M10 over UART (UBX binary protocol)
//
//  This module ships configured for UBX output with NMEA disabled, at GPS_BAUD.
//  We parse UBX-NAV-PVT (class 0x01, id 0x07): one 92-byte message carrying fix
//  status, satellites-in-use, lat/lon, MSL altitude, ground speed and course.
//  The last position with a lock is persisted to NVS so the moving map recalls
//  it across power cycles, and the first fix is latched as "home".
// ============================================================================
#include <Preferences.h>           // NVS storage for the last GPS lock position

HardwareSerial GPSSerial(1);          // ESP32-S3 UART1 (GPS_RX / GPS_TX)
Preferences    gpsPrefs;

bool GPS_First_Fix;

void initGPS(state *s) {
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  GPS_First_Fix = false;

  // Restore the last position that had a GPS lock (saved on a previous run) so the
  // map opens there after a power cycle instead of the hard-coded default. Stays
  // the map center until/unless a fresh fix updates it.
  gpsPrefs.begin("gpspos", true);                          // read-only
  if (gpsPrefs.getBool("valid", false)) {
    s->last_lat = gpsPrefs.getFloat("lat", MAP_DEFAULT_LAT);
    s->last_lon = gpsPrefs.getFloat("lon", MAP_DEFAULT_LON);
    s->has_pos  = true;
  }
  gpsPrefs.end();
}

// Persist the last lock to NVS. Throttled: the first fix saves immediately, then
// at most once a minute and only after moving ~30 m, so flash wear is negligible.
static void saveLastPos(state *s) {
  static unsigned long lastSave  = 0;
  static float         savedLat  = 0, savedLon = 0;
  static bool          everSaved = false;
  unsigned long now = millis();
  bool moved = fabsf(s->last_lat - savedLat) > 0.0003f || fabsf(s->last_lon - savedLon) > 0.0003f;
  if (everSaved && (now - lastSave < 60000 || !moved)) return;
  gpsPrefs.begin("gpspos", false);
  gpsPrefs.putFloat("lat", s->last_lat);
  gpsPrefs.putFloat("lon", s->last_lon);
  gpsPrefs.putBool("valid", true);
  gpsPrefs.end();
  savedLat = s->last_lat; savedLon = s->last_lon; lastSave = now; everSaved = true;
}

// --- UBX frame decoder ------------------------------------------------------
// Frame: B5 62 | class | id | len(LE16) | payload | ckA ckB. The checksum is an
// 8-bit Fletcher over class..payload. We only act on NAV-PVT; other messages
// (and any oversize frame) are skipped by resyncing on the next B5 62.
#define UBX_SYNC1 0xB5
#define UBX_SYNC2 0x62

static uint8_t  ubxBuf[100];                 // NAV-PVT payload is 92 bytes
static int      ubxPos = 0, ubxLen = 0;
static uint8_t  ubxState = 0, ubxClass = 0, ubxId = 0, ubxRxCkA = 0, ckA = 0, ckB = 0;

static int32_t rdI32(const uint8_t *p) {     // little-endian signed 32-bit
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void ubxHandlePVT(state *s) {
  uint8_t fixType = ubxBuf[20];              // 0=none, 2=2D, 3=3D
  uint8_t flags   = ubxBuf[21];              // bit0 = gnssFixOK
  uint8_t numSV   = ubxBuf[23];              // satellites used in the solution
  bool ok = (flags & 0x01) && fixType >= 2;
  s->sats = numSV;                           // valid even before a usable fix
  s->GPS  = ok;
  if (ok) {
    s->lon          = rdI32(&ubxBuf[24]) * 1e-7f;          // 1e-7 deg
    s->lat          = rdI32(&ubxBuf[28]) * 1e-7f;
    s->gps_alt      = rdI32(&ubxBuf[36]) * 0.00328084f;    // hMSL mm -> ft
    s->ground_speed = rdI32(&ubxBuf[60]) * 0.00194384f;    // gSpeed mm/s -> knots
    s->ground_track = rdI32(&ubxBuf[64]) * 1e-5f;          // headMot 1e-5 deg
    s->last_lat     = s->lat;                              // remember for the map fallback
    s->last_lon     = s->lon;
    s->has_pos      = true;
    if (!GPS_First_Fix) {
      GPS_First_Fix   = true;
      s->home_lat     = s->lat;
      s->home_lon     = s->lon;
      s->home_gps_alt = s->gps_alt;
    }
    saveLastPos(s);                                        // persist to NVS (throttled)
  }
}

static void ubxByte(state *s, uint8_t b) {
  switch (ubxState) {
    case 0: if (b == UBX_SYNC1) ubxState = 1; break;
    case 1: ubxState = (b == UBX_SYNC2) ? 2 : 0; break;
    case 2: ubxClass = b; ckA = b; ckB = b; ubxState = 3; break;          // start checksum
    case 3: ubxId  = b; ckA += b; ckB += ckA; ubxState = 4; break;
    case 4: ubxLen = b; ckA += b; ckB += ckA; ubxState = 5; break;
    case 5: ubxLen |= (b << 8); ckA += b; ckB += ckA; ubxPos = 0;
            ubxState = (ubxLen == 0) ? 7 : 6;
            if (ubxLen > (int)sizeof(ubxBuf)) ubxState = 0;                // oversize -> resync
            break;
    case 6: ubxBuf[ubxPos++] = b; ckA += b; ckB += ckA;
            if (ubxPos >= ubxLen) ubxState = 7;
            break;
    case 7: ubxRxCkA = b; ubxState = 8; break;
    case 8: ubxState = 0;
            if (ubxRxCkA == ckA && b == ckB &&                            // checksum OK
                ubxClass == 0x01 && ubxId == 0x07 && ubxLen >= 92)        // NAV-PVT
              ubxHandlePVT(s);
            break;
  }
}

void updateGPS(state *s) {
  while (GPSSerial.available()) ubxByte(s, (uint8_t)GPSSerial.read());
}
