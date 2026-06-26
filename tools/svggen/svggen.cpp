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
#include <algorithm>
#include <utility>

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

// Representative screen positions of map features, recorded by SVG_RENDER hooks
// in drawChart/drawNavigationDisplay so the legend's callouts point at real spots.
struct Landmark { std::string name; int x, y; };
std::vector<Landmark> gLandmarks;
void svgLandmark(const char *name, int x, int y) {
  for (auto &L : gLandmarks) if (L.name == name) return;   // keep the first of each kind
  gLandmarks.push_back({name, x, y});
}

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
//  Annotated ND legend
// ---------------------------------------------------------------------------
static std::string xesc(const std::string &s) {
  std::string o;
  for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else o += c; }
  return o;
}
struct LM { int x = -1, y = -1; };
static LM findLM(const char *n) {
  for (auto &L : gLandmarks) if (L.name == n) return {L.x, L.y};
  return {};
}

static void genLegend(const char *path, const std::vector<Elem> &nd) {
  const int W = RGB_WIDTH, H = ND_CANVAS_H;
  // Mirror drawNavigationDisplay's BOARD_C / COMBINED geometry.
  const int sc = 2, hbh = 17 * sc, ringTop = 3 * hbh / 2;
  const int cyc = H - (int)(0.03 * H), rad = cyc - ringTop, triApex = cyc - (int)(0.03 * H);
  auto ringPt = [&](double deg, double rr) {
    double a = deg * M_PI / 180.0;
    return std::make_pair(W / 2.0 + sin(a) * rr, cyc - cos(a) * rr);
  };

  struct CO { std::string t; uint8_t col; double ax, ay; };
  std::vector<CO> C;
  auto add   = [&](std::string t, uint8_t c, double x, double y) { C.push_back({t, c, x, y}); };
  auto addLM = [&](const char *n, std::string t, uint8_t c) { LM l = findLM(n); if (l.x >= 0) add(t, c, l.x, l.y); };

  // structural / overlay elements (computed the same way the drawer does)
  add("Heading — digital readout",        IWHITE,   W / 2.0, ringTop - hbh * 0.6);
  { auto p = ringPt(-40, rad);      add("Compass card (rotates, heading-up)", IWHITE, p.first, p.second); }
  { auto p = ringPt(-62, rad * 0.75); add("Range rings (¼ ½ ¾ of range)", IGREY, p.first, p.second); }
  add("Own-aircraft symbol (fixed)",           IWHITE,   W / 2.0, triApex + 0.03 * H);
  add("Heading reference (track-up)",          IMAGENTA, W / 2.0 - 1, cyc - rad * 0.55);
  addLM("track", "Ground track (course made good)", IWHITE);
  addLM("home",  "Bearing & distance to home",      IGREEN);
  // map features (anchors recorded during the ND draw)
  addLM("apt_twr",   "Towered airport",       IBLUE);
  addLM("apt_ntwr",  "Non-towered airport",   IMAGENTA);
  addLM("navaid",    "VOR / navaid",          IBLUE);
  addLM("city",      "City / landmark",       IYELLOW);
  addLM("apt_closed","Closed airfield",       IGREY);
  { const char *rn[] = {"ring_lg","ring_md","ring_sm","ring_cl"};
    uint8_t rc[] = {ICYAN, IGREEN, IYELLOW, IGREY};
    for (int i = 0; i < 4; i++) { LM l = findLM(rn[i]); if (l.x >= 0) { add("Airport ring (size→colour)", rc[i], l.x, l.y); break; } } }
  addLM("ring_rstr", "Restricted airspace",   IRED);
  addLM("river",     "River",                 ISKY);
  addLM("road",      "Road / highway",        IDGREY);
  addLM("state",     "State line",            IGREY);
  add("GPS position (lat / lon)",             IGREEN, 55, H - 9);
  addLM("batt", "Battery voltage", IGREY);

  // legend canvas: ND in the middle, label columns left & right.
  const double f = 1.25;
  const int ndW = (int)(W * f), ndH = (int)(H * f);
  const int colW = 274, ox = colW, oy = 44, LW = ndW + 2 * colW, LH = oy + ndH + 54;
  auto TX = [&](double x) { return ox + f * x; };
  auto TY = [&](double y) { return oy + f * y; };

  std::vector<int> Ls, Rs;
  for (int i = 0; i < (int)C.size(); i++) (C[i].ax < W / 2.0 ? Ls : Rs).push_back(i);
  auto byY = [&](int a, int b) { return C[a].ay < C[b].ay; };
  std::sort(Ls.begin(), Ls.end(), byY);
  std::sort(Rs.begin(), Rs.end(), byY);

  std::string o; char buf[768];
  snprintf(buf, sizeof buf,
    "<svg viewBox=\"0 0 %d %d\" xmlns=\"http://www.w3.org/2000/svg\" "
    "font-family=\"Helvetica,Arial,sans-serif\">\n", LW, LH);
  o += buf;
  snprintf(buf, sizeof buf, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"#000000\"/>\n", LW, LH);
  o += buf;
  // dim the busy map so the bright callouts/leaders/labels stand out on top
  snprintf(buf, sizeof buf, "<g transform=\"translate(%d %d) scale(%g)\" shape-rendering=\"crispEdges\" opacity=\"0.58\">\n", ox, oy, f);
  o += buf;
  emitElems(o, coalesce(nd));
  o += "</g>\n";
  snprintf(buf, sizeof buf,
    "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"#444\" stroke-width=\"1\"/>\n",
    ox, oy, ndW, ndH);
  o += buf;

  auto place = [&](std::vector<int> &idx, bool right) {
    int n = (int)idx.size();
    double top = oy + 18, bot = LH - 30;
    for (int k = 0; k < n; k++) {
      const CO &c = C[idx[k]];
      double ly = (n > 1) ? top + (bot - top) * k / (n - 1) : (top + bot) / 2;
      double fx = TX(c.ax), fy = TY(c.ay);
      double sx = right ? ox + ndW + 8 : ox - 8;
      double lx = right ? ox + ndW + 13 : ox - 13;
      double mx = right ? sx - 7 : sx + 7;
      std::string col = hexFor(c.col);
      // leader (black halo + coloured line)
      snprintf(buf, sizeof buf,
        "<polyline points=\"%g,%g %g,%g %g,%g\" fill=\"none\" stroke=\"#000\" stroke-width=\"4\"/>"
        "<polyline points=\"%g,%g %g,%g %g,%g\" fill=\"none\" stroke=\"%s\" stroke-width=\"1.6\"/>\n",
        sx, ly, mx, ly, fx, fy, sx, ly, mx, ly, fx, fy, col.c_str());
      o += buf;
      // marker ring on the feature
      snprintf(buf, sizeof buf,
        "<circle cx=\"%g\" cy=\"%g\" r=\"6.5\" fill=\"none\" stroke=\"#000\" stroke-width=\"3\"/>"
        "<circle cx=\"%g\" cy=\"%g\" r=\"6.5\" fill=\"none\" stroke=\"%s\" stroke-width=\"1.7\"/>\n",
        fx, fy, fx, fy, col.c_str());
      o += buf;
      // label (haloed text)
      snprintf(buf, sizeof buf,
        "<text x=\"%g\" y=\"%g\" fill=\"%s\" font-size=\"14\" font-weight=\"600\" "
        "text-anchor=\"%s\" paint-order=\"stroke\" stroke=\"#000\" stroke-width=\"3.5\">%s</text>\n",
        lx, ly + 5, col.c_str(), right ? "start" : "end", xesc(c.t).c_str());
      o += buf;
    }
  };
  place(Ls, false);
  place(Rs, true);
  o += "</svg>\n";

  FILE *fp = fopen(path, "w");
  if (!fp) { fprintf(stderr, "cannot open %s\n", path); return; }
  fwrite(o.data(), 1, o.size(), fp); fclose(fp);
  fprintf(stderr, "wrote %s (%zu bytes, %zu callouts)\n", path, o.size(), C.size());
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
  s.heading = 75; s.ground_track = 82; s.ground_speed = 118;   // ~7 deg crab -> track distinct
  s.lat = MAP_DEFAULT_LAT; s.lon = MAP_DEFAULT_LON;
  s.last_lat = MAP_DEFAULT_LAT; s.last_lon = MAP_DEFAULT_LON;   // ND centres on last_*
  s.home_lat = MAP_DEFAULT_LAT + 0.04f; s.home_lon = MAP_DEFAULT_LON + 0.06f;  // home NE -> green line visible
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
  // Annotated ND legend (uses landmarks recorded during the ND draw above).
  genLegend("docs/nd_legend.svg", nd.elems);
  return 0;
}
