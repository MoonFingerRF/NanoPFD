// Host test for the BOARD_C 4-bit packed canvas math (MyCanvas8.h) + composite
// (CombinedDisplay.ino). Draws an identical pattern into a 4-bit and an 8-bit canvas,
// composites both to RGB565, and asserts they match pixel-for-pixel. If they match, the
// nibble pack/unpack + combo-LUT round-trip is correct.
//   g++ -std=c++11 tests/test_canvas4.cpp -o /tmp/tc4 && /tmp/tc4
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

static const int W = 480, H = 16;
static uint16_t color_index[16];
static uint32_t gComboLUT[256];
static void combineLutRebuild() {
  for (int b = 0; b < 256; b++)
    gComboLUT[b] = (uint32_t)color_index[(b >> 4) & 0x0F] | ((uint32_t)color_index[b & 0x0F] << 16);
}

// ---- 4-bit canvas (mirrors MyCanvas8.h packed4 path) ----
struct C4 {
  uint8_t buf[(W * H + 1) >> 1];
  void fillScreen(uint8_t c) { memset(buf, (c << 4) | c, (W * H + 1) >> 1); }
  void putNib(int x, int y, uint8_t c) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    int32_t i = (int32_t)y * W + x; uint8_t *p = buf + (i >> 1);
    if (i & 1) *p = (*p & 0xF0) | (c & 0x0F); else *p = (*p & 0x0F) | ((c & 0x0F) << 4);
  }
  void drawPixel(int x, int y, uint8_t c) { putNib(x, y, c); }
  void hLine(int x, int y, int w, uint8_t color) {
    if (w <= 0 || y < 0 || y >= H) return;
    if (x < 0) { w += x; x = 0; } if (x + w > W) w = W - x; if (w <= 0) return;
    uint8_t c = color & 0x0F; int32_t rowx = (int32_t)y * W + x;
    if (x & 1) { uint8_t *p = buf + (rowx >> 1); *p = (*p & 0xF0) | c; rowx++; w--; }
    int bytes = w >> 1;
    if (bytes) { memset(buf + (rowx >> 1), (uint8_t)((c << 4) | c), bytes); rowx += bytes * 2; w -= bytes * 2; }
    if (w == 1) { uint8_t *p = buf + (rowx >> 1); *p = (*p & 0x0F) | (c << 4); }
  }
  void vLine(int x, int y, int h, uint8_t color) {
    if (h <= 0 || x < 0 || x >= W) return;
    if (y < 0) { h += y; y = 0; } if (y + h > H) h = H - y; if (h <= 0) return;
    uint8_t c = color & 0x0F; uint8_t *p = buf + (((int32_t)y * W + x) >> 1); int step = W >> 1;
    if (x & 1) { for (int i = 0; i < h; i++) { *p = (*p & 0xF0) | c; p += step; } }
    else { uint8_t cc = c << 4; for (int i = 0; i < h; i++) { *p = (*p & 0x0F) | cc; p += step; } }
  }
};
// ---- 8-bit reference canvas ----
struct C8 {
  uint8_t buf[W * H];
  void fillScreen(uint8_t c) { memset(buf, c, W * H); }
  void drawPixel(int x, int y, uint8_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) buf[(int32_t)y * W + x] = c; }
  void hLine(int x, int y, int w, uint8_t c) { for (int i = 0; i < w; i++) drawPixel(x + i, y, c); }
  void vLine(int x, int y, int h, uint8_t c) { for (int i = 0; i < h; i++) drawPixel(x, y + i, c); }
};

static void composite4(uint16_t *fb, const uint8_t *packed, int npix) {
  uint32_t *fb32 = (uint32_t *)fb; int nb = npix >> 1;
  for (int b = 0; b < nb; b++) fb32[b] = gComboLUT[packed[b]];
  if (npix & 1) fb[npix - 1] = color_index[packed[nb] >> 4];
}
static void composite8(uint16_t *fb, const uint8_t *idx, int npix) {
  uint32_t *fb32 = (uint32_t *)fb; int i = 0, half = npix >> 1;
  for (int j = 0; j < half; j++, i += 2) fb32[j] = (uint32_t)color_index[idx[i]] | ((uint32_t)color_index[idx[i + 1]] << 16);
  if (npix & 1) fb[npix - 1] = color_index[idx[npix - 1]];
}

int main() {
  for (int i = 0; i < 16; i++) color_index[i] = (uint16_t)(0x1000 + i * 0x0911);   // distinct
  combineLutRebuild();
  C4 c4; C8 c8;
  // identical draw program into both
  c4.fillScreen(0); c8.fillScreen(0);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t c = (x * 7 + y * 13) & 0x0F; c4.drawPixel(x, y, c); c8.drawPixel(x, y, c); }
  c4.hLine(3, 2, 100, 5);  c8.hLine(3, 2, 100, 5);     // odd start
  c4.hLine(4, 3, 101, 9);  c8.hLine(4, 3, 101, 9);     // even start, odd width
  c4.vLine(7, 0, H, 12);   c8.vLine(7, 0, H, 12);      // odd x column
  c4.vLine(8, 0, H, 14);   c8.vLine(8, 0, H, 14);      // even x column
  // a fill-like direct nibble write (mirrors the horizon fill: fixed column, step down)
  for (int cx = 100; cx < 104; cx++) { bool hi = !(cx & 1); for (int y = 0; y < H; y++) {
      uint8_t v = (uint8_t)((cx + y) & 0x0F); uint8_t *p = c4.buf + (((int32_t)y * W + cx) >> 1);
      if (hi) *p = (*p & 0x0F) | (v << 4); else *p = (*p & 0xF0) | (v & 0x0F);
      c8.drawPixel(cx, y, v); } }
  // composite both and compare
  std::vector<uint16_t> fb4(W * H, 0xDEAD), fb8(W * H, 0xBEEF);
  composite4(fb4.data(), c4.buf, W * H);
  composite8(fb8.data(), c8.buf, W * H);
  int bad = 0, firstx = -1, firsty = -1;
  for (int i = 0; i < W * H; i++) if (fb4[i] != fb8[i]) { if (!bad) { firstx = i % W; firsty = i / W; } bad++; }
  if (bad) { printf("FAIL: %d/%d pixels differ; first at (%d,%d) 4bit=%04x 8bit=%04x\n",
                    bad, W * H, firstx, firsty, fb4[firsty * W + firstx], fb8[firsty * W + firstx]); return 1; }
  printf("PASS: 4-bit canvas + composite match the 8-bit reference for all %d pixels\n", W * H);
  return 0;
}
