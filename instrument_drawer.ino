// ============================================================================
//  instrument_drawer.ino — all PFD and ND rendering
//
//  Two top-level renderers, each called from its own display task:
//    drawHorizonDisplay()    - the Primary Flight Display (attitude, tapes,
//                              turn coordinator, VSI, G-load, FMA, etc.)
//    drawNavigationDisplay() - the rotating compass card / HSI
//
//  Everything is drawn into an 8-bit indexed-color canvas (palette in the main
//  sketch) and blitted by the task. FOV, full-scale ranges and color indices
//  come from config.h.
// ============================================================================

#define p 3.1415926

#include "layout.h"
#ifdef CHART_DATA_HEADER          // the SVG legend build swaps in a fabricated map
#include CHART_DATA_HEADER
#else
#include "chart_data.h"
#endif
#include "map_zoom.h"             // runtime zoom range/tier + field-mode state
#include "chart_cull.h"           // latitude-band binary search for the national data

// Place string `s` on canvas `c` so the (ax,ay) anchor lands on the requested
// edge/center of the text, using the glyph's MEASURED bounds (getTextBounds) — so
// a label sits correctly no matter how big the glyphs are. Caller selects font +
// size first. Works on MyCanvas8 and plain GFXcanvas8 (drawPixel is virtual).
static void drawText(GFXcanvas8 *c, const char *s, int ax, int ay,
                     lyt::HAlign h, lyt::VAlign v) {
  int16_t bx, by;
  uint16_t bw, bh;
  c->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int cx, cy;
  lyt::alignCursor(ax, ay, (int)bx, (int)by, (int)bw, (int)bh, h, v, cx, cy);
  c->setCursor(cx, cy);
  c->print(s);
}

// Clamp x to [min, max].
float clamp(float x, float min, float max) {
  if (x > max)
    return max;
  if (x < min)
    return min;
  return x;
}

// Reflect x back into [min, max] (used to wrap the pitch ladder about the horizon).
float mirror(float x, float min, float max) {
  if (x > max)
    return 2 * max - x;
  if (x < min)
    return 2 * min - x;
  return x;
}

// Build the pitch-ladder texture once at startup: a tall strip showing the
// sky/ground halves and the labelled pitch graticule, later sampled per-frame
// by the attitude indicator. `d` is the strip width (≈ horizon diameter).
GFXcanvas8 generate_inc_map(int d, int sc) {
  int h = d * 180 / FOV;
  GFXcanvas8 inc_map(d, h + 1);
  inc_map.fillScreen(IGND);
  inc_map.setTextColor(IWHITE);
  inc_map.setTextSize(sc);
  inc_map.fillRect(0, 0, d, h / 2, ISKY);
  inc_map.drawFastHLine(0, h / 2, d, IWHITE);
  for (int n = 1; n <= 9 * 4; n++) {
    int w = d / 18;
    if (n % 4 == 0 && n != 36) {
      w *= 3;
      char lbl[4];
      sprintf(lbl, "%d", n * 10 / 4);
      int yUp = (h / 2) - n * (h / 72);
      int yDn = (h / 2) + n * (h / 72);
      int xL  = 0.5 * (d - w) - 3 * sc;   // right edge of the left-hand labels
      int xR  = 0.5 * (d + w) + 5 * sc;   // left edge of the right-hand labels
      drawText(&inc_map, lbl, xL, yUp, lyt::HR, lyt::VC);
      drawText(&inc_map, lbl, xL, yDn, lyt::HR, lyt::VC);
      drawText(&inc_map, lbl, xR, yUp, lyt::HL, lyt::VC);
      drawText(&inc_map, lbl, xR, yDn, lyt::HL, lyt::VC);
    } else if (n % 2 == 0)
      w *= 2;
    inc_map.drawFastHLine(0.5 * (d - w), (h / 2) + n * (h / 72), w, IWHITE);
    inc_map.drawFastHLine(0.5 * (d - w), (h / 2) - n * (h / 72), w, IWHITE);
  }
  return inc_map;
}

// TODO/unused: stub kept as a placeholder for a future text-in-arrow helper.
// drawArrowNumber() below is the one actually used for the tape readouts.
void drawArrowText(GFXcanvas8 *canvas, int value, int x, int y, bool left) {
  canvas->setTextColor(IWHITE);
  canvas->setTextSize(2);
}

void drawDualRadialLine(GFXcanvas8 *canvas, float x, float y, float angle, float min_length, float max_length, uint8_t col) {
  float dx = sin(angle);
  float dy = cos(angle);
  canvas->drawLine(x + dx * min_length, y - dy * min_length, x + dx * max_length, y - dy * max_length, col);
  canvas->drawLine(x - dx * min_length, y - dy * min_length, x - dx * max_length, y - dy * max_length, col);
}

void drawRadialLine(GFXcanvas8 *canvas, float x, float y, float angle, float min_length, float max_length) {
  float dx = sin(angle);
  float dy = cos(angle);
  canvas->drawLine(x + dx * min_length, y - dy * min_length, x + dx * max_length, y - dy * max_length, IWHITE);
}

void drawBankAngleTriangle(MyCanvas8 *canvas, float x, float y, float r, float angle, float maxAngle, float minAngle) {
  bool overAngle = false;
  if (angle > maxAngle) {
    overAngle = true;
    angle = maxAngle;
  }
  if (angle < minAngle) {
    overAngle = true;
    angle = minAngle;
  }
  float gx = sin(angle);
  float gy = -cos(angle);
  int   sc = lyt::txtScale(canvas->width());   // scale the marker with the panel (1x at 280)
  float L  = 8 * sc, W = 4 * sc;
  canvas->drawTriangle(x + (r - 1) * gx, y + (r - 1) * gy, x + (r - 1) * gx - L * gx - W * gy, y + (r - 1) * gy - L * gy + gx * W, x + (r - 1) * gx - L * gx + W * gy, y + (r - 1) * gy - L * gy - gx * W, IWHITE);
  if (overAngle && (millis() % 500 < 250)) {
    canvas->fillTriangle(x + (r - 1) * gx, y + (r - 1) * gy, x + (r - 1) * gx - L * gx - W * gy, y + (r - 1) * gy - L * gy + gx * W, x + (r - 1) * gx - L * gx + W * gy, y + (r - 1) * gy - L * gy - gx * W, IYELLOW);
  }
}

void printCentered(GFXcanvas8 *canvas, const char *str, int x, int y) {
  uint16_t w = 0;
  uint16_t h = 0;
  int16_t x_ = 0;
  int16_t y_ = 0;
  canvas->getTextBounds(str, 0, 0, &x_, &y_, &w, &h);
  canvas->setCursor(x - x_ - 0.5 * w + 1, y - y_ - 0.5 * h + 1);
  canvas->print(str);
}

void drawArrowNumber(MyCanvas8 *canvas, const char *str, int x, int y, int w, int h, int dir) {
  const uint8_t outline_col = IGREY;
  switch (dir) {
    case 0:
      {
        x -= (w / 2) + (w / 4);
        canvas->fillRect(x - w / 2, y - h / 2, w, h, IBLACK);
        int pointW = h / 4;
        int pointL = w / 4;
        canvas->fillTriangle(x + w / 2, y + pointW, x + w / 2, y - pointW, x + w / 2 + pointL, y, IBLACK);
        canvas->drawLine(x - w / 2, y + h / 2, x + w / 2, y + h / 2, outline_col);
        canvas->drawLine(x - w / 2, y - h / 2, x + w / 2, y - h / 2, outline_col);
        canvas->drawLine(x - w / 2, y - h / 2, x - w / 2, y + h / 2, outline_col);
        canvas->drawLine(x + w / 2, y + h / 2, x + w / 2, y + pointW, outline_col);
        canvas->drawLine(x + w / 2, y + pointW, x + w / 2 + pointL, y, outline_col);
        canvas->drawLine(x + w / 2, y - pointW, x + w / 2 + pointL, y, outline_col);
        canvas->drawLine(x + w / 2, y - h / 2, x + w / 2, y - pointW, outline_col);
        printCentered(canvas, str, x, y);
        break;
      }
    case 1:
      {
        x += (w / 2) + (w / 4);
        canvas->fillRect(x - w / 2, y - h / 2, w, h, IBLACK);
        int pointW = h / 4;
        int pointL = w / 4;
        canvas->fillTriangle(x - w / 2, y + pointW, x - w / 2, y - pointW, x - w / 2 - pointL, y, IBLACK);
        canvas->drawLine(x - w / 2, y + h / 2, x + w / 2, y + h / 2, outline_col);
        canvas->drawLine(x - w / 2, y - h / 2, x + w / 2, y - h / 2, outline_col);
        canvas->drawLine(x + w / 2, y - h / 2, x + w / 2, y + h / 2, outline_col);
        canvas->drawLine(x - w / 2, y + h / 2, x - w / 2, y + pointW, outline_col);
        canvas->drawLine(x - w / 2, y + pointW, x - w / 2 - pointL, y, outline_col);
        canvas->drawLine(x - w / 2, y - pointW, x - w / 2 - pointL, y, outline_col);
        canvas->drawLine(x - w / 2, y - h / 2, x - w / 2, y - pointW, outline_col);
        printCentered(canvas, str, x, y);
        break;
      }
    case 2:
      {
        y -= (h / 2) + (h / 2);
        canvas->fillRect(x - w / 2, y - h / 2, w, h, IBLACK);
        int pointW = w / 5;
        int pointL = h / 2;
        canvas->fillTriangle(x, y + h / 2 + pointL, x - pointW, y + h / 2, x + pointW, y + h / 2, IBLACK);
        canvas->drawLine(x - w / 2, y - h / 2, x + w / 2, y - h / 2, outline_col);   // top (close the box)
        canvas->drawLine(x - w / 2, y - h / 2, x - w / 2, y + h / 2, outline_col);
        canvas->drawLine(x + w / 2, y - h / 2, x + w / 2, y + h / 2, outline_col);
        canvas->drawLine(x - w / 2, y + h / 2, x - pointW, y + h / 2, outline_col);
        canvas->drawLine(x - pointW, y + h / 2, x, y + h / 2 + pointL, outline_col);
        canvas->drawLine(x + pointW, y + h / 2, x, y + h / 2 + pointL, outline_col);
        canvas->drawLine(x + w / 2, y + h / 2, x + pointW, y + h / 2, outline_col);
        printCentered(canvas, str, x, y);
        break;
      }
  }
}

float battery_voltage = 4.2;

void drawTurnCoordinator(MyCanvas8 *canvas, float x, float y, float r1, float r2, float range, float angle) {
  canvas->fillCircle(x, y, r2 - 1, IWHITE);
  canvas->fillCircle(x, y, r1 - 1, IBLACK);
  float r = (r1 + r2) / 2.0;
  float c = cos(range);
  float s = sin(range);
  canvas->fillRect(0, y - r2, canvas->width(), r2 + r1 * c, IBLACK);
  canvas->fillTriangle(x + r1 * s, y + r1 * c, x + (r2 + 2) * s, y + (r2 + 2) * c, x + r2 / s, y + r1 * c, IBLACK);
  canvas->fillTriangle(x - r1 * s, y + r1 * c, x - (r2 + 2) * s, y + (r2 + 2) * c, x - r2 / s, y + r1 * c, IBLACK);
  canvas->fillCircle(x + r * s, y + r * c, 0.5 * (r2 - r1), IWHITE);
  canvas->fillCircle(x - r * s, y + r * c, 0.5 * (r2 - r1), IWHITE);
  if (angle > range)
    angle = range;
  if (angle < -range)
    angle = -range;
  c = cos(angle);
  s = sin(angle);
  canvas->fillCircle(x + r * s, y + r * c, 0.5 * (r2 - r1) - 1, IBLACK);
  canvas->drawFastVLine(x - (r2 - r1) * 0.5, y, r2 + 2, IBLACK);
  canvas->drawFastVLine(x + (r2 - r1) * 0.5, y, r2 + 2, IBLACK);
}

#if BATT_VIA_PMIC
// LilyGO (BOARD_D): sample the SY6970 PMIC battery ADC over I2C. This touches the
// shared sensor bus, so it MUST run on the sensor task (sensorTask) — never from a
// draw task. The draw path reads the cached value via readBatteryVoltage().
void updateBatteryPMIC() {
  float v = sy6970ReadVbat();
  if (v > 0.0f) battery_voltage = battery_voltage * (1 - ALPHA_BATT) + ALPHA_BATT * v;
}
#endif

// Latest smoothed cell voltage. On ADC boards the divider read is bus-free, so we
// sample it here on demand; on BOARD_D the value is refreshed by the sensor task
// (updateBatteryPMIC) and we just return the cached reading (no I2C from a draw task).
float readBatteryVoltage() {
#if !BATT_VIA_PMIC
  float v = analogRead(BATT_ADC_PIN) * BATT_ADC_SCALE;
  battery_voltage = battery_voltage * (1 - ALPHA_BATT) + ALPHA_BATT * v;
#endif
  return battery_voltage;
}

