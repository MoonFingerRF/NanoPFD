#pragma once
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

class MyCanvas8 : public GFXcanvas8 {
public:
  MyCanvas8(uint16_t w, uint16_t h, bool allocate_buffer = true) : GFXcanvas8(w, h, allocate_buffer) {
    setRotationMatrix();   // default to identity so drawPixel is a no-op transform
  }
  float rotA, rotB, offX, offY;
  bool  identity;                                                     // true when no rotation is set
  void setRotationMatrix(float angle = 0, float x = 0, float y = 0) {  //rotation angle around point
    rotA = cos(angle);
    rotB = sin(angle);
    offX = x;
    offY = y;
    identity = (angle == 0.0f && x == 0.0f && y == 0.0f);
  }
  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (identity) {                  // matrix identity (the common case): inline the canvas
      // rotation + a direct buffer write, avoiding the non-inlined GFXcanvas8::drawPixel call
      // on EVERY pixel. Byte-identical to the base path, now for ALL rotations (BOARD_C/D are
      // rot0; BOARD_A is rot2 PFD / rot1 ND -> the map's per-pixel draw gets this too).
      if ((uint16_t)x >= (uint16_t)_width || (uint16_t)y >= (uint16_t)_height) return;
      switch (rotation) {
        case 1: { int16_t t = x; x = WIDTH - 1 - y; y = t; } break;
        case 2: x = WIDTH - 1 - x; y = HEIGHT - 1 - y; break;
        case 3: { int16_t t = x; x = y; y = HEIGHT - 1 - t; } break;
      }
      buffer[(int32_t)y * WIDTH + x] = (uint8_t)color;
      return;
    }
    float x_ = x - offX;
    float y_ = y - offY;
    GFXcanvas8::drawPixel(rotA * x_ - rotB * y_ + offX, rotA * y_ + rotB * x_ + offY, color);
  }
  // Adafruit_GFX turns vertical/horizontal drawLine() into drawFastV/HLine,
  // which write the buffer directly and would BYPASS the rotation matrix — that
  // dropped the rotated compass tick marks. Route them through drawPixel() while
  // a rotation is active; keep the fast base path for the common (identity) case.
  // fillRect/text route through these; for the unrotated canvas write the buffer directly
  // (a clipped memset / strided store) instead of a non-inlined GFXcanvas8 call + rotation
  // switch per span. Byte-identical to the rot-0 base path. Big win for size-2 text (one
  // tiny call+switch per glyph pixel otherwise). Falls back for rot != 0 or active matrix.
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    if (identity) {
      if (rotation == 0) {
        if (h <= 0 || x < 0 || x >= WIDTH) return;
        if (y < 0) { h += y; y = 0; }
        if (y + h > HEIGHT) h = HEIGHT - y;
        uint8_t *pp = buffer + (int32_t)y * WIDTH + x, c = (uint8_t)color;
        for (int16_t i = 0; i < h; i++) { *pp = c; pp += WIDTH; }
        return;
      }
      GFXcanvas8::drawFastVLine(x, y, h, color); return;
    }
    for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, color);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    if (identity) {
      if (rotation == 0) {
        if (w <= 0 || y < 0 || y >= HEIGHT) return;
        if (x < 0) { w += x; x = 0; }
        if (x + w > WIDTH) w = WIDTH - x;
        if (w > 0) memset(buffer + (int32_t)y * WIDTH + x, (uint8_t)color, w);
        return;
      }
      GFXcanvas8::drawFastHLine(x, y, w, color); return;
    }
    for (int16_t i = 0; i < w; i++) drawPixel(x + i, y, color);
  }
  void fastDrawPixel(int16_t x, int16_t y, uint16_t color) {
    GFXcanvas8::drawPixel(x, y, color);
  }
  // Point the canvas at an externally-owned buffer (e.g. one placed in PSRAM
  // via heap_caps_malloc). Construct with allocate_buffer=false first; the
  // GFXcanvas8 destructor won't free a buffer it didn't allocate.
  void useBuffer(uint8_t *b) { buffer = b; }
  // Per-canvas text magnification (1 = unchanged). The COMBINED build (larger
  // panel) sets this >1 so labels scale up with the bigger canvas. Adafruit's
  // size multiplier scales the built-in font AND custom GFX fonts alike, so this
  // one knob covers every setTextSize() the drawers issue on this canvas.
  uint8_t textScale = 1;
  void setTextSize(uint8_t s) { GFXcanvas8::setTextSize((uint8_t)(s * textScale)); }

#ifdef SVG_RENDER
  // Host SVG generator (tools/svggen) only: Adafruit_GFX's drawCircle/fillCircle/
  // drawTriangle/fillTriangle are non-virtual, so the recording canvas can't
  // intercept them as whole shapes. Re-declaring them here as virtual lets the
  // generator capture clean vector circles/polygons instead of rasterised pixels.
  // On the device build (SVG_RENDER undefined) none of this exists and the base
  // (non-virtual) GFX implementations are used exactly as before.
  virtual void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { Adafruit_GFX::drawCircle(x, y, r, c); }
  virtual void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { Adafruit_GFX::fillCircle(x, y, r, c); }
  virtual void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t c) { Adafruit_GFX::drawTriangle(x0, y0, x1, y1, x2, y2, c); }
  virtual void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t c) { Adafruit_GFX::fillTriangle(x0, y0, x1, y1, x2, y2, c); }
#endif
};