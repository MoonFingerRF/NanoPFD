// svggen.cpp — host renderer that runs the REAL PFD/ND drawers against an
// SVG-recording canvas, so the README illustrations are derived from the actual
// rendering code rather than hand-drawn. The GFX shape primitives are recorded
// as SVG vector elements; the attitude indicator (texture-sampled straight into
// the framebuffer, not via GFX primitives) is captured as a pixel diff.
#define SVG_RENDER 1
#include <Adafruit_GFX.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <cstdint>

#include "config.h"      // BOARD_C config (resolved from /tmp/npfd via -I)
#include "State.h"
#include "MyCanvas8.h"

// Palette — must match color_index[] in InstrumentPanel.ino (RGB565 per index).
uint16_t color_index[NUM_COLORS] = {
  0x0000, 0x001F, 0xF800, 0x07E0, 0x07FF, 0xF81F,
  0xFFE0, 0xFFFF, 0x055F, 0xD421, 0xAD55, 0x4208
};

// ---------------------------------------------------------------------------
//  Recorded SVG element
// ---------------------------------------------------------------------------
struct Elem {
  char kind = 0;            // R rectfill, r rectstroke, L line, C circfill,
                            // c circstroke, P polyfill, p polystroke, X pixel
  float a=0,b=0,c=0,d=0;    // rect:x,y,w,h | line:x0,y0,x1,y1 | circ:cx,cy,r | px:x,y
  std::vector<float> pts;   // polygons
  uint8_t col=0;            // palette index
};

// ---------------------------------------------------------------------------
//  SVG-recording canvas
// ---------------------------------------------------------------------------
class SvgCanvas : public MyCanvas8 {
public:
  std::vector<Elem> elems;
  bool recording = true;
  std::vector<uint8_t> bufBefore;

  SvgCanvas(uint16_t w, uint16_t h) : MyCanvas8(w, h, true) {}

  // Apply the active rotation matrix (compass labels/ticks) so recorded coords
  // land where the matrix would have put them. Identity is the common case.
  inline void xf(int16_t x, int16_t y, int &ox, int &oy) {
    if (identity) { ox = x; oy = y; return; }
    float x_ = x - offX, y_ = y - offY;
    ox = (int)lround(rotA * x_ - rotB * y_ + offX);
    oy = (int)lround(rotA * y_ + rotB * x_ + offY);
  }
  inline void push(char k, float a, float b, float c, float d, uint8_t col) {
    Elem e; e.kind = k; e.a = a; e.b = b; e.c = c; e.d = d; e.col = col;
    elems.push_back(std::move(e));
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (recording) { int ox, oy; xf(x, y, ox, oy); push('X', ox, oy, 0, 0, (uint8_t)color); }
    MyCanvas8::drawPixel(x, y, color);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (identity && rotation == 0) {
      if (recording) push('R', x, y, w, 1, (uint8_t)color);
      MyCanvas8::drawFastHLine(x, y, w, color);
    } else MyCanvas8::drawFastHLine(x, y, w, color);   // matrix -> drawPixel records
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (identity && rotation == 0) {
      if (recording) push('R', x, y, 1, h, (uint8_t)color);
      MyCanvas8::drawFastVLine(x, y, h, color);
    } else MyCanvas8::drawFastVLine(x, y, h, color);
  }
  void fillScreen(uint16_t color) override {
    if (recording) push('R', 0, 0, width(), height(), (uint8_t)color);
    GFXcanvas8::fillScreen(color);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (identity && rotation == 0) {
      bool sv = recording; recording = false;
      Adafruit_GFX::fillRect(x, y, w, h, color);       // writes buffer
      recording = sv;
      if (recording) push('R', x, y, w, h, (uint8_t)color);
    } else Adafruit_GFX::fillRect(x, y, w, h, color);
  }
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (identity && rotation == 0) {
      bool sv = recording; recording = false;
      Adafruit_GFX::drawRect(x, y, w, h, color);
      recording = sv;
      if (recording) push('r', x, y, w, h, (uint8_t)color);
    } else Adafruit_GFX::drawRect(x, y, w, h, color);
  }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
    if (identity && rotation == 0) {
      bool sv = recording; recording = false;
      Adafruit_GFX::drawLine(x0, y0, x1, y1, color);
      recording = sv;
      if (recording) push('L', x0, y0, x1, y1, (uint8_t)color);
    } else Adafruit_GFX::drawLine(x0, y0, x1, y1, color);  // matrix -> drawPixel records
  }
  // drawCircle/fillCircle/drawTriangle/fillTriangle are NOT virtual in
  // Adafruit_GFX, so they can't be intercepted as whole shapes; they decompose
  // into writePixel/drawFastVLine/drawFastHLine, which the overrides above
  // capture (circles -> pixel runs, filled shapes -> span rects). Faithful to
  // the on-device pixel result, which is the point.

  // Attitude (inc_map) capture: snapshot before, diff after -> exact pixels.
  void beginAttitude() { uint8_t *b = getBuffer(); bufBefore.assign(b, b + (size_t)width()*height()); }
  void endAttitude() {
    uint8_t *b = getBuffer(); int W = width(), H = height();
    for (int yy = 0; yy < H; yy++)
      for (int xx = 0; xx < W; xx++) {
        size_t i = (size_t)yy*W + xx;
        if (b[i] != bufBefore[i]) push('X', xx, yy, 0, 0, b[i]);
      }
  }
};

