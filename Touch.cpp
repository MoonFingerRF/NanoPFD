// ============================================================================
//  Touch.cpp — GT911 / CST226 tap reader that drives the map zoom.
//  A tap in the TOP half of the display zooms the map in, the BOTTOM half zooms
//  out (edge-detected: one zoom step per press). Polled from the sensor task.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "State.h"
#include "map_zoom.h"
#include "Touch.h"

#if BOARD_C
  #define TOUCH_GT911   1
  #define TOUCH_ADDR    0x5D             // 2.8B demo latches 0x5D (INT low at reset)
  #define GT911_INT_PIN 16               // touch INT (GPIO); RST = TCA9554 EXIO2
  #define TOUCH_DISP_H  RGB_HEIGHT       // 640 — full combined display height
#elif BOARD_D
  #define TOUCH_CST226  1
  #define TOUCH_ADDR    0x5A
  #define TOUCH_DISP_H  RGB_HEIGHT
#endif

#if defined(TOUCH_GT911) || defined(TOUCH_CST226)

extern HWCDC USBSerial;   // shared debug console (declared in InstrumentPanel.ino)
static bool gPrevDown = false;

// ---- GT911 (16-bit register addressing) ------------------------------------
#if TOUCH_GT911
static bool gtRead(uint16_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(true) != 0) return false;      // STOP (matches the 2.8B demo)
  int got = Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)n);
  int i = 0; while (i < n && Wire.available()) buf[i++] = Wire.read();
  return i == n;
}
static void gtWrite(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF)); Wire.write(val);
  Wire.endTransmission();
}
// Returns true + (x,y) when a finger is down. GT911: status @0x814E (bit7=ready,
// low nibble=#points), point0 @0x8150 (id,xL,xH,yL,yH,...). Clear status after.
static bool readTouch(int &x, int &y) {
  uint8_t st;
  bool ok = gtRead(0x814E, &st, 1);                         // buffer status @0x814E
  if (!ok) return false;
  if (!(st & 0x80)) return false;                           // bit7 = new frame ready
  uint8_t cnt = st & 0x0F;
  bool have = false;
  if (cnt >= 1 && cnt <= 5) {
    uint8_t b[9];                                           // b[1..8] = point 0 (0x814F..0x8156)
    if (gtRead(0x814F, &b[1], 8)) { x = b[2] | (b[3] << 8); y = b[4] | (b[5] << 8); have = true; }
  }
  gtWrite(0x814E, 0);                                       // clear status (ack), as the demo does
  return have;
}
#endif // TOUCH_GT911

// ---- CST226SE (status report at 1-byte reg 0x00) ---------------------------
//  Protocol (per Lewis He's SensorLib TouchDrvCST226): read 28 bytes from reg 0x00;
//  b[5]&0x7F = #points, b[6] must be 0xAB for a valid frame; point 0 is b[1..3] with
//  12-bit packing (X = b1<<4 | b3>>4, Y = b2<<4 | b3&0x0F). When there's no valid
//  frame, clear the status (write 0xAB to reg 0x00) so it isn't left latched.
//  NOTE: axis may need swap/flip on first hardware run — see touchPoll().
#if TOUCH_CST226
static bool readTouch(int &x, int &y) {
  uint8_t b[28] = {0};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);                                         // status register (1-byte addr)
  if (Wire.endTransmission(false) != 0) return false;      // repeated start
  Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)28);
  int i = 0; while (i < 28 && Wire.available()) b[i++] = Wire.read();
  if (i < 7) return false;
  uint8_t pts = b[5] & 0x7F;
  bool valid = (b[6] == 0xAB) && pts >= 1 && pts <= 5;
  if (valid) {
    x = ((uint16_t)b[1] << 4) | (b[3] >> 4);
    y = ((uint16_t)b[2] << 4) | (b[3] & 0x0F);
  } else {
    Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x00); Wire.write(0xAB); Wire.endTransmission();
  }
  return valid;
}
#endif // TOUCH_CST226

extern HWCDC USBSerial;   // shared debug console (declared in InstrumentPanel.ino)

#if TOUCH_GT911
extern void exioSet(uint8_t pin, bool high);   // TCA9554 driver (CombinedDisplay.ino)
#endif

// IMPORTANT: call AFTER combinedDisplayInit() — it needs the TCA9554 expander up.
void touchInit() {
#if TOUCH_GT911
  // GT911 reset — EXACT Waveshare 2.8B demo sequence: INT held LOW through RST's
  // rising edge latches I2C address 0x5D and starts the scan. RST = TCA9554 EXIO2,
  // INT = GPIO16. (The panel config lives in the GT911; active resolution is at
  // 0x8146 — the config registers at 0x8047 read 0 on this board, which is normal.)
  pinMode(GT911_INT_PIN, OUTPUT);
  digitalWrite(GT911_INT_PIN, LOW);    // INT LOW => I2C address 0x5D
  exioSet(2, false);                   // TP_RST low  (TCA9554 EXIO2)
  delay(10);
  exioSet(2, true);                    // TP_RST high (release) — INT low here => 0x5D
  delay(200);
  digitalWrite(GT911_INT_PIN, HIGH);
  pinMode(GT911_INT_PIN, INPUT);       // release INT — becomes the GT911 interrupt output
  delay(50);

  uint8_t id[4] = {0}, rs[4] = {0};
  bool ok = gtRead(0x8140, id, 4);     // product id = "911\0"
  gtRead(0x8146, rs, 4);               // active resolution (X then Y) — expect ~480x640
  USBSerial.printf("[TOUCH] GT911 @0x%02X id=%c%c%c%c ok=%d res=%dx%d\n",
                   TOUCH_ADDR, id[0]?id[0]:'?', id[1]?id[1]:'?', id[2]?id[2]:'?', id[3]?id[3]:'?',
                   ok, rs[0] | (rs[1] << 8), rs[2] | (rs[3] << 8));
#endif
#if TOUCH_CST226
  // CST226SE shares the onboard 6/7 I2C bus with the sensors + PMU and powers up with
  // the board (no reset pin needed here). Confirm it ACKs at 0x5A.
  Wire.beginTransmission(TOUCH_ADDR);
  int ack = Wire.endTransmission();
  USBSerial.printf("[TOUCH] CST226 @0x%02X ack=%d (0=present)\n", TOUCH_ADDR, ack);
#endif
}

void touchPoll(state *s) {
  int x = 0, y = 0;
  bool down = readTouch(x, y);
  if (down && !gPrevDown) {                                 // new press only (one step per tap)
    float clat = s->has_pos ? s->last_lat : MAP_DEFAULT_LAT;
    float clon = s->has_pos ? s->last_lon : MAP_DEFAULT_LON;
    int dir = y < TOUCH_DISP_H / 2 ? -1 : +1;               // top half = zoom in
    mapZoom(dir, clat, clon);
    USBSerial.printf("[TOUCH] x=%d y=%d %s -> range=%dm lod=%d field=%d\n",
                     x, y, dir < 0 ? "IN" : "OUT", gMapRangeM, gMapLod, (int)gMapFieldActive);
  }
  gPrevDown = down;
}

#else   // BOARD_A (no touch)
void touchInit() {}
void touchPoll(state *) {}
#endif