// Map smoothed cell voltage to a rough state-of-charge fraction (Li-ion curve).
// Below 3.3 V the pack is critically low, so we latch the system off.
float batteryPercentage() {
  float v = readBatteryVoltage();
  if (v > 4.1) return 1.00f;
  if (v > 4.0) return 0.93f;
  if (v > 3.9) return 0.84f;
  if (v > 3.8) return 0.75f;
  if (v > 3.7) return 0.64f;
  if (v > 3.6) return 0.52f;
  if (v > 3.5) return 0.22f;
  if (v > 3.4) return 0.09f;
  if (v > 3.3) return 0.00f;
  digitalWrite(SYS_EN, LOW);   // critically low — power down
  return 0.00f;
}

// Initial great-circle bearing (deg, 0 = North) from the current GPS position
// to the first-fix "home" location. Returns -1 if unavailable (no fix, no home
// set yet, or essentially at home so the bearing would be meaningless).
float bearingToHome(state *s) {
  if (!s->GPS) return -1.0f;
  if (s->home_lat == 0.0f && s->home_lon == 0.0f) return -1.0f;
  if (fabsf(s->home_lat - s->lat) < 1e-4f && fabsf(s->home_lon - s->lon) < 1e-4f)
    return -1.0f;                              // within ~10 m of home
  float lat1 = s->lat * p / 180.0f;
  float lat2 = s->home_lat * p / 180.0f;
  float dLon = (s->home_lon - s->lon) * p / 180.0f;
  float y = sin(dLon) * cos(lat2);
  float x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  float b = atan2(y, x) * 180.0f / p;
  if (b < 0) b += 360.0f;
  return b;
}

// Great-circle distance (metres) from the current GPS position to "home".
// Returns -1 if unavailable. (For the ND map + any range readout.)
float distanceToHome(state *s) {
  if (!s->GPS) return -1.0f;
  if (s->home_lat == 0.0f && s->home_lon == 0.0f) return -1.0f;
  float lat1 = s->lat * p / 180.0f;
  float lat2 = s->home_lat * p / 180.0f;
  float dLat = (s->home_lat - s->lat) * p / 180.0f;
  float dLon = (s->home_lon - s->lon) * p / 180.0f;
  float a = sin(dLat/2)*sin(dLat/2) + cos(lat1)*cos(lat2)*sin(dLon/2)*sin(dLon/2);
  return 6371000.0f * 2.0f * atan2(sqrt(a), sqrt(1.0f - a));
}

// Large "cut-off circle" heading arc across the bottom of the PFD. The circle
// center sits far below the screen, so only the top sweep shows as a curved
// band; the middle is left empty. The rose scrolls with `heading`:
//   - white arc + ticks every 5 deg (longer every 10)
//   - 30-deg labels (N/E/S/W + tens-of-degrees) rotated to follow the arc,
//     drawn via the MyCanvas8 rotation matrix (text-only; ticks/lines are
//     positioned directly so they don't depend on the matrix)
//   - cyan forward index at the apex (fixed, = current heading)
//   - magenta line at the GPS ground track (course over ground)
void drawHeadingArc(MyCanvas8 *canvas, state *s) {
  int   width   = canvas->width();
  int   height  = canvas->height();
  float cx      = width / 2.0f;
  float R       = 0.60f * width;          // smaller circle -> tighter, more compact arc
  // Sit the arc just below the turn-coordinator / slip band (outer ring bottom
  // at midPointY + (stdW1 + 17)). triTop is the index triangle's top edge; keep
  // it ~3 px below the turn coordinator (the gap), and shrink the triangle
  // downward from there.
  float tcBottom = height / 2.0f + (0.3f * height + 17.0f);
  float triTop  = tcBottom + 3.0f;        // top edge of the cyan index triangle (fixed gap)
  float triH    = 5.0f, triW = 4.0f;      // smaller index triangle (was 8 tall / 5 wide)
  float apexY   = triTop + triH;          // arc apex = triangle tip
  float cy      = apexY + R;              // circle center (below the screen)
  float heading = s->IMU ? s->heading : 0.0f;
  float maxA    = acos(clamp((cy - height) / R, -1.0f, 1.0f));  // on-screen half-window (rad)

  canvas->setFont();
  canvas->setTextSize(1);

  // arc outline (chord segments)
  bool have = false; float px = 0, py = 0;
  for (float a = -maxA; a <= maxA + 0.001f; a += 0.07f) {
    float x = cx + R * sin(a), y = cy - R * cos(a);
    if (have) canvas->drawLine(px, py, x, y, IWHITE);
    px = x; py = y; have = true;
  }

  // tick marks every 5 deg (longer every 10)
  for (int b = 0; b < 360; b += 5) {
    float th = b - heading;
    while (th > 180) th -= 360;  while (th < -180) th += 360;
    float a = th * p / 180.0f;
    if (fabsf(a) > maxA) continue;
    float sn = sin(a), cs = cos(a);
    int len = (b % 10 == 0) ? 6 : 3;      // shorter ticks (was 8 / 4)
    canvas->drawLine(cx + sn * R, cy - cs * R,
                     cx + sn * (R - len), cy - cs * (R - len), IWHITE);
  }

  // 30-deg labels, rotated to follow the arc (rotation matrix transforms the
  // glyph pixels; placed at the apex, the matrix swings them to the bearing)
  canvas->setTextColor(IWHITE);
  for (int b = 0; b < 360; b += 30) {
    float th = b - heading;
    while (th > 180) th -= 360;  while (th < -180) th += 360;
    float a = th * p / 180.0f;
    if (fabsf(a) > maxA) continue;
    char num[3]; const char *lbl;
    switch (b) {
      case 0:   lbl = "N"; break;
      case 90:  lbl = "E"; break;
      case 180: lbl = "S"; break;
      case 270: lbl = "W"; break;
      default:  sprintf(num, "%02d", b / 10); lbl = num; break;
    }
    canvas->setRotationMatrix(a, cx, cy);
    printCentered(canvas, lbl, (int)cx, (int)(cy - (R - 11)));   // labels closer to the arc
  }
  canvas->setRotationMatrix();   // back to identity for the lines below

  // ground-track line (magenta) at the GPS course over ground (only when moving)
  if (s->GPS && s->ground_speed > 2.0f) {
    float th = s->ground_track - heading;
    while (th > 180) th -= 360;  while (th < -180) th += 360;
    float a = th * p / 180.0f;
    if (fabsf(a) <= maxA) {
      float sn = sin(a), cs = cos(a);
      canvas->drawLine(cx + sn * (R - 12), cy - cs * (R - 12),
                       cx + sn * (R + 5),  cy - cs * (R + 5), IMAGENTA);
    }
  }

  // home pointer (blue) toward the first-fix location, with a marker at the rim
  float hb = bearingToHome(s);
  if (hb >= 0) {
    float th = hb - heading;
    while (th > 180) th -= 360;  while (th < -180) th += 360;
    float a = th * p / 180.0f;
    if (fabsf(a) <= maxA) {
      float sn = sin(a), cs = cos(a);
      canvas->drawLine(cx + sn * (R - 12), cy - cs * (R - 12),
                       cx + sn * R,        cy - cs * R, IBLUE);
      canvas->fillCircle(cx + sn * (R - 12), cy - cs * (R - 12), 2, IBLUE);
    }
  }

  // fixed forward / current-heading index (cyan, always points up) — smaller
  canvas->drawLine((int)cx, (int)apexY, (int)cx, height - 1, ICYAN);
  canvas->fillTriangle((int)cx, (int)apexY, (int)(cx - triW), (int)triTop, (int)(cx + triW), (int)triTop, ICYAN);
}

// Display-unit conversions. Native sensor units: airspeed=mph, ground speed=knots, alt=ft.
static inline float cvtAsi(float mph, uint8_t u) { return u == 0 ? mph * 0.868976f : u == 2 ? mph * 1.60934f : mph; }
static inline float cvtGs (float kt,  uint8_t u) { return u == 1 ? kt * 1.15078f  : u == 2 ? kt * 1.852f    : kt; }
static inline float cvtAlt(float ft,  uint8_t u) { return u == 1 ? ft * 0.3048f : ft; }
static inline const char *unitAsiStr(uint8_t u) { return u == 0 ? "KT" : u == 2 ? "KMH" : "MPH"; }
static inline const char *unitGsStr (uint8_t u) { return u == 1 ? "MPH" : u == 2 ? "KMH" : "KT"; }
static inline const char *unitAltStr(uint8_t u) { return u == 1 ? "M" : "FT"; }

// Draw the RED warning as a vertical dashed strip of mw-square blocks (mw on, mw off) between
// speeds lo..hi on the airspeed tape's warning strip. Screen-y-aligned so the pattern is stable as
// the tape scrolls. The strip (width mw) is thin enough to always clear the bank-scale arc.
static void speedDash(MyCanvas8 *canvas, int midPointY, int stdW2, int tapeX, int mw,
                      float asiDisp, float pxU, float lo, float hi, uint8_t col) {
  int yA = midPointY - (int)((lo - asiDisp) * pxU);
  int yB = midPointY - (int)((hi - asiDisp) * pxU);
  int yTop = (yB < yA ? yB : yA), yBot = (yB < yA ? yA : yB);
  if (yTop < midPointY - stdW2) yTop = midPointY - stdW2;
  if (yBot > midPointY + stdW2) yBot = midPointY + stdW2;
  if (mw < 1) mw = 1;
  for (int y = yTop; y <= yBot; y++)
    if (!((y / mw) & 1)) canvas->drawFastHLine(tapeX + 1, y, mw, col);   // mw rows red, mw rows gap
}