// Hooks invoked from instrument_drawer.ino (guarded by SVG_RENDER).
void svgPreAttitudeHook(MyCanvas8 *c) { static_cast<SvgCanvas*>(c)->beginAttitude(); }
void svgAttitudeHook   (MyCanvas8 *c) { static_cast<SvgCanvas*>(c)->endAttitude(); }

int imuSource() { return 1; }   // stub (IMU.ino owns the real one): 1 = BNO08x

#include "instrument_drawer.ino"
#undef p     // the drawer's '#define p 3.1415926' must not leak into our code below

// ---------------------------------------------------------------------------
//  Emit
// ---------------------------------------------------------------------------
static std::string hexFor(uint8_t idx) {
  uint16_t c = color_index[idx];
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (r*255 + 15)/31; g = (g*255 + 31)/63; b = (b*255 + 15)/31;
  char buf[8]; snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b); return buf;
}

// Merge horizontally-adjacent same-row/-height/-colour rects (and 1x1 pixels)
// to keep the file small without changing the rendered result.
static std::vector<Elem> coalesce(const std::vector<Elem> &in) {
  std::vector<Elem> out;
  for (Elem e : in) {
    if (e.kind == 'X') { e.kind = 'R'; e.c = 1; e.d = 1; }
    if (e.kind == 'R' && !out.empty()) {
      Elem &prev = out.back();
      if (prev.kind == 'R' && prev.col == e.col && prev.b == e.b && prev.d == e.d && prev.a + prev.c == e.a) {
        prev.c += e.c; continue;
      }
    }
    out.push_back(e);
  }
  return out;
}

static void emitElems(std::string &o, const std::vector<Elem> &els) {
  char buf[256];
  for (const Elem &e : els) {
    std::string col = hexFor(e.col);
    switch (e.kind) {
      case 'R':
        if (e.c <= 0 || e.d <= 0) break;
        snprintf(buf, sizeof buf,
          "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"%s\"/>\n",
          e.a, e.b, e.c, e.d, col.c_str()); o += buf; break;
      case 'r':
        snprintf(buf, sizeof buf,
          "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"%s\"/>\n",
          e.a, e.b, e.c, e.d, col.c_str()); o += buf; break;
      case 'L':
        snprintf(buf, sizeof buf,
          "<line x1=\"%g\" y1=\"%g\" x2=\"%g\" y2=\"%g\" stroke=\"%s\"/>\n",
          e.a+0.5f, e.b+0.5f, e.c+0.5f, e.d+0.5f, col.c_str()); o += buf; break;
      case 'C':
        snprintf(buf, sizeof buf,
          "<circle cx=\"%g\" cy=\"%g\" r=\"%g\" fill=\"%s\"/>\n",
          e.a+0.5f, e.b+0.5f, e.c+0.5f, col.c_str()); o += buf; break;
      case 'c':
        snprintf(buf, sizeof buf,
          "<circle cx=\"%g\" cy=\"%g\" r=\"%g\" fill=\"none\" stroke=\"%s\"/>\n",
          e.a+0.5f, e.b+0.5f, e.c, col.c_str()); o += buf; break;
      case 'P': case 'p': {
        o += "<polygon points=\"";
        for (size_t i = 0; i + 1 < e.pts.size(); i += 2) {
          snprintf(buf, sizeof buf, "%g,%g ", e.pts[i]+0.5f, e.pts[i+1]+0.5f); o += buf;
        }
        if (e.kind == 'P') { o += "\" fill=\"" + col + "\"/>\n"; }
        else { o += "\" fill=\"none\" stroke=\"" + col + "\"/>\n"; }
        break;
      }
    }
  }
}

