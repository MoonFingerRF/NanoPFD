#pragma once
#include "config.h"            // BOARD_C selects the 4-bit packed canvas (below)
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

#if BOARD_C && !defined(SVG_RENDER)
// ============================================================================
//  MyCanvas8 — BOARD_C variant: a 4-BIT PACKED palette canvas (2 pixels per byte).
//
//  The palette has 15 colors (fits in a nibble), so packing 2 px/byte halves the
//  PFD/ND canvas footprint. That frees ~84 KB of internal SRAM, enough to keep the
//  PFD canvas in fast internal SRAM AND run the WiFi AP + BLE + WiFi Remote ID at the
//  same time at full fps. It also speeds the hot paths: same-color spans memset two
//  pixels per byte, and the composite reads a byte (a pixel-pair) through a 256-entry
//  combo LUT -> two RGB565 pixels per 32-bit store (CombinedDisplay.ino).
//
//  Nibble convention (WIDTH is even, so each row is byte-aligned and x parity == index
//  parity): even x -> HIGH nibble (bits 7:4), odd x -> LOW nibble (bits 3:0). A byte
//  therefore holds (px[even] << 4) | px[odd], left pixel in the high nibble.
//
//  Kept API-compatible with the 8-bit class so the shared drawers (instrument_drawer.ino)
//  need no changes except the direct-buffer horizon fill, which forks #if BOARD_C.
//  Screen rotation is 0 on the combined panel; only the ND compass uses the rotation
//  MATRIX (setRotationMatrix), handled in drawPixel.
// ============================================================================
class MyCanvas8 : public GFXcanvas8 {
public:
  MyCanvas8(uint16_t w, uint16_t h, bool allocate_buffer = false) : GFXcanvas8(w, h, false) {
    (void)allocate_buffer;        // 4-bit canvases always use an external (half-size) buffer
    setRotationMatrix();
  }
  float rotA, rotB, offX, offY;
  bool  identity;
  void setRotationMatrix(float angle = 0, float x = 0, float y = 0) {
    rotA = cosf(angle); rotB = sinf(angle); offX = x; offY = y;
    identity = (angle == 0.0f && x == 0.0f && y == 0.0f);
  }

  // Write one palette index (0..15) into its nibble.
  inline void putNib(int16_t x, int16_t y, uint8_t c) {
    if ((uint16_t)x >= (uint16_t)WIDTH || (uint16_t)y >= (uint16_t)HEIGHT) return;
    int32_t i = (int32_t)y * WIDTH + x;
    uint8_t *p = buffer + (i >> 1);
    if (i & 1) *p = (*p & 0xF0) | (c & 0x0F);              // odd x  -> low nibble
    else       *p = (*p & 0x0F) | ((c & 0x0F) << 4);       // even x -> high nibble
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (!identity) {                                        // ND compass rotation matrix
      float x_ = x - offX, y_ = y - offY;
      x = (int16_t)(rotA * x_ - rotB * y_ + offX);
      y = (int16_t)(rotA * y_ + rotB * x_ + offY);
    }
    putNib(x, y, (uint8_t)color);                           // panel is rotation 0
  }
  void fastDrawPixel(int16_t x, int16_t y, uint16_t color) { putNib(x, y, (uint8_t)color); }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    if (!identity) { for (int16_t i = 0; i < w; i++) drawPixel(x + i, y, color); return; }
    if (w <= 0 || y < 0 || y >= HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > WIDTH) w = WIDTH - x;
    if (w <= 0) return;
    uint8_t c = color & 0x0F;
    int32_t rowx = (int32_t)y * WIDTH + x;                  // linear index of (x,y)
    if (x & 1) { uint8_t *p = buffer + (rowx >> 1); *p = (*p & 0xF0) | c; rowx++; w--; }  // odd lead
    int bytes = w >> 1;                                     // middle: 2 px/byte
    if (bytes) { memset(buffer + (rowx >> 1), (uint8_t)((c << 4) | c), bytes); rowx += bytes * 2; w -= bytes * 2; }
    if (w == 1) { uint8_t *p = buffer + (rowx >> 1); *p = (*p & 0x0F) | (c << 4); }        // even tail
  }

  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    if (!identity) { for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, color); return; }
    if (h <= 0 || x < 0 || x >= WIDTH) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (h <= 0) return;
    uint8_t c = color & 0x0F;
    uint8_t *p = buffer + (((int32_t)y * WIDTH + x) >> 1);
    int step = WIDTH >> 1;                                  // bytes per row
    if (x & 1) { for (int16_t i = 0; i < h; i++) { *p = (*p & 0xF0) | c;        p += step; } }       // low
    else       { uint8_t cc = c << 4; for (int16_t i = 0; i < h; i++) { *p = (*p & 0x0F) | cc; p += step; } } // high
  }

  void fillScreen(uint16_t color) {
    uint8_t c = color & 0x0F;
    memset(buffer, (uint8_t)((c << 4) | c), ((int32_t)WIDTH * HEIGHT + 1) >> 1);
  }

  void useBuffer(uint8_t *b) { buffer = b; }
  uint8_t textScale = 1;
  void setTextSize(uint8_t s) { GFXcanvas8::setTextSize((uint8_t)(s * textScale)); }

  // Bytes needed for a 4-bit packed w*h canvas (half, rounded up).
  static size_t bufBytes(uint16_t w, uint16_t h) { return ((size_t)w * h + 1) >> 1; }
};

#else
// ---------------------------------------------------------------------------
//  8-bit palette canvas — BOARD_A (dual SPI) and BOARD_D (AMOLED) + the host SVG
//  generator. Unchanged from the original.
// ---------------------------------------------------------------------------
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
  static size_t bufBytes(uint16_t w, uint16_t h) { return (size_t)w * h; }

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
#endif  // BOARD_C 4-bit vs 8-bit