// Render the full Primary Flight Display into `canvas`, sampling the pre-built
// pitch ladder `inc_map`. All geometry is derived from the canvas size, so it
// scales with the panel resolution.
void drawHorizonDisplay(MyCanvas8 *canvas, GFXcanvas8 *inc_map, state *s, bool showCompass) {
  canvas->setRotationMatrix();   // identity: all PFD drawing is untransformed
                                 // (only the heading-arc labels rotate)
  int width = canvas->width();
  int height = canvas->height();
  canvas->textScale = lyt::txtScale(width);   // bigger panel -> bigger text (1x at the 280 reference)

  int midPointX = width / 2;
  int midPointY = height / 2;
#if COMBINED_DISPLAY
  midPointY -= PFD_SHIFT;   // raise the PFD so its top element sits ~20px down, freeing the
                            // bottom margin for a larger ND (the ND overlaps up into it)
#endif
  int stdW1 = 0.3 * height;
  int stdW2 = 0.25 * height;
  int stdW3 = 0.125 * width;
  int std1 = 0.1 * height;
  int std2 = 0.01 * height;
  int std3 = 0.075 * height;
  int std4 = 0.025 * height;
  int std5 = 0.02 * height;
  int std6 = 0.1 * height;
  int speedSpacing = stdW2 / 5;
  // Roll scale full-scale deflection. The bank ticks below are drawn at
  // 1/6, 1/3, 1/2, 3/4 and 1x of this, i.e. 10/20/30/45/60 deg — the standard
  // attitude-indicator bank marks — so the scale tops out at 60 deg.
  // (The old `acos(stdW2 / stdW1)` was integer division and collapsed to a
  //  meaningless constant.)
  float topAngle = p / 3.0f;

  canvas->setFont();
  canvas->fillScreen(IBLACK);

  canvas->setTextColor(IWHITE);
  canvas->setTextSize(1);

  //draw turn coordinator
  // dedicated slip/turn-coordinator damping (on the accel components, so it
  // never wraps; independent of the attitude smoothing). Tune ALPHA_TURNCOORD.
  static float tcx = 0, tcy = 1;
  tcx = tcx * (1 - ALPHA_TURNCOORD) + ALPHA_TURNCOORD * s->ax;
  tcy = tcy * (1 - ALPHA_TURNCOORD) + ALPHA_TURNCOORD * s->ay;
  drawTurnCoordinator(canvas, midPointX, midPointY, stdW1 + lyt::scaled(6, width),
                      stdW1 + lyt::scaled(17, width), p / 12, atan2(tcx, tcy));

  //draw speed
  float air_speed = 0;
  if (s->ASI)
    air_speed = cvtAsi(s->air_speed, gUnitAsi);

  canvas->drawFastVLine(midPointX - stdW1, midPointY - stdW2, 2 * stdW2, IWHITE);

  int midSpeed = 5 * floor(air_speed / 5);
  for (int n = -5; n <= 5; n++) {
    int this_speed = n * 5 + midSpeed;
    if (this_speed < 0) continue;                       // airspeed can't be negative: no tick/number
    int y = (this_speed - air_speed) * speedSpacing / 5;
    int even = 2 - ((abs(this_speed) / 5) % 2);
    canvas->drawFastHLine(midPointX - stdW1 - std5 * even, midPointY - y, std5 * even, IWHITE);
    if ((abs(this_speed) / 5) % 2 == 0) {
      char tn[6];
      sprintf(tn, "%d", this_speed);
      drawText(canvas, tn, midPointX - stdW1 - std5 * even - lyt::scaled(2, width),
               midPointY - y, lyt::HR, lyt::VC);   // right-aligned against the tape
    }
  }

  char str[6];
  if (s->ASI)
    sprintf(str, "%d", int(air_speed));
  else
    sprintf(str, "N/A");
  canvas->setFont(&FreeMono9pt7b);
  drawArrowNumber(canvas, str, midPointX - stdW1 - 2, midPointY, 37 * lyt::txtScale(width), 33 * lyt::txtScale(width), 0);
  canvas->setFont();

  //draw speed outline
  canvas->fillRect(stdW3, midPointY - stdW2 - stdW3 + 1, midPointX - stdW2 - stdW3, stdW3 + 1, IBLACK);
  canvas->fillRect(stdW3, midPointY + stdW2 - 1, midPointX - stdW2 - stdW3, stdW3 + 1, IBLACK);
  canvas->drawFastHLine(stdW3, midPointY + stdW2, midPointX - stdW2 - stdW3, IWHITE);
  canvas->drawFastHLine(stdW3, midPointY - stdW2, midPointX - stdW2 - stdW3, IWHITE);
  // airspeed unit, just above the tape frame (right-aligned at the tape line)
  { canvas->setFont(); canvas->setTextSize(1); canvas->setTextColor(IGREY);
    drawText(canvas, unitAsiStr(gUnitAsi), midPointX - stdW1, midPointY - stdW2 - lyt::scaled(7, width), lyt::HR, lyt::VC); }

  //draw alt
  float alt = 0;
  if (s->BPS)
    alt = cvtAlt(s->alt, gUnitAlt);

  canvas->drawFastVLine(midPointX + stdW1, midPointY - stdW2, 2 * stdW2, IWHITE);

  int midAlt = 10 * floor(alt / 10);
  for (int n = -6; n <= 8; n++) {
    int this_alt = n * 5 + midAlt;
    int y = (this_alt - alt) * speedSpacing / 7;
    int even = 2 - ((abs(this_alt) / 5) % 2);
    if ((abs(this_alt) / 5) % 4 == 0) {
      even *= 2;
      char tn[6];
      sprintf(tn, "%d", this_alt);
      drawText(canvas, tn, midPointX + stdW1 + std5 * 2 + lyt::scaled(5, width),
               midPointY - y, lyt::HL, lyt::VC);   // left-aligned just right of the tape
    }
    canvas->drawFastHLine(midPointX + stdW1, midPointY - y, 1 + 0.5 * std5 * even, IWHITE);
  }

  if (s->BPS)
    sprintf(str, "%d", int(alt));
  else
    sprintf(str, "N/A");
  canvas->setFont(&FreeMono9pt7b);
  drawArrowNumber(canvas, str, midPointX + stdW1 + lyt::scaled(2, width), midPointY,
                  lyt::scaled(37, width), lyt::scaled(33, width), 1);
  canvas->setFont();

  //draw alt outline
  canvas->fillRect(width - stdW3 - (midPointX - stdW2 - stdW3), midPointY - stdW2 - stdW3 + 1, (midPointX - stdW2), stdW3 + 1, IBLACK);
  canvas->fillRect(width - stdW3 - (midPointX - stdW2 - stdW3), midPointY + stdW2 - 1, (midPointX - stdW2), stdW3 + 1, IBLACK);
  // Positive width: drawFastHLine ignores w<=0, so the old negative-width form
  // never drew these two lines (the speed tape mirror uses positive width).
  canvas->drawFastHLine(width - stdW3 - (midPointX - stdW2 - stdW3), midPointY + stdW2, midPointX - stdW2 - stdW3, IWHITE);
  canvas->drawFastHLine(width - stdW3 - (midPointX - stdW2 - stdW3), midPointY - stdW2, midPointX - stdW2 - stdW3, IWHITE);

  // alt-tape labels, on TOP of the frame. Barometric setting (Kollsman, white) ABOVE:
  { char baro[8]; sprintf(baro, "%.2f", gBaroInHg);
    canvas->setTextColor(IWHITE);
    drawText(canvas, baro, midPointX + stdW1 + lyt::scaled(2, width),
             midPointY - stdW2 - lyt::scaled(7, width), lyt::HL, lyt::VC);
    canvas->setTextColor(IGREY);   // altitude unit, right-aligned left of the alt tape line
    drawText(canvas, unitAltStr(gUnitAlt), midPointX + stdW1 - lyt::scaled(2, width),
             midPointY - stdW2 - lyt::scaled(7, width), lyt::HR, lyt::VC); }
  // GPS altitude (ft) BELOW: a two-line "GPS"/"ALT" label + a divider line, with the
  // value to the right — mirrors the "GS" block under the airspeed tape.
  {
    canvas->setFont(); canvas->setTextSize(1); canvas->setTextColor(IWHITE);
    int by     = midPointY + stdW2 + 4;
    int by2    = by + 10 * lyt::txtScale(width);
    int lineX  = midPointX + stdW1;                      // divider UNDER the alt tape line
    int labelX = lineX - lyt::scaled(20, width);         // "GPS"/"ALT" to the left of it
    canvas->setCursor(labelX, by);  canvas->print("GPS");
    canvas->setCursor(labelX, by2); canvas->print("ALT");
    canvas->drawFastVLine(lineX, by - 1, 19 * lyt::txtScale(width), IGREY);
    char ga[8];
    if (!s->GPS) sprintf(ga, "--"); else sprintf(ga, "%d", (int)s->gps_alt);
    canvas->setCursor(lineX + 4, by2);
    canvas->print(ga);
  }

  // ---- draw horizon (attitude indicator) ----------------------------------
  // The pitch ladder lives in inc_map as a tall strip. For each screen pixel in
  // the circular attitude window we sample inc_map at a position that is rotated
  // by bank and shifted by pitch. The mapping is affine in (x, y), so within a
  // column the texture coords advance by a constant step — we exploit that to
  // make the inner loop two adds and a buffer index, with no per-pixel multiply,
  // circle test, or virtual drawPixel/getPixel call.
  float norm = sqrt(s->gx * s->gx + s->gy * s->gy);
  if (norm > 0)
    norm = 1.0 / norm;
  float myx = s->gx * norm;   // unit gravity ("texture up") basis vector
  float myy = s->gy * norm;
  float mxx = myy;            // perpendicular basis vector
  float mxy = -myx;
  float angle = 4 * stdW1 - (asin(s->gz) * 8.0 * stdW1 / p);   // pitch shift

  // We write straight into the raw framebuffer (no per-pixel virtual drawPixel),
  // so we apply the canvas rotation ourselves. The rotation is read from the
  // canvas, so this fast path stays correct for whatever orientation the panel
  // needs (board A uses rotation 3). Within a column the destination address
  // advances by a constant step; only the per-column base and that step depend
  // on the rotation. Texture-coordinate stepping is in logical space and so is
  // rotation-independent.
  uint8_t *cbuf = canvas->getBuffer();      // raw buffer (native, unrotated)
  uint8_t *mbuf = inc_map->getBuffer();
  int      mw   = inc_map->width();
  int      mh   = inc_map->height();
  int      r2   = stdW1 * stdW1;

#ifdef SVG_RENDER
  extern void svgPreAttitudeHook(MyCanvas8 *);
  svgPreAttitudeHook(canvas);   // snapshot buffer so the SVG gen can diff out the attitude
#endif

  uint8_t rot = canvas->getRotation();
  // Native (constructor) dimensions: odd rotations swap the reported W/H.
  int nativeW = (rot & 1) ? height : width;
  int nativeH = (rot & 1) ? width  : height;
  // Raw-buffer index step as logical y increases by one.
  int dstStep = (rot == 0) ?  nativeW
              : (rot == 1) ? -1
              : (rot == 2) ? -nativeW
                           :  1;            // rot == 3

  for (int x = -stdW2; x <= stdW2; x++) {
    float pxBase = stdW1 - mxx * x;         // texture coords at logical y = 0
    float pyBase = angle - myx * x;

    int ymax = (int)sqrtf((float)(r2 - x * x));   // circle half-height (1 sqrt/col)
    if (ymax > stdW1 - 1) ymax = stdW1 - 1;

    int cx = midPointX + x;
    int ly = midPointY - ymax;              // logical y of the first pixel

    // texture coords at the first pixel, then stepped per row
    float px = pxBase - mxy * (float)(-ymax);
    float py = pyBase - myy * (float)(-ymax);

    // raw-buffer index of logical pixel (cx, ly) for the current rotation
    long base;
    switch (rot) {
      case 0:  base = (long)cx + (long)ly * nativeW; break;
      case 1:  base = (long)(nativeW - 1 - ly) + (long)cx * nativeW; break;
      case 2:  base = (long)(nativeW - 1 - cx) + (long)(nativeH - 1 - ly) * nativeW; break;
      default: base = (long)ly + (long)(nativeH - 1 - cx) * nativeW; break;   // 3
    }
#if BOARD_C
    if (canvas->packed4) {
      // 4-bit packed canvas: write the sampled index into its nibble. The column cx has fixed
      // parity (nativeW even), so it's the HIGH nibble (even x) or LOW (odd x) all the way down.
      // rot is 0 on the combined panel, so dstStep = nativeW (positive).
      uint8_t *cp    = cbuf + (base >> 1);
      int      bstep = dstStep >> 1;
      bool     hi    = !(cx & 1);
      for (int y = -ymax; y <= ymax; y++) {
        int piy = (int)py;
        if (piy < 0) piy = -piy; else if (piy >= mh) piy = 2 * (mh - 1) - piy;
        if (piy < 0) piy = 0; else if (piy >= mh) piy = mh - 1;
        int pix = (int)px;
        uint8_t v = (pix < 0 || pix >= mw) ? IBLACK : mbuf[piy * mw + pix];
        if (hi) *cp = (*cp & 0x0F) | (v << 4);
        else    *cp = (*cp & 0xF0) | (v & 0x0F);
        cp += bstep;
        px -= mxy;
        py -= myy;
      }
    } else
#endif
    {
      uint8_t *crow = cbuf + base;
      for (int y = -ymax; y <= ymax; y++) {
        // reflect py into the texture's vertical bounds (the ladder is mirrored
        // top/bottom about the horizon), then clamp as a safety net
        int piy = (int)py;
        if (piy < 0) piy = -piy;
        else if (piy >= mh) piy = 2 * (mh - 1) - piy;
        if (piy < 0) piy = 0; else if (piy >= mh) piy = mh - 1;

        int pix = (int)px;
        *crow = (pix < 0 || pix >= mw) ? IBLACK : mbuf[piy * mw + pix];

        crow += dstStep;
        px -= mxy;
        py -= myy;
      }
    }
  }

#ifdef SVG_RENDER
  // The attitude above is texture-sampled straight into the buffer (not via GFX
  // primitives), so the SVG generator captures it here as a raster snapshot; the
  // overlays below are recorded as vector shapes. No-op on the device build.
  extern void svgAttitudeHook(MyCanvas8 *);
  svgAttitudeHook(canvas);
#endif

  drawDualRadialLine(canvas, midPointX, midPointY, 0.166 * topAngle, stdW1, stdW1 + 5, IWHITE);
  drawDualRadialLine(canvas, midPointX, midPointY, 0.334 * topAngle, stdW1, stdW1 + 5, IWHITE);
  drawDualRadialLine(canvas, midPointX, midPointY, 0.5 * topAngle, stdW1, stdW1 + 10, IWHITE);
  drawDualRadialLine(canvas, midPointX, midPointY, 0.75 * topAngle, stdW1, stdW1 + 5, IGREY);
  drawDualRadialLine(canvas, midPointX, midPointY, topAngle, stdW1, stdW1 + 5, IGREY);

  float skyW = 3.5 * lyt::txtScale(width), skyH = 7 * lyt::txtScale(width);   // scale with panel
  canvas->drawTriangle(midPointX, midPointY - stdW1, midPointX - skyW, midPointY - stdW1 - skyH, midPointX + skyW, midPointY - stdW1 - skyH, IWHITE);
  drawBankAngleTriangle(canvas, midPointX, midPointY, stdW1, -atan(s->gx / s->gy), topAngle, -topAngle);

  // ---- V-speed markers on the airspeed tape (drawn OVER the attitude, right of the tape line) ----
  // Red warning = a vertical DASHED strip of squares (stall below Vstall / overspeed above Vmax).
  // Yellow caution = a 1-px vertical line centred in the strip (Caution..Max). Green/cyan V1/Vr bugs
  // = same width as the strip, drawn ON TOP, no text. All in the display unit and move with the tape.
  if (s->ASI) {
    int   tapeX = midPointX - stdW1;
    int   mw    = stdW1 * 9 / 100; if (mw < 4) mw = 4;   // thin strip; clears the bank arc
    float pxU   = (float)speedSpacing / 5.0f;            // pixels per unit airspeed
    if (gVStall > 0) speedDash(canvas, midPointY, stdW2, tapeX, mw, air_speed, pxU, 0, gVStall, IRED);      // stall (>=0)
    if (gVMax   > 0) speedDash(canvas, midPointY, stdW2, tapeX, mw, air_speed, pxU, gVMax, gVMax + 2000, IRED); // overspeed
    if (gVCaut > 0 && gVMax > gVCaut) {                  // caution: 1-px vertical line at the strip centre
      int xc = tapeX + mw / 2;
      int yl = midPointY - (int)((gVMax  - air_speed) * pxU);
      int yh = midPointY - (int)((gVCaut - air_speed) * pxU);
      if (yl < midPointY - stdW2) yl = midPointY - stdW2;
      if (yh > midPointY + stdW2) yh = midPointY + stdW2;
      if (yh > yl) canvas->drawFastVLine(xc, yl, yh - yl, IYELLOW);
    }
    struct { float v; uint8_t col; } bugs[2] = { { gV1, ICYAN }, { gVr, IGREEN } };   // bugs on top, no text
    for (int b = 0; b < 2; b++) {
      if (bugs[b].v <= 0) continue;
      int by = midPointY - (int)((bugs[b].v - air_speed) * pxU);
      if (by < midPointY - stdW2 + 1 || by > midPointY + stdW2 - 1) continue;
      canvas->fillRect(tapeX + 1, by - 2, mw, 4, bugs[b].col);
    }
  }


  //draw plane reference
  canvas->fillTriangle(midPointX, midPointY, midPointX - std1, midPointY + std2, midPointX - std3, midPointY + std4, IBLACK);
  canvas->fillTriangle(midPointX, midPointY, midPointX + std1, midPointY + std2, midPointX + std3, midPointY + std4, IBLACK);
  canvas->drawTriangle(midPointX, midPointY, midPointX - std1, midPointY + std2, midPointX - std3, midPointY + std4, IWHITE);
  canvas->drawTriangle(midPointX, midPointY, midPointX + std1, midPointY + std2, midPointX + std3, midPointY + std4, IWHITE);

  canvas->setTextSize(1);

  // Center status box: red "NO IMU" when there's no attitude source, amber
  // "BACKUP" when running on the onboard QMI fallback (BNO lost).
  int isrc = imuSource();
  if (!s->IMU || isrc == 2) {
    const char *msg    = (!s->IMU) ? "NO IMU" : "BACKUP";
    uint8_t     border = (!s->IMU) ? IRED     : IYELLOW;
    canvas->fillRect(midPointX - stdW3, midPointY - std3, 2*stdW3, 2*std3, IGREY);
    canvas->drawRect(midPointX - stdW3 + 3, midPointY - std3 + 3, 2*stdW3 - 6, 2*std3 - 6, border);
    printCentered(canvas, msg, midPointX, midPointY);
  }

  canvas->setTextColor(IGREEN);
#if COMBINED_DISPLAY
  // Green FMA strip: horizontally centered, 5px below the top, clear of the PFD elements.
  drawText(canvas, "FMC SPD   VNAV PTH    LNAV     CMD", midPointX, 5, lyt::HC, lyt::VT);
#else
  canvas->setCursor(stdW3, std1);
  canvas->print("FMC SPD   VNAV PTH    LNAV     CMD");
#endif

  // ---- Speed reference block under the airspeed tape (737-style) ------------
  //   [ Mach ] | GS        ("GS" over the value, hard right against the tape;
  //   [ Mach ] | 142        a divider line; Mach number to the left of it)
  {
    canvas->setFont();
    canvas->setTextSize(1);
    int by    = midPointY + stdW2 + 4;     // top row, just below the speed tape
    int by2   = by + 10 * lyt::txtScale(width);      // bottom row
    int lineX = midPointX - stdW1 + 2;     // divider just right of the airspeed tape line
    char num[8];
    int16_t bx, byy; uint16_t bw, bh;

    canvas->drawFastVLine(lineX, by - 1, 19 * lyt::txtScale(width), IGREY);

    // GS, stacked, to the RIGHT of the divider (all the way right); unit next to the label
    canvas->setTextColor(IWHITE);
    canvas->setCursor(lineX + 4, by);
    canvas->print("GS ");
    canvas->setTextColor(IGREY); canvas->print(unitGsStr(gUnitGs)); canvas->setTextColor(IWHITE);
    if (!s->GPS) sprintf(num, "--");
    else         sprintf(num, "%d", (int)cvtGs(s->ground_speed, gUnitGs));
    canvas->setCursor(lineX + 4, by2);
    canvas->print(num);

    // Mach number to the LEFT of the divider (lower row, clear of the g-meter)
    if (s->ASI) sprintf(num, ".%03d", (int)((s->air_speed / 767.0f) * 1000));  // IAS(mph)/sound(mph)
    else        sprintf(num, "---");
    canvas->getTextBounds(num, 0, 0, &bx, &byy, &bw, &bh);
    canvas->setCursor(lineX - 3 - bw, by2);
    canvas->print(num);
  }

  // ---- Vertical Speed Indicator (thin bar on the far right edge) -----------
  // Pointer position is proportional to climb/descent rate, clamped to
  // +/-gVsiFs ft/sec (center = level). Indentation ticks every VSI_TICK
  // ft/sec, mirroring the g-meter (longer ticks at the even multiples). Tunable
  // in config.h.
  {
    int vsiX   = width - lyt::scaled(4, width);
    int vsiTop = midPointY - stdW2;
    int vsiBot = midPointY + stdW2;
    canvas->drawFastVLine(vsiX, vsiTop, vsiBot - vsiTop, IWHITE);
    // tick marks at 0, +/-10, +/-20 ft/sec, extending inward (left) from the bar;
    // alternating lengths (longer on even multiples: 0 and +/-20) like the g-meter
    for (float t = -gVsiFs; t <= gVsiFs + 0.1f; t += VSI_TICK) {
      int ty   = midPointY - (int)(t / gVsiFs * stdW2);
      int tlen = ((int)(fabsf(t) / VSI_TICK) % 2 == 0) ? lyt::scaled(4, width) : lyt::scaled(2, width);
      canvas->drawFastHLine(vsiX - tlen, ty, tlen, IWHITE);
    }
    float vs  = s->BPS ? clamp(s->vertical_speed / 60.0f, -gVsiFs, gVsiFs) : 0;  // ft/min -> ft/sec
    int   vsY = midPointY - (int)(vs / gVsiFs * stdW2);
    uint8_t vsCol = s->BPS ? IGREEN : IGREY;
    canvas->fillTriangle(vsiX, vsY, vsiX - lyt::scaled(5, width), vsY - lyt::scaled(3, width),
                         vsiX - lyt::scaled(5, width), vsY + lyt::scaled(3, width), vsCol);
  }

  // ---- G-meter (left edge, VSI-style bar: 0 g bottom .. gGmeterFs g top) ----
  {
    int   gmX   = lyt::scaled(4, width);      // far-left edge (mirrors the VSI)
    int   gmTop = midPointY - stdW2;          // gGmeterFs g
    int   gmBot = midPointY + stdW2;          // 0 g
    float fs    = gGmeterFs;
    int   span  = gmBot - gmTop;
    canvas->drawFastVLine(gmX, gmTop, span, IWHITE);
    // integer-g ticks, alternating lengths (longer on even g), extending inward
    for (int gi = 0; gi <= (int)fs; gi++) {
      int ty   = gmBot - (int)(span * gi / fs);
      int tlen = (gi % 2 == 0) ? lyt::scaled(7, width) : lyt::scaled(4, width);
      canvas->drawFastHLine(gmX, ty, tlen, IWHITE);
    }
    float g       = s->IMU ? s->g : 0.0f;
    float maxg    = s->max_g;
    bool  gOver   = g    > fs;
    bool  maxOver = maxg > fs;
    int   gY      = gmBot - (int)(span * clamp(g,    0, fs) / fs);
    int   maxY    = gmBot - (int)(span * clamp(maxg, 0, fs) / fs);
    // max-g marker: yellow line; clamps to the top and turns red once over scale
    canvas->drawFastHLine(gmX, maxY, lyt::scaled(9, width), maxOver ? IRED : IYELLOW);
    // current-g pointer: green; clamps to the top and pulses red once over scale
    uint8_t gCol = s->IMU ? IGREEN : IGREY;
    if (gOver) gCol = (millis() % 500 < 250) ? IRED : IBLACK;   // pulse
    canvas->fillTriangle(gmX, gY, gmX + lyt::scaled(7, width), gY - lyt::scaled(3, width),
                         gmX + lyt::scaled(7, width), gY + lyt::scaled(3, width), gCol);
    canvas->setFont();
    canvas->setTextSize(1);
    char gnum[6];
    // max-g number at the TOP of the bar (matches the max marker color)
    sprintf(gnum, "%.1f", maxg);
    canvas->setTextColor(maxOver ? IRED : IYELLOW);
    canvas->setCursor(gmX, gmTop - 10 * lyt::txtScale(width));
    canvas->print(gnum);
    // real-time current-g number at the BOTTOM of the bar
    sprintf(gnum, "%.1f", g);
    canvas->setTextColor(gOver ? IRED : IWHITE);
    canvas->setCursor(gmX, gmBot + 3);
    canvas->print(gnum);
  }

  // ---- Heading arc (bottom) ------------------------------------------------
  // The COMBINED build (2.8B) puts the compass on the ND directly below the PFD,
  // so the PFD suppresses its own bottom arc and reclaims that space.
  if (showCompass) drawHeadingArc(canvas, s);

/*
  float batt = batteryPercentage();
  uint8_t batt_col = IGREEN;
  if (batt < 0.5)
    batt_col = IYELLOW;
  if (batt < 0.2)
    batt_col = IRED;
  canvas->fillRect(width * 0.75, height * 0.95, width * 0.1 * batt, height * 0.04, batt_col);
  canvas->drawRect(width * 0.75, height * 0.95, width * 0.1, height * 0.04, IGREY);
*/
}