struct Group { std::vector<Elem> els; float dx, dy; };

static void writeSvg(const char *path, int W, int H, const std::vector<Group> &groups) {
  std::string o;
  char hdr[256];
  snprintf(hdr, sizeof hdr,
    "<svg viewBox=\"0 0 %d %d\" xmlns=\"http://www.w3.org/2000/svg\" "
    "shape-rendering=\"crispEdges\" stroke-width=\"1\">\n", W, H);
  o += hdr;
  o += "<rect x=\"0\" y=\"0\" width=\"" + std::to_string(W) + "\" height=\"" +
       std::to_string(H) + "\" fill=\"#000000\"/>\n";
  for (const Group &g : groups) {
    char gt[64];
    snprintf(gt, sizeof gt, "<g transform=\"translate(%g %g)\">\n", g.dx, g.dy);
    o += gt;
    emitElems(o, coalesce(g.els));
    o += "</g>\n";
  }
  o += "</svg>\n";
  FILE *f = fopen(path, "w");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
  fwrite(o.data(), 1, o.size(), f); fclose(f);
  fprintf(stderr, "wrote %s (%zu bytes)\n", path, o.size());
}

// ---------------------------------------------------------------------------
int main() {
  // A representative in-flight state: ~10 deg right bank, slight climb.
  state s; init_state(&s);
  s.IMU = true; s.BPS = true; s.ASI = true; s.GPS = true;
  s.gx = 0.17f; s.gy = -0.97f; s.gz = 0.06f;       // bank + slight nose-up
  s.g = 1.04f; s.max_g = 1.6f;
  s.ax = 0.06f; s.ay = -1.0f; s.az = 0.0f;         // slip ball slightly off
  s.air_speed = 110;
  s.alt = 3500; s.home_alt = 900; s.vertical_speed = 320;
  s.heading = 75; s.ground_track = 75; s.ground_speed = 118;
  s.lat = MAP_DEFAULT_LAT; s.lon = MAP_DEFAULT_LON;
  s.last_lat = MAP_DEFAULT_LAT; s.last_lon = MAP_DEFAULT_LON;   // ND centres on last_*
  s.home_lat = MAP_DEFAULT_LAT - 0.05f; s.home_lon = MAP_DEFAULT_LON - 0.09f;
  s.gps_alt = 3500; s.has_pos = true; s.sats = 11;

  static GFXcanvas8 incmap = generate_inc_map(0.6 * PFD_REGION_H, lyt::txtScale(RGB_WIDTH));

  // ---- PFD ----
  SvgCanvas pfd(RGB_WIDTH, PFD_REGION_H);
  drawHorizonDisplay(&pfd, &incmap, &s, /*showCompass*/ false);

  // ---- ND ----
  SvgCanvas nd(RGB_WIDTH, ND_CANVAS_H);
  drawNavigationDisplay(&nd, &s);

  writeSvg("docs/pfd.svg", RGB_WIDTH, PFD_REGION_H, {{pfd.elems, 0, 0}});
  writeSvg("docs/nd.svg",  RGB_WIDTH, ND_CANVAS_H, {{nd.elems, 0, 0}});
  // Combined single panel: PFD on top, ND drawn over the overlap region below.
  writeSvg("docs/combined.svg", RGB_WIDTH, RGB_HEIGHT,
           {{pfd.elems, 0, 0}, {nd.elems, 0, ND_TOP}});
  return 0;
}