#if MAP_ENABLE
// ============================================================================
//  ND moving-map chart (data in chart_data.h)
//
//  Projects the curated chart features around the aircraft, rotates them to the
//  compass heading (heading-up), scales so MAP_RANGE_M maps to the radar radius,
//  and clips every line/dot to the radar circle. Drawn FIRST in the nav display
//  so the compass card, rings, aircraft, and overlays all sit on top of it.
// ============================================================================
static inline bool mapInCirc(int x, int y, int cx, int cy, long r2) {
  long dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r2;
}

// Cheap reject: does the segment's bbox overlap the radar circle's bbox? (Skips
// the huge off-screen polyline segments so Bresenham never walks them.)
static inline bool mapSegVis(int x0, int y0, int x1, int y1, int cx, int cy, int rad) {
  if (x0 < cx - rad && x1 < cx - rad) return false;
  if (x0 > cx + rad && x1 > cx + rad) return false;
  if (y0 < cy - rad && y1 < cy - rad) return false;
  if (y0 > cy + rad && y1 > cy + rad) return false;
  return true;
}

// Clipped Bresenham line: plots only the pixels inside the radar circle, so any
// feature crossing the rim is cut cleanly. `dashed` skips alternate 4px runs.
static void mapLine(MyCanvas8 *c, int x0, int y0, int x1, int y1, uint8_t col,
                    int cx, int cy, long r2, bool dashed) {
#ifdef SVG_RENDER
  // SVG generator: record one clean vector segment (clipped to the radar circle in
  // SVG) instead of rasterising it pixel-by-pixel.
  { extern void svgVecLine(MyCanvas8 *, int, int, int, int, uint8_t, bool);
    svgVecLine(c, x0, y0, x1, y1, col, dashed); return; }
#endif
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, n = 0;
  for (;;) {
    if ((!dashed || !(n & 4)) && mapInCirc(x0, y0, cx, cy, r2)) {
#if BOARD_C && !defined(SVG_RENDER)
      if (c->packed4 && c->identity) c->putNib(x0, y0, col);   // inline nibble write (skip virtual drawPixel)
      else
#endif
      c->drawPixel(x0, y0, col);
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
    n++;
  }
}

// Project a chart feature (lat/lon) to screen, heading-up + scaled (MapProj in chart_data.h).
static inline void mapXY(const MapProj &m, float lat, float lon, int &sx, int &sy) {
  float dN = (lat - m.clat) * 111320.0f;                 // metres north
  float dE = (lon - m.clon) * 111320.0f * m.cosLat;      // metres east
  float xr = dE * m.cosH - dN * m.sinH;                  // rotate so heading points up
  float yr = dN * m.cosH + dE * m.sinH;
  sx = m.cx + (int)(xr * m.scale);
  sy = m.cy - (int)(yr * m.scale);
}

// ---- label placement: text is ALWAYS drawn (never dropped); each label tries
//      positions around its anchor and takes the first that doesn't collide and
//      stays on the map, else overlaps as a last resort. -----------------------
#define MAP_MAXLBL 64
static int16_t gLblBox[MAP_MAXLBL][4];   // x,y,w,h of placed labels
static int     gLblN = 0;                // reset each labels pass
static bool mapFits(int x, int y, int w, int h) {
  for (int i = 0; i < gLblN; i++) {
    const int16_t *b = gLblBox[i];
    if (x < b[0] + b[2] && x + w > b[0] && y < b[1] + b[3] && y + h > b[1]) return false;
  }
  return true;
}
static void mapClaim(int x, int y, int w, int h) {
  if (gLblN < MAP_MAXLBL) { gLblBox[gLblN][0]=x; gLblBox[gLblN][1]=y; gLblBox[gLblN][2]=w; gLblBox[gLblN][3]=h; gLblN++; }
}
// True if (px,py) lies inside the black masking polygon at the bottom of the ND:
// the bottom rectangle (y >= cyc) plus the two corner wedges drawn under the radar
// (see drawNavigationDisplay). Map text must stay out of it, exactly as the map
// geometry is clipped there, so labels never bleed over the lat/lon / aircraft area.
static inline bool mapBotMask(MyCanvas8 *c, int cx, int cyc, int px, int py) {
  if (py >= cyc) return true;
  float wedge = 0.05f * c->height() * (2.0f * fabsf((float)(px - cx)) / c->width());
  return (float)py >= (float)cyc - wedge;
}
static inline bool mapBoxMasked(MyCanvas8 *c, int cx, int cyc, int x, int y, int w, int h) {
  return mapBotMask(c, cx, cyc, x, y + h) || mapBotMask(c, cx, cyc, x + w, y + h);
}
// The radar circle can be WIDER than the panel (e.g. the dual ND), so the map is
// clipped by the screen edges too. "Inside the circle" is then not enough — a label
// box must also fit on the canvas, and a feature must actually be on-screen, or the
// text floats clipped at the screen edge, detached from anything.
static inline bool mapBoxOnScreen(MyCanvas8 *c, int x, int y, int w, int h) {
  return x >= 0 && y >= 0 && x + w <= c->width() && y + h <= c->height();
}
static inline bool mapPtOnScreen(MyCanvas8 *c, int x, int y) {
  return x >= 0 && y >= 0 && x < c->width() && y < c->height();
}
// Point label: try right / left / above / below the symbol; pick the first that
// fits without colliding, stays on the map (radius^2 r2), and clears the bottom
// mask; never dropped (for a visible symbol the last resort sits above it).
static void mapLabelPt(MyCanvas8 *c, const char *id, const char *info, int sx, int sy, int off,
                       uint8_t col, int cx, int cy, long r2) {
  if (!id || !id[0]) return;
  int s = c->textScale ? c->textScale : 1;
  int h1 = 8 * s, w1 = (int)strlen(id) * 6 * s;
  bool hasInfo = info && info[0];
  int w = w1; if (hasInfo) { int wi = (int)strlen(info) * 6 * s; if (wi > w) w = wi; }
  int h = h1 + (hasInfo ? h1 + 1 : 0);
  int cxs[4] = { sx + off, sx - off - w, sx - w / 2, sx - w / 2 };
  int cys[4] = { sy - h1 / 2, sy - h1 / 2, sy - off - h, sy + off };
  int bx = 0, by = 0, fx = 0, fy = 0; bool haveB = false, haveF = false;
  for (int i = 0; i < 4; i++) {
    int ax = cxs[i], ay = cys[i];
    if (!mapInCirc(ax, ay, cx, cy, r2) || !mapInCirc(ax + w, ay + h, cx, cy, r2)) continue;
    if (mapBoxMasked(c, cx, cy, ax, ay, w, h)) continue;   // keep off the bottom black polygon
    if (!mapBoxOnScreen(c, ax, ay, w, h)) continue;        // keep on the visible screen
    if (!haveF) { fx = ax; fy = ay; haveF = true; }
    if (mapFits(ax, ay, w, h)) { bx = ax; by = ay; haveB = true; break; }
  }
  if (!haveB && haveF) { bx = fx; by = fy; haveB = true; }
  if (!haveB) {   // last resort: above the symbol, clamped on-screen so it never floats at the edge
    bx = sx - w / 2; by = sy - off - h;
    if (bx < 0) bx = 0; else if (bx + w > c->width())  bx = c->width()  - w;
    if (by < 0) by = 0; else if (by + h > c->height()) by = c->height() - h;
  }
  mapClaim(bx, by, w, h);
  c->setTextColor(col); c->setCursor(bx, by); c->print(id);
  if (hasInfo) { c->setTextColor(IGREY); c->setCursor(bx, by + h1 + 1); c->print(info); }
}
// Road label: scan the road, clip each segment to the circle, and label at the
// first visible midpoint that doesn't collide (so a partly-off-screen road is
// named at the best on-screen spot). Never dropped if any part is visible.
static void mapLabelRoad(MyCanvas8 *c, const ChartPoly &P, const MapProj &m, uint8_t col,
                         int cx, int cy, int rad, long r2, long rl2) {
  if (!P.name || !P.name[0]) return;
  int s = c->textScale ? c->textScale : 1;
  int w = (int)strlen(P.name) * 6 * s, h = 8 * s;
  int bx = 0, by = 0, fx = 0, fy = 0; bool haveB = false, haveF = false;
  int q0x = 0, q0y = 0; bool have0 = false;
  for (int k = 0; k < P.n && !haveB; k++) {
    int q1x, q1y; mapXY(m, P.pts[2 * k], P.pts[2 * k + 1], q1x, q1y);
    if (have0 && mapSegVis(q0x, q0y, q1x, q1y, cx, cy, rad)) {
      float ddx = q1x - q0x, ddy = q1y - q0y, A = ddx * ddx + ddy * ddy;
      if (A > 0.5f) {
        float ffx = q0x - cx, ffy = q0y - cy;
        float B = 2 * (ffx * ddx + ffy * ddy), C = ffx * ffx + ffy * ffy - (float)r2;
        float disc = B * B - 4 * A * C;
        if (disc >= 0) {
          disc = sqrtf(disc);
          float t0 = (-B - disc) / (2 * A), t1 = (-B + disc) / (2 * A);
          if (t0 < 0) t0 = 0; if (t1 > 1) t1 = 1;
          if (t0 < t1) {
            float tm = (t0 + t1) * 0.5f;
            int ax = q0x + (int)(ddx * tm) + 2, ay = q0y + (int)(ddy * tm) - h / 2;
            if (mapInCirc(ax, ay, cx, cy, rl2) && mapInCirc(ax + w, ay + h, cx, cy, r2) &&
                !mapBoxMasked(c, cx, cy, ax, ay, w, h) && mapBoxOnScreen(c, ax, ay, w, h)) {   // on-screen, off the mask
              if (!haveF) { fx = ax; fy = ay; haveF = true; }
              if (mapFits(ax, ay, w, h)) { bx = ax; by = ay; haveB = true; }
            }
          }
        }
      }
    }
    q0x = q1x; q0y = q1y; have0 = true;
  }
  if (!haveB && haveF) { bx = fx; by = fy; haveB = true; }
  if (!haveB) return;
  mapClaim(bx, by, w, h);
  c->setTextColor(col); c->setCursor(bx, by); c->print(P.name);
}

#ifdef SVG_RENDER
// Record a legend landmark for a map polyline segment at the point on it closest
// to the radar centre (so a feature that only clips the edge of the view still
// anchors at a visible, on-screen spot — not at a midpoint that may be off-circle).
static void svgLineLM(MyCanvas8 *canvas, const char *name, int px, int py, int qx, int qy, int cx, int cy, long r2) {
  float dx = qx - px, dy = qy - py, len2 = dx * dx + dy * dy;
  float t = len2 > 0 ? (((cx - px) * dx + (cy - py) * dy) / len2) : 0.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  int ax = px + (int)(t * dx), ay = py + (int)(t * dy);
  if (mapInCirc(ax, ay, cx, cy, r2) && !mapBotMask(canvas, cx, cy, ax, ay)) {
    extern void svgLandmark(const char *, int, int);
    svgLandmark(name, ax, ay);
  }
}
#endif

// Latitude lower-bound for the lat-sorted polyline (.la0) and glide-seg (.lat1)
// arrays, so those per-frame loops touch only the visible latitude band (like the
// point/ring loops) instead of linearly scanning the whole LOD.
static inline int chartSegLower(const ChartSeg *a, int n, float v) {
  int lo = 0, hi = n; while (lo < hi) { int m = (lo + hi) >> 1; if (a[m].lat1 < v) lo = m + 1; else hi = m; } return lo;
}
static inline int chartPolyLower(const ChartPoly *a, int n, float v) {
  int lo = 0, hi = n; while (lo < hi) { int m = (lo + hi) >> 1; if (a[m].la0 < v) lo = m + 1; else hi = m; } return lo;
}

// Even-odd scanline fill of a GROUP of closed lat/lon rings (the ocean = box minus
// continent; or all lakes), projected through m and clipped to the radar circle.
// Projects the verts of rings whose bbox overlaps the view into a static buffer,
// then fills row by row. Optionally draws the ring outlines white (lake shores).
// ND-only (single task) so the static buffers are safe.
// Fill scratch lives in the PSRAM HEAP (allocated once) so it never competes with the
// PFD canvas for scarce internal RAM. Active-edge-table buffers: edges bucketed by top
// scan-row so the fill is O(edges + rows*active), not O(rows*edges) — the whole-US
// coastline ring is otherwise far too costly to brute-force every frame.
#define FILL_V 2048      // max projected vertices in view
#define FILL_E 2560      // max edges
static int16_t  *gFillX, *gFillY, *gE_ytop, *gE_ybot, *gE_xtop, *gE_next, *gBucket;
static uint16_t *gRingStart, *gActive;
static float    *gE_dxdy;
static bool fillBufAlloc() {
  static int8_t st = 0;                                   // 0 untried, 1 ok, -1 failed
  if (st) return st > 0;
  gFillX = (int16_t *)ps_malloc(FILL_V * 2); gFillY = (int16_t *)ps_malloc(FILL_V * 2);
  gE_ytop = (int16_t *)ps_malloc(FILL_E * 2); gE_ybot = (int16_t *)ps_malloc(FILL_E * 2);
  gE_xtop = (int16_t *)ps_malloc(FILL_E * 2); gE_next = (int16_t *)ps_malloc(FILL_E * 2);
  gE_dxdy = (float *)ps_malloc(FILL_E * 4);
  gBucket = (int16_t *)ps_malloc(700 * 2); gActive = (uint16_t *)ps_malloc(512 * 2);
  gRingStart = (uint16_t *)ps_malloc(128 * 2);
  st = (gFillX && gFillY && gE_ytop && gE_ybot && gE_xtop && gE_next && gE_dxdy &&
        gBucket && gActive && gRingStart) ? 1 : -1;
  return st > 0;
}
static void mapFillGroup(MyCanvas8 *canvas, const ChartFill *fills, int nf,
                         const MapProj &m, uint8_t col, bool outline,
                         int cx, int cy, int rad, long r2,
                         float clat, float clon, float dLatMax, float dLonMax) {
  if (!fillBufAlloc()) return;
  int nv = 0, nr = 0, ymin = cy + rad, ymax = cy - rad;
  for (int f = 0; f < nf && nr < 127; f++) {
    const ChartFill &F = fills[f];
    if (F.n < 3) continue;
    if (F.la1 < clat - dLatMax || F.la0 > clat + dLatMax ||
        F.lo1 < clon - dLonMax || F.lo0 > clon + dLonMax) continue;   // ring off-screen
    if (nv + F.n > FILL_V - 1) break;                                 // buffer full
    gRingStart[nr++] = nv;
    for (int k = 0; k < F.n; k++) {
      int x, y; mapXY(m, F.pts[2 * k], F.pts[2 * k + 1], x, y);
      gFillX[nv] = x; gFillY[nv] = y; nv++;
      if (y < ymin) ymin = y;
      if (y > ymax) ymax = y;
    }
  }
  gRingStart[nr] = nv;
  if (nr == 0) return;
  if (ymin < cy - rad) ymin = cy - rad;
  if (ymax > cy + rad) ymax = cy + rad;
  if (ymin < 0) ymin = 0;                                           // clamp to the canvas: the radar
  if (ymax > canvas->height() - 1) ymax = canvas->height() - 1;     // circle can extend off-screen
  // build edges into per-row buckets (active-edge-table fill)
  int H = ymax - ymin; if (H < 0) return; if (H > 698) H = 698;
  for (int i = 0; i <= H; i++) gBucket[i] = -1;
  int ne = 0;
  for (int r = 0; r < nr && ne < FILL_E - 6; r++) {
    int s = gRingStart[r], e = gRingStart[r + 1];
    for (int i = s; i < e && ne < FILL_E - 6; i++) {
      int a = i, b = (i + 1 < e) ? i + 1 : s;
      int ya = gFillY[a], yb = gFillY[b];
      if (ya == yb) continue;                                        // horizontal edge: no crossing
      int xa = gFillX[a], xb = gFillX[b], yt, yo, xt; float dd;
      if (ya < yb) { yt = ya; yo = yb; xt = xa; dd = (xb - xa) / (float)(yb - ya); }
      else         { yt = yb; yo = ya; xt = xb; dd = (xa - xb) / (float)(ya - yb); }
      if (yo < ymin || yt > ymax) continue;                          // edge entirely off the band
      int br = yt - ymin; if (br < 0) br = 0;
      gE_ytop[ne] = yt; gE_ybot[ne] = yo; gE_xtop[ne] = xt; gE_dxdy[ne] = dd;
      gE_next[ne] = gBucket[br]; gBucket[br] = ne; ne++;
    }
  }
  int na = 0;
  for (int y = ymin, ry = 0; y <= ymax; y++, ry++) {
    if (ry <= H) for (int e = gBucket[ry]; e != -1; e = gE_next[e]) if (na < 512) gActive[na++] = e;
    int xs[256], nx = 0, w = 0;
    for (int k = 0; k < na; k++) {                                   // drop finished edges + read crossings
      int e = gActive[k];
      if (gE_ybot[e] <= y) continue;
      gActive[w++] = e;
      if (nx < 256) xs[nx++] = gE_xtop[e] + (int)((y - gE_ytop[e]) * gE_dxdy[e]);
    }
    na = w;
    for (int i = 1; i < nx; i++) { int v = xs[i], j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
    long dy = (long)y - cy, h2 = r2 - dy * dy;                        // span clip to the radar circle
    if (h2 < 0) continue;
    int hw = (int)sqrtf((float)h2);
    for (int k = 0; k + 1 < nx; k += 2) {
      int x0 = xs[k], x1 = xs[k + 1];
      if (x0 < cx - hw) x0 = cx - hw;
      if (x1 > cx + hw) x1 = cx + hw;
      if (x1 >= x0) canvas->drawFastHLine(x0, y, x1 - x0 + 1, col);
    }
  }
  if (outline)
    for (int r = 0; r < nr; r++) {
      int s = gRingStart[r], e = gRingStart[r + 1];
      for (int i = s; i < e; i++) {
        int a = i, b = (i + 1 < e) ? i + 1 : s;
        if (mapSegVis(gFillX[a], gFillY[a], gFillX[b], gFillY[b], cx, cy, rad))
          mapLine(canvas, gFillX[a], gFillY[a], gFillX[b], gFillY[b], IWHITE, cx, cy, r2, false);
      }
    }
}

static void drawChart(MyCanvas8 *canvas, int cx, int cy, int rad,
                      float clat, float clon, float headingDeg,
                      int rangeM, int lod) {
  canvas->setRotationMatrix();          // identity: the projection does the rotation
#ifdef SVG_RENDER
  { extern void svgSetClip(int, int, int); svgSetClip(cx, cy, rad); }   // map clip circle
#endif
  const int  width = canvas->width();
  const int  sc    = lyt::txtScale(width);
  const long r2    = (long)rad * rad;
  const long rl2   = (long)(rad * 82 / 100) * (rad * 82 / 100);  // inner radius^2: keep labels off the rim

  MapProj m;
  m.clat = clat; m.clon = clon; m.cx = cx; m.cy = cy;
  m.cosLat = cosf(clat * p / 180.0f);
  float h = headingDeg * p / 180.0f;
  m.cosH = cosf(h); m.sinH = sinf(h);
  m.scale = (float)rad / rangeM;

  // Cheap "grid" cull: reject by a lat/lon bbox (a bit beyond the range) before
  // any trig, so far-off features (e.g. when the dataset is extended) cost ~nothing.
  const float dLatMax = (rangeM * 1.5f) / 111320.0f;
  const float dLonMax = dLatMax / (m.cosLat > 0.1f ? m.cosLat : 0.1f);
  #define MAP_CULL(la, lo) (fabsf((la) - clat) > dLatMax || fabsf((lo) - clon) > dLonMax)
  #define MAP_CULL_POLY(P) ((P).la1 < clat - dLatMax || (P).la0 > clat + dLatMax || \
                            (P).lo1 < clon - dLonMax || (P).lo0 > clon + dLonMax)

  int sx, sy, ex, ey;

  // ---- filled water FIRST (under everything): oceans + lakes, dim blue. Lakes get a
  // white shore outline; the ocean's white shore is the coastline polyline below.
  // Ocean only at the zoomed-out LODs: its continent ring spans the whole US so it
  // can't cull, and at close zoom there's rarely ocean in view anyway (white shore
  // delineates it). Lakes cull by bbox and stay at every LOD.
  if (lod <= 2)
    mapFillGroup(canvas, LOD_OCEAN[lod], LOD_NOCEAN[lod], m, IDBLUE, false, cx, cy, rad, r2, clat, clon, dLatMax, dLonMax);
  mapFillGroup(canvas, LOD_LAKE[lod],  LOD_NLAKE[lod],  m, IDBLUE, true,  cx, cy, rad, r2, clat, clon, dLatMax, dLonMax);

  // ---- base geography: white coast/borders, state lines, rivers, roads (polylines) -
  const ChartPoly *POLYS = LOD_POLYS[lod];
  for (int i = chartPolyLower(POLYS, LOD_NPOLYS[lod], clat - dLatMax - 2.0f), n = LOD_NPOLYS[lod];
       i < n && POLYS[i].la0 <= clat + dLatMax; i++) {
    const ChartPoly &P = POLYS[i];
    if (MAP_CULL_POLY(P)) continue;
    uint8_t col = P.type == PLY_INTERSTATE ? IDGREY :    // roads dark grey (vs the IGREY rings)
                  P.type == PLY_RIVER      ? ISKY   :    // rivers light blue
                  P.type == PLY_RESTRICTED ? IRED   :    // restricted/prohibited airspace red
                  P.type == PLY_STATE      ? IGREY  : IWHITE;  // state grey (dashed); BORDER = coast/country white
    bool dash = (P.type == PLY_STATE || P.type == PLY_RESTRICTED);
    int px = 0, py = 0; bool have = false;
    for (int k = 0; k < P.n; k++) {
      int qx, qy; mapXY(m, P.pts[2 * k], P.pts[2 * k + 1], qx, qy);
      if (have && mapSegVis(px, py, qx, qy, cx, cy, rad)) {
        mapLine(canvas, px, py, qx, qy, col, cx, cy, r2, dash);
#ifdef SVG_RENDER
        svgLineLM(canvas, P.type==PLY_RIVER?"river":P.type==PLY_INTERSTATE?"road":P.type==PLY_HIGHWAY?"road":
                  P.type==PLY_STATE?"state":"border", px, py, qx, qy, cx, cy, r2);
#endif
      }
      px = qx; py = qy; have = true;
    }
  }

  // ---- regional medium roads (OSM secondary/tertiary), close-in only (<= "5NM" =
  // the 10 km level, which the range readout rounds to 5 NM) ----
  if (rangeM <= 10000)
    for (int i = chartPolyLower(MEDROADS, N_MEDROADS, clat - dLatMax - 0.4f), n = N_MEDROADS;
         i < n && MEDROADS[i].la0 <= clat + dLatMax; i++) {
      const ChartPoly &P = MEDROADS[i];
      if (MAP_CULL_POLY(P)) continue;
      int px = 0, py = 0; bool have = false;
      for (int k = 0; k < P.n; k++) {
        int qx, qy; mapXY(m, P.pts[2 * k], P.pts[2 * k + 1], qx, qy);
        if (have && mapSegVis(px, py, qx, qy, cx, cy, rad))
          mapLine(canvas, px, py, qx, qy, IMROAD, cx, cy, r2, false);
        px = qx; py = qy; have = true;
      }
    }

  // ---- rivers (light blue), coast (tan), glide paths (dashed green) ---------
  const ChartSeg *SEGS = LOD_SEGS[lod];
  for (int i = chartSegLower(SEGS, LOD_NSEGS[lod], clat - dLatMax - 0.3f), n = LOD_NSEGS[lod];
       i < n && SEGS[i].lat1 <= clat + dLatMax + 0.3f; i++) {
    const ChartSeg &s = SEGS[i];
    if (MAP_CULL(s.lat1, s.lon1) && MAP_CULL(s.lat2, s.lon2)) continue;
    mapXY(m, s.lat1, s.lon1, sx, sy);
    mapXY(m, s.lat2, s.lon2, ex, ey);
    if (!mapSegVis(sx, sy, ex, ey, cx, cy, rad)) continue;
#ifdef SVG_RENDER
    svgLineLM(canvas, s.type==SG_RIVER?"river":s.type==SG_COAST?"coast":"glidepath", sx, sy, ex, ey, cx, cy, r2);
#endif
    uint8_t scol = IYELLOW; bool sdash = true;                      // SG_GLIDEPATH (yellow)
    if      (s.type == SG_RIVER) { scol = ISKY; sdash = false; }    // rivers light blue
    else if (s.type == SG_COAST) { scol = IGND; sdash = false; }    // coast tan
    mapLine(canvas, sx, sy, ex, ey, scol, cx, cy, r2, sdash);
  }

  // ---- distance circles around airports (bright, color by size/status) + restricted
  // NOT culled by the center bbox: a ring whose center is off the panel still draws
  // if its circle reaches into the radar view (center distance <= rad + ringRadius).
  const ChartRing *RINGS = LOD_RINGS[lod];
  CHART_GRID_BEGIN(lod, 1)                          // +1 cell: a ring reaches past its center
  CHART_GRID_ROWS(LOD_RINGS_CELL[lod], i) {
    const ChartRing &r = RINGS[i];
    int rr = (int)(r.rad_m * m.scale);
    if (rr < 2) continue;
    int rcx, rcy; mapXY(m, r.lat, r.lon, rcx, rcy);
    long ddx = rcx - cx, ddy = rcy - cy, reach = (long)rad + rr;
    if (ddx * ddx + ddy * ddy > reach * reach) continue;   // circle can't reach the view
#ifdef SVG_RENDER
    if (mapInCirc(rcx, rcy, cx, cy, r2) && !mapBotMask(canvas, cx, cy, rcx, rcy)) { extern void svgLandmark(const char *, int, int);
      svgLandmark(r.type==RG_APT_LARGE?"ring_lg":r.type==RG_APT_MEDIUM?"ring_md":
                  r.type==RG_APT_SMALL?"ring_sm":r.type==RG_APT_CLOSED?"ring_cl":"ring_rstr", rcx, rcy); }
#endif
    uint8_t col = r.type == RG_APT_LARGE  ? ICYAN   :      // large = bright cyan
                  r.type == RG_APT_MEDIUM ? IGREEN  :      // medium = bright green
                  r.type == RG_APT_SMALL  ? IYELLOW :      // small = bright yellow
                  r.type == RG_APT_CLOSED ? IGREY   : IRED; // closed = grey, restricted = red
#ifdef SVG_RENDER
    // SVG generator: one clean dashed circle (clipped to the radar circle) instead
    // of the rasterised dotted ring.
    { extern void svgVecRingDashed(MyCanvas8 *, int, int, int, uint8_t); svgVecRingDashed(canvas, rcx, rcy, rr, col); }
#else
    int npt = rr; if (npt < 24) npt = 24; if (npt > 140) npt = 140;
    float ca = cosf(2.0f * p / npt), sa = sinf(2.0f * p / npt);
    float vx = -rr * m.sinH, vy = -rr * m.cosH;   // anchored to geographic north
    for (int k = 0; k < npt; k++) {
      int px = rcx + (int)vx, py = rcy + (int)vy;
      if (mapInCirc(px, py, cx, cy, r2)) canvas->drawPixel(px, py, col);
      float nx = vx * ca - vy * sa; vy = vx * sa + vy * ca; vx = nx;
    }
#endif
  }
  CHART_GRID_END

  // ---- runways (short white lines at the true heading) ----------------------
  const ChartRwy *RWYS = LOD_RWYS[lod];
  CHART_GRID_BEGIN(lod, 0)
  CHART_GRID_ROWS(LOD_RWYS_CELL[lod], i) {
    const ChartRwy &r = RWYS[i];
    if (MAP_CULL(r.lat, r.lon)) continue;
    float hr = r.hdg * p / 180.0f, half = r.len_m / 2.0f;
    float dN = half * cosf(hr), dE = half * sinf(hr);
    mapXY(m, r.lat + dN / 111320.0f, r.lon + dE / (111320.0f * m.cosLat), sx, sy);
    mapXY(m, r.lat - dN / 111320.0f, r.lon - dE / (111320.0f * m.cosLat), ex, ey);
    mapLine(canvas, sx, sy, ex, ey, IWHITE, cx, cy, r2, false);
  }
  CHART_GRID_END

  // ---- point symbols: airports, navaids, landmarks --------------------------
  canvas->setFont();
  canvas->setTextSize(1);
  const ChartPt *PTS = LOD_PTS[lod];
  CHART_GRID_BEGIN(lod, 0)
  CHART_GRID_ROWS(LOD_PTS_CELL[lod], i) {
    const ChartPt &pt = PTS[i];
    if (MAP_CULL(pt.lat, pt.lon)) continue;
    mapXY(m, pt.lat, pt.lon, sx, sy);
    if (!mapInCirc(sx, sy, cx, cy, r2)) continue;
#ifdef SVG_RENDER
    if (!mapBotMask(canvas, cx, cy, sx, sy)) { extern void svgLandmark(const char *, int, int);
      svgLandmark(pt.type==PT_AIRPORT_TWR?"apt_twr":pt.type==PT_AIRPORT_NTWR?"apt_ntwr":
                  pt.type==PT_NAVAID?"navaid":pt.type==PT_AIRPORT_CLOSED?"apt_closed":"city", sx, sy); }
#endif
    int rsym = 2 * sc;
    switch (pt.type) {
      case PT_AIRPORT_TWR:  canvas->drawCircle(sx, sy, rsym, IBLUE);    canvas->drawPixel(sx, sy, IBLUE);    break;
      case PT_AIRPORT_NTWR: canvas->drawCircle(sx, sy, rsym, IMAGENTA); canvas->drawPixel(sx, sy, IMAGENTA); break;
      case PT_NAVAID:
        canvas->drawLine(sx, sy - rsym, sx + rsym, sy, IBLUE); canvas->drawLine(sx + rsym, sy, sx, sy + rsym, IBLUE);
        canvas->drawLine(sx, sy + rsym, sx - rsym, sy, IBLUE); canvas->drawLine(sx - rsym, sy, sx, sy - rsym, IBLUE); break;
      case PT_AIRPORT_CLOSED:   // decommissioned airfield: grey X
        canvas->drawLine(sx - rsym, sy - rsym, sx + rsym, sy + rsym, IGREY);
        canvas->drawLine(sx - rsym, sy + rsym, sx + rsym, sy - rsym, IGREY); break;
      case PT_CITY:         canvas->fillCircle(sx, sy, sc, IWHITE); break;        // city: small white dot
      default:              canvas->fillRect(sx - sc, sy - sc, 2 * sc, 2 * sc, IYELLOW); break;  // PT_LANDMARK
    }
  }
  CHART_GRID_END

  // NOTE: map text is NOT drawn here. drawChartLabels() runs as a separate pass
  // AFTER the compass rose/rings/overlays so labels always sit on top and stay
  // readable. Leaving GFX text size at `sc` here keeps the compass numbers full size.
  canvas->setTextColor(IWHITE);
  #undef MAP_CULL
  #undef MAP_CULL_POLY
}

#if MAP_LABELS
// Second chart pass: all map text. Drawn on top of the compass so labels are
// always visible and readable. Priority: airports/navaids (id + freq), then
// cities, then road names. Every label is placed (never dropped) by trying
// candidate positions that avoid collisions and stay on the map.
static void drawChartLabels(MyCanvas8 *canvas, int cx, int cy, int rad,
                            float clat, float clon, float headingDeg,
                            int rangeM, int lod) {
  const int  width = canvas->width();
  const int  sc    = lyt::txtScale(width);
  const long r2    = (long)rad * rad;
  const long rl2   = (long)(rad * 82 / 100) * (rad * 82 / 100);  // inner radius^2: keep labels off the rim

  MapProj m;
  m.clat = clat; m.clon = clon; m.cx = cx; m.cy = cy;
  m.cosLat = cosf(clat * p / 180.0f);
  float hh = headingDeg * p / 180.0f;
  m.cosH = cosf(hh); m.sinH = sinf(hh);
  m.scale = (float)rad / rangeM;

  const float dLatMax = (rangeM * 1.5f) / 111320.0f;
  const float dLonMax = dLatMax / (m.cosLat > 0.1f ? m.cosLat : 0.1f);
  #define MAP_CULL(la, lo) (fabsf((la) - clat) > dLatMax || fabsf((lo) - clon) > dLonMax)
  #define MAP_CULL_POLY(P) ((P).la1 < clat - dLatMax || (P).la0 > clat + dLatMax || \
                            (P).lo1 < clon - dLonMax || (P).lo0 > clon + dLonMax)

  int sx, sy;
  gLblN = 0;
  canvas->setFont();
  canvas->textScale = 1; canvas->setTextSize(1);   // all map text is small (size 1)

  // 1) airports + navaids: id + grey frequency line --------------------------
  const ChartPt *PTS = LOD_PTS[lod];
  CHART_GRID_BEGIN(lod, 0)
  CHART_GRID_ROWS(LOD_PTS_CELL[lod], i) {
    const ChartPt &pt = PTS[i];
    if (pt.type == PT_LANDMARK || pt.type == PT_CITY || MAP_CULL(pt.lat, pt.lon)) continue;
    mapXY(m, pt.lat, pt.lon, sx, sy);
    if (!mapInCirc(sx, sy, cx, cy, r2) || mapBotMask(canvas, cx, cy, sx, sy) ||
        !mapPtOnScreen(canvas, sx, sy)) continue;          // off-screen symbol -> no floating label
    mapLabelPt(canvas, pt.id, pt.info, sx, sy, 2 * sc + 2,
               pt.type == PT_AIRPORT_CLOSED ? IGREY : pt.type == PT_AIRPORT_NTWR ? IMAGENTA : IBLUE,
               cx, cy, rl2);
  }
  CHART_GRID_END
  // 2) cities / landmarks: id only -------------------------------------------
  CHART_GRID_BEGIN(lod, 0)
  CHART_GRID_ROWS(LOD_PTS_CELL[lod], i) {
    const ChartPt &pt = PTS[i];
    if ((pt.type != PT_LANDMARK && pt.type != PT_CITY) || MAP_CULL(pt.lat, pt.lon)) continue;
    mapXY(m, pt.lat, pt.lon, sx, sy);
    if (!mapInCirc(sx, sy, cx, cy, r2) || mapBotMask(canvas, cx, cy, sx, sy) ||
        !mapPtOnScreen(canvas, sx, sy)) continue;          // off-screen symbol -> no floating label
    mapLabelPt(canvas, pt.id, "", sx, sy, sc + 2, pt.type == PT_CITY ? IWHITE : IYELLOW, cx, cy, rl2);
  }
  CHART_GRID_END
  // 3) road / highway names: best visible spot along the road ----------------
  // mapLabelRoad clips each segment to the circle, so a road that runs partly
  // off-screen is still named at its best on-screen position.
  const ChartPoly *POLYS = LOD_POLYS[lod];
  for (int i = chartPolyLower(POLYS, LOD_NPOLYS[lod], clat - dLatMax - 2.0f), nP = LOD_NPOLYS[lod];
       i < nP && POLYS[i].la0 <= clat + dLatMax; i++) {
    const ChartPoly &P = POLYS[i];
    if (MAP_CULL_POLY(P) || !P.name || !P.name[0]) continue;
    mapLabelRoad(canvas, P, m, IWHITE, cx, cy, rad, r2, rl2);
  }

  canvas->textScale = sc; canvas->setTextSize(1);   // restore full-size text for whatever draws next
  canvas->setTextColor(IWHITE);
  #undef MAP_CULL
  #undef MAP_CULL_POLY
}
#endif // MAP_LABELS
#endif // MAP_ENABLE

// Draw the user flight plan (FlightPlan.ino) over the moving map: a solid YELLOW route line
// between waypoints, a yellow marker at each, and each waypoint's name beside its marker —
// heading-up, projected + clipped to the radar circle exactly like the chart.
static void drawFlightPlan(MyCanvas8 *canvas, int cx, int cy, int rad,
                           float clat, float clon, float headingDeg, int rangeM) {
  int nwp = fplanCount();
  if (nwp < 1) return;
  canvas->setRotationMatrix();                 // identity: the projection does the rotation
  const long r2 = (long)rad * rad;
  const int  sc = lyt::txtScale(canvas->width());
  MapProj m; m.clat = clat; m.clon = clon; m.cx = cx; m.cy = cy;
  m.cosLat = cosf(clat * p / 180.0f);
  float h = headingDeg * p / 180.0f; m.cosH = cosf(h); m.sinH = sinf(h);
  m.scale = (float)rad / rangeM;
  // route line — SOLID + THICK. A single-pixel diagonal Bresenham line only touches at the
  // corners and reads as "dashed" on the panel, so draw the segment 5x on a +-shaped offset
  // (a 3-px-wide solid band). Each pass is clipped to the radar circle by mapLine (dashed=false).
  static const int OFX[5] = { 0, 1, -1, 0, 0 }, OFY[5] = { 0, 0, 0, 1, -1 };
  int px = 0, py = 0;
  for (int i = 0; i < nwp; i++) {
    float la, lo; char nm[10]; fplanGet(i, &la, &lo, nm);
    int sx, sy; mapXY(m, la, lo, sx, sy);
    if (i) for (int o = 0; o < 5; o++)
      mapLine(canvas, px + OFX[o], py + OFY[o], sx + OFX[o], sy + OFY[o], IYELLOW, cx, cy, r2, false);
    px = sx; py = sy;
  }
  // markers + names on top: a filled yellow circle (radius mr) at each waypoint + its name
  canvas->setFont(); canvas->setTextSize(1); canvas->setTextColor(IYELLOW);
  const int mr = 2 + sc;                        // ~4 px on the wide panel
  for (int i = 0; i < nwp; i++) {
    float la, lo; char nm[10]; fplanGet(i, &la, &lo, nm);
    int sx, sy; mapXY(m, la, lo, sx, sy);
    if (!mapInCirc(sx, sy, cx, cy, r2)) continue;
    for (int dy = -mr; dy <= mr; dy++)          // filled disc via drawPixel (safe on the 4-bit canvas)
      for (int dx = -mr; dx <= mr; dx++)
        if (dx * dx + dy * dy <= mr * mr) canvas->drawPixel(sx + dx, sy + dy, IYELLOW);
    if (nm[0]) { canvas->setCursor(sx + mr + 2, sy + 3 * sc); canvas->print(nm); }
  }
}

// Render the Navigation Display (rotating compass card / HSI) into `canvas`.
// Heading is the tilt-compensated value computed in updateHeading() (IMU.ino),
// so the card stays accurate when the unit is pitched or rolled.
void drawNavigationDisplay(MyCanvas8 *canvas, state *s) {
  int width = canvas->width();
  int height = canvas->height();
  canvas->textScale = lyt::txtScale(width);   // bigger panel -> bigger text (1x at the 280 reference)

  canvas->setFont();
  canvas->fillScreen(IBLACK);

  canvas->setTextColor(IWHITE);
  canvas->setTextSize(1);

  float angle = s->heading;   // tilt-compensated magnetic heading (deg, 0..360)

  // Map center: live GPS; when GPS is lost, the last position that had a lock
  // (restored from NVS across power cycles); only if there has never been one
  // does it fall back to the hard-coded default (Cincinnati).
  bool  gpsOk = s->GPS;
  // Map centre: live GPS normally; in field mode it freezes on the captured field
  // point P so the small field stays put (and is shareable).
  bool  field = gMapFieldActive;
  float clat  = field ? gMapFieldLat : (s->has_pos ? s->last_lat : MAP_DEFAULT_LAT);
  float clon  = field ? gMapFieldLon : (s->has_pos ? s->last_lon : MAP_DEFAULT_LON);
  // ringRangeM = the ACTUAL range (range rings, Remote ID, own-ship distance) — RID
  // is never magnified. chartRangeM/chartLod = the basemap: in field mode it renders
  // zoomed to range*FIELD_MAP_MULT (~80 km and in) around P for context; gMapLod is
  // already that basemap's LOD (see map_zoom.cpp).
  int ringRangeM  = gMapRangeM;
  int chartRangeM = field ? gMapRangeM * FIELD_MAP_MULT : gMapRangeM;
  int chartLod    = gMapLod;

  // Compass geometry. Lower the ring top just enough that the heading box (which
  // rises ~1.5x its height above the ring top) fits on-canvas above it. At the
  // 280-ref ND the box already clears height/8, so ringTop stays height/8 and the
  // compass is identical there; on the wide panel the ring shrinks a touch.
  int cyc     = height * 11 / 12;            // compass center y (dual: low, so the radar is large)
  int hbh     = lyt::scaled(17, width);     // heading box height
  int ringTop = height / 8;
  int need    = 3 * hbh / 2 + lyt::scaled(4, width);
  if (ringTop < need) ringTop = need;
  int rad     = cyc - ringTop;              // outer ring radius
#if COMBINED_DISPLAY
  // Combined panel: grow the compass to fill the ND band. Center the rings on the aircraft
  // triangle's CENTER (not its tip) so the radius is as large as possible, with the triangle
  // base at the display bottom; pull the heading box flush to the ND top (which ND_OVERLAP
  // positions ~15px under the PFD turn coordinator).
  cyc     = height - 0.03 * height;         // ring center == triangle center (near the bottom)
  ringTop = 3 * hbh / 2;                    // heading box top sits at the ND canvas top
  rad     = cyc - ringTop;
#endif

#if MAP_ENABLE
  // Moving-map chart UNDER the compass card (clipped to the radar circle).
  drawChart(canvas, width / 2, cyc, rad, clat, clon, angle, chartRangeM, chartLod);
#endif

  for (int n = 0; n < 72; n++) {
    canvas->setRotationMatrix((n * 5 - angle) * p / 180, width / 2, cyc);
    if (n % 2 == 0)
      canvas->drawLine(width / 2, lyt::scaled(10, width) + ringTop, width / 2, lyt::scaled(1, width) + ringTop, IWHITE);
    else
      canvas->drawLine(width / 2, lyt::scaled(5, width) + ringTop, width / 2, lyt::scaled(1, width) + ringTop, IWHITE);
    if (n % 6 == 0) {
      char str[4];
      if (n / 2 < 10)
        sprintf(str, "0%d", n / 2);
      else
        sprintf(str, "%d", n / 2);
      printCentered(canvas, str, width / 2, lyt::scaled(17, width) + ringTop);
    }
  }

  canvas->setRotationMatrix();

  char str[4];
  if (s->IMU) {
    sprintf(str, "00%d", int(angle));
    if (angle >= 100)
      sprintf(str, "%d", int(angle));
    else if (angle >= 10)
      sprintf(str, "0%d", int(angle));
  } else
    sprintf(str, "N/A");

  canvas->setFont(&FreeMono9pt7b);
  // Heading box: tip at the ring top, box rising above it (ring sized so it fits).
  drawArrowNumber(canvas, str, width / 2, ringTop, lyt::scaled(48, width), hbh, 2);

  canvas->drawCircle(width / 2, cyc, rad, IWHITE);
  canvas->drawCircle(width / 2, cyc, rad * 3 / 4, IGREY);
  canvas->drawCircle(width / 2, cyc, rad * 1 / 2, IGREY);
  canvas->drawCircle(width / 2, cyc, rad * 1 / 4, IGREY);

  canvas->fillTriangle(0, cyc - 0.05 * height, width / 2, cyc, 0, cyc, IBLACK);
  canvas->fillTriangle(width, cyc - 0.05 * height, width / 2, cyc, width, cyc, IBLACK);
  canvas->fillRect(0, cyc, width, height - cyc, IBLACK);

  // aircraft symbol — height-based on BOTH axes so it isn't stretched on a wide panel
  int triApex = cyc;
#if COMBINED_DISPLAY
  triApex = cyc - 0.03 * height;   // center the triangle on cyc (its center == the ring center)
#endif
  canvas->drawTriangle(width / 2, triApex, width / 2 - 0.04 * height, triApex + 0.06 * height,
                       width / 2 + 0.04 * height, triApex + 0.06 * height, IWHITE);
  canvas->drawLine(width / 2, height, width / 2, lyt::scaled(23, width) + ringTop, IMAGENTA);
  canvas->drawLine(width / 2, lyt::scaled(23, width) + ringTop, width / 2, ringTop, IGREY);

  // ---- user flight plan (yellow route + waypoint markers/names), over the map ----
#if MAP_ENABLE
  drawFlightPlan(canvas, width / 2, cyc, rad, clat, clon, angle, chartRangeM);
#endif

  // ---- home + ground-track overlays (ground-track drawn LAST = on top) ------
  float ndcx = width / 2.0f, ndcy = cyc, ndr = rad;   // match the (possibly shrunk) compass

  // return-to-home: a GREEN direction line (2x the old length), plus a green
  // "map point" at the home bearing + range-scaled distance.
  float hb = bearingToHome(s);
  if (hb >= 0) {
    float a  = (hb - angle) * p / 180.0f;
    float ey = ndcy - cos(a) * ndr;
    int   hl = lyt::scaled(28, width);                 // 2x the previous 14 px length
    if (ey < ndcy) {
      canvas->drawLine(ndcx + sin(a) * (ndr - hl), ndcy - cos(a) * (ndr - hl),
                       ndcx + sin(a) * ndr, ey, IGREEN);
#ifdef SVG_RENDER
      { extern void svgLandmark(const char *, int, int);
        svgLandmark("home", (int)(ndcx + sin(a) * ndr), (int)ey); }
#endif
    }
    float hd = distanceToHome(s);
    if (hd >= 0) {
      float rr = ndr * (hd < ringRangeM ? hd / ringRangeM : 1.0f);
      float hx = ndcx + sin(a) * rr, hy = ndcy - cos(a) * rr;
      if (hy < ndcy)
        canvas->fillRect(hx - 2, hy - 2, 4, 4, IGREEN);   // home map point
    }
  }

  // ground-track (course over ground): a WHITE line, longer than the old tick but
  // SHORTER than the home line, drawn last so it sits on top of the home + lubber.
  if (s->GPS && s->ground_speed > 2.0f) {
    float a = (s->ground_track - angle) * p / 180.0f;
    float ey = ndcy - cos(a) * ndr;
    if (ey < ndcy) {
      // track vector runs from the own-aircraft symbol (ring centre) out to the rim
      canvas->drawLine(ndcx, ndcy, ndcx + sin(a) * ndr, ey, IWHITE);
#ifdef SVG_RENDER
      { extern void svgLandmark(const char *, int, int);
        svgLandmark("track", (int)(ndcx + sin(a) * ndr), (int)ey); }
#endif
    }
  }

#if MAP_ENABLE
  // ---- Remote ID traffic: one uniquely-coloured (orange) dot per drone/UAS that
  //      broadcast its position, with its altitude in feet beside it. Altitude is
  //      height AGL (Remote ID aircraft are capped at 400 ft). The targets are
  //      received over BLE + WiFi by RemoteID.cpp; here we project + plot them.
  if (s->n_rid > 0) {
    MapProj rm;
    rm.clat = clat; rm.clon = clon; rm.cosLat = cosf(clat * p / 180.0f);
    float hh = angle * p / 180.0f; rm.cosH = cosf(hh); rm.sinH = sinf(hh);
    rm.scale = (float)rad / ringRangeM; rm.cx = width / 2; rm.cy = cyc;   // ACTUAL range — RID never magnified
    for (int i = 0; i < s->n_rid && i < RID_MAX; i++) {
      int rx, ry; mapXY(rm, s->rid[i].lat, s->rid[i].lon, rx, ry);
      if (!mapInCirc(rx, ry, width / 2, cyc, (long)rad * rad)) continue;
      int rs = lyt::scaled(3, width);
      canvas->fillCircle(rx, ry, rs, IORANGE);
      canvas->setFont(); canvas->setTextSize(1); canvas->setTextColor(IORANGE);
      char rb[8]; sprintf(rb, "%d", s->rid[i].alt_ft);
      drawText(canvas, rb, rx + rs + lyt::scaled(3, width), ry, lyt::HL, lyt::VC);
      canvas->setTextColor(IWHITE);
#ifdef SVG_RENDER
      { extern void svgLandmark(const char *, int, int); svgLandmark("rid", rx, ry); }
#endif
    }
  }
#endif

#if MAP_ENABLE && MAP_LABELS
  // ---- map text: drawn LAST so labels sit on top of the compass/rings/overlays
  //      and are always readable; placement avoids collisions and stays on-map.
  drawChartLabels(canvas, width / 2, cyc, rad, clat, clon, angle, chartRangeM, chartLod);
#endif

  // ---- GPS-lost warning over the radar: an exact copy of the PFD status box
  //      (grey fill + white centered text), but with a red inlaid outline. -----
  canvas->setRotationMatrix();
  canvas->setFont();
  canvas->setTextSize(1);
  if (!gpsOk) {
    const char *wmsg = "GPS LOST";
    int16_t wbx, wby; uint16_t wbw, wbh;
    canvas->getTextBounds(wmsg, 0, 0, &wbx, &wby, &wbw, &wbh);
    int scl  = lyt::txtScale(width);
    int padH = (int)(0.125f * width)  - 18 * scl;    // same padding the PFD "BACKUP" box has
    int padV = (int)(0.075f * height) -  4 * scl;    // (0.25W x 0.15H box around its text)
    if (padH < 4 * scl) padH = 4 * scl;
    if (padV < 3 * scl) padV = 3 * scl;
    int bcx = width / 2, bcy = cyc - rad / 2;        // box center, upper-middle of the radar
    int bw  = wbw + 2 * padH, bh = wbh + 2 * padV;
    canvas->fillRect(bcx - bw / 2, bcy - bh / 2, bw, bh, IGREY);              // same grey fill
    canvas->drawRect(bcx - bw / 2 + 3, bcy - bh / 2 + 3, bw - 6, bh - 6, IRED); // red inlaid outline
    canvas->setTextColor(IWHITE);                                             // same white text
    printCentered(canvas, wmsg, bcx, bcy);
  }

  // ---- lat / lon at the bottom (lat left, lon right) ------------------------
  {
    char latS[14], lonS[14];
    sprintf(latS, "%.4f%c", fabsf(clat), clat >= 0 ? 'N' : 'S');
    sprintf(lonS, "%.4f%c", fabsf(clon), clon >= 0 ? 'E' : 'W');
    canvas->setTextColor(gpsOk ? IGREEN : IGREY);
    int16_t bx, by; uint16_t lw, lh, ow;
    canvas->getTextBounds(latS, 0, 0, &bx, &by, &lw, &lh);
    canvas->getTextBounds(lonS, 0, 0, &bx, &by, &ow, &lh);
    int ty = height - lh - 1;
#if COMBINED_DISPLAY
    // Combined (single) panel: the text is 2x (textScale) and collides with the radar
    // when flanking the triangle, so push lat/lon out to the bottom corners.
    canvas->setCursor(2, ty);              canvas->print(latS);   // bottom-left
    canvas->setCursor(width - ow - 2, ty); canvas->print(lonS);   // bottom-right
#else
    // Dual panel: flank the aircraft triangle (UNCHANGED from prior builds).
    int tw = (int)(0.04f * height) + lyt::scaled(5, width);
    canvas->setCursor(width / 2 - tw - lw, ty); canvas->print(latS);
    canvas->setCursor(width / 2 + tw, ty);      canvas->print(lonS);
#endif
  }

  // ---- GPS satellite count: LOW-SAT warning (< 5), upper-right (BOTH configs) ---
  int sc = lyt::txtScale(width);
  // Corner text (SAT count, PROXIMITY, range readout, battery) sits below the top
  // edge — shifted down ~2 lines so it clears the heading box / top of the radar.
  int satY = lyt::scaled(4, width) + 18 * sc;
  if (s->sats < 5) {
    canvas->setRotationMatrix();          // identity for this static label
    canvas->setFont();
    canvas->setTextSize(1);
    char sg[12];
    sprintf(sg, "SAT %d", s->sats);
    canvas->setTextColor(IYELLOW);
    int16_t sgx, sgy; uint16_t sgw, sgh;
    canvas->getTextBounds(sg, 0, 0, &sgx, &sgy, &sgw, &sgh);
    canvas->setCursor(width - sgw - 2, satY);   // upper-right corner
    canvas->print(sg);
  }

  // ---- Remote ID PROXIMITY warning: same colour as the Remote ID markers,
  //      far left, at the SAT-count height, shown whenever any Remote ID traffic
  //      is being tracked. ---------------------------------------------------------
  if (s->n_rid_seen > 0) {
    canvas->setRotationMatrix();
    canvas->setFont();
    canvas->setTextSize(1);
    canvas->setTextColor(IORANGE);
    canvas->setCursor(2, satY);
    canvas->print("PROXIMITY");
    canvas->setTextColor(IWHITE);
  }

  // ---- range readout: the outer-ring radius = the current ACTUAL zoom. In field
  //      mode it's orange (the basemap is magnified to ~80 km but the rings + RID
  //      stay at this real range) and the frozen field point is shown at bottom. --
  {
    canvas->setRotationMatrix();
    canvas->setFont();
    canvas->setTextSize(1);
    char rng[16];
    float nm = ringRangeM / 1852.0f;
    if (nm < 1.0f) sprintf(rng, "%dm", ringRangeM);
    else           sprintf(rng, "%.0fNM", nm);
    canvas->setTextColor(field ? IORANGE : IGREY);
    canvas->setCursor(2, satY + 9 * sc);
    canvas->print(rng);
#ifdef SVG_RENDER
    { extern void svgLandmark(const char *, int, int);
      int16_t rbx, rby; uint16_t rbw, rbh;
      canvas->getTextBounds(rng, 2, satY + 9 * sc, &rbx, &rby, &rbw, &rbh);
      svgLandmark("range", 2 + rbw / 2, satY + 9 * sc + 4 * sc); }
#endif
    canvas->setTextColor(IWHITE);
  }

#if HAVE_BATTERY
  // ---- battery voltage: small text just UNDER the SAT-count slot (upper-right,
  //      BOTH configs). Grey normally, red below 3.1 V. BOARD_A/C read a built-in
  //      divider (GPIO1 / GPIO4); BOARD_D reads the SY6970 PMIC over I2C.
  {
    canvas->setRotationMatrix();          // identity for this static label
    canvas->setFont();
    canvas->setTextSize(1);
    float vb = readBatteryVoltage();
    char  bs[12];
    sprintf(bs, "%.2fV", vb);
    canvas->setTextColor(vb < 3.1f ? IRED : IGREY);
    int16_t bbx, bby; uint16_t bbw, bbh;
    canvas->getTextBounds(bs, 0, 0, &bbx, &bby, &bbw, &bbh);
    canvas->setCursor(width - bbw - 2, satY + 8 * sc + lyt::scaled(2, width));   // under the SAT slot
    canvas->print(bs);
#ifdef SVG_RENDER
    { extern void svgLandmark(const char *, int, int);
      svgLandmark("batt", width - bbw / 2 - 2, satY + 8 * sc + lyt::scaled(2, width) + 4); }
#endif
  }
#endif
}