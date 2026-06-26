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
  char kind = 0;            // R rectfill r rectstroke | L line | M map-line(clipped)
                            // C/c circ fill/stroke | O map-ring(clipped) | P/p poly
                            // T text | X pixel
  float a=0,b=0,c=0,d=0;    // rect:x,y,w,h | line:x0,y0,x1,y1 | circ:cx,cy,r | text:x,y
  std::vector<float> pts;   // polygons
  std::string text;         // T: the character(s)
  float fs=0, ang=0;        // T: font size px, rotation deg
  bool dashed=false, mono=false;
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
  inline void xfd(double x, double y, double &ox, double &oy) {
    if (identity) { ox = x; oy = y; return; }
    double x_ = x - offX, y_ = y - offY;
    ox = rotA * x_ - rotB * y_ + offX;
    oy = rotA * y_ + rotB * x_ + offY;
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
  // A clean <line> — endpoints transformed through the active matrix (so the
  // rotated compass ticks come out as real vectors, not pixels).
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
    if (!recording) { Adafruit_GFX::drawLine(x0, y0, x1, y1, color); return; }
    double ax, ay, bx, by; xfd(x0, y0, ax, ay); xfd(x1, y1, bx, by);
    push('L', ax, ay, bx, by, (uint8_t)color);
  }
  // Clean circles / polygons (virtual via the SVG_RENDER shim in MyCanvas8).
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) override { if (recording) push('c', x, y, r, 0, (uint8_t)color); }
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) override { if (recording) push('C', x, y, r, 0, (uint8_t)color); }
  void drawTriangle(int16_t x0,int16_t y0,int16_t x1,int16_t y1,int16_t x2,int16_t y2,uint16_t color) override { pushTri('p', x0,y0,x1,y1,x2,y2,(uint8_t)color); }
  void fillTriangle(int16_t x0,int16_t y0,int16_t x1,int16_t y1,int16_t x2,int16_t y2,uint16_t color) override { pushTri('P', x0,y0,x1,y1,x2,y2,(uint8_t)color); }
  void pushTri(char k,int x0,int y0,int x1,int y1,int x2,int y2,uint8_t col) {
    if (!recording) return;
    Elem e; e.kind = k; e.col = col;
    double ax,ay; xfd(x0,y0,ax,ay); e.pts.push_back(ax); e.pts.push_back(ay);
    xfd(x1,y1,ax,ay); e.pts.push_back(ax); e.pts.push_back(ay);
    xfd(x2,y2,ax,ay); e.pts.push_back(ax); e.pts.push_back(ay);
    elems.push_back(std::move(e));
  }
  // Clean text: catch each printed glyph at the live cursor + font and emit a
  // <text> element (all the firmware's fonts are monospace). The base write still
  // runs (recording suppressed) so the cursor advances exactly as on-device.
  size_t write(uint8_t ch) override {
    if (recording && ch != '\n' && ch != '\r' && ch != ' ') {
      bool custom = (gfxFont != nullptr);
      double sx = textsize_x, sy = textsize_y;
      double bx = cursor_x, by = custom ? cursor_y : cursor_y + 7.0 * sy;  // baseline
      double ox, oy; xfd(bx, by, ox, oy);
      Elem e; e.kind = 'T'; e.col = (uint8_t)textcolor;
      e.a = ox; e.b = oy; e.mono = true;
      e.fs = custom ? 14.0 * sy : 9.0 * sy;     // tuned to the 5x7 / FreeMono cells
      e.ang = identity ? 0.0 : atan2((double)rotB, (double)rotA) * 180.0 / M_PI;
      e.text = std::string(1, (char)ch);
      elems.push_back(std::move(e));
    }
    bool sv = recording; recording = false;
    size_t r = GFXcanvas8::write(ch);
    recording = sv;
    return r;
  }

  // Attitude (inc_map) capture: snapshot before, diff after -> exact pixels.
  void beginAttitude() { uint8_t *b = getBuffer(); bufBefore.assign(b, b + (size_t)width()*height()); }
  void endAttitude() {
    uint8_t *b = getBuffer();
    int LW = width(), LH = height();      // logical (rotated) dims
    int NW = WIDTH, NH = HEIGHT;          // native buffer dims
    uint8_t rot = getRotation();
    for (int y = 0; y < LH; y++)
      for (int x = 0; x < LW; x++) {
        long idx;
        switch (rot) {                    // logical (x,y) -> raw buffer index
          case 1:  idx = (long)(NW-1-y) + (long)x * NW; break;
          case 2:  idx = (long)(NW-1-x) + (long)(NH-1-y) * NW; break;
          case 3:  idx = (long)y + (long)(NH-1-x) * NW; break;
          default: idx = (long)x + (long)y * NW; break;   // 0
        }
        if (b[idx] != bufBefore[idx]) push('X', x, y, 0, 0, b[idx]);
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

// Radar clip circle (set by drawChart) — map lines/rings are clipped to it in SVG.
int gClipX = 0, gClipY = 0, gClipR = 0;
void svgSetClip(int cx, int cy, int rad) { gClipX = cx; gClipY = cy; gClipR = rad; }

// Map geometry as clean vectors (clipped to the radar circle), instead of pixels.
void svgVecLine(MyCanvas8 *c, int x0, int y0, int x1, int y1, uint8_t col, bool dashed) {
  SvgCanvas *s = static_cast<SvgCanvas *>(c);
  if (!s->recording) return;
  Elem e; e.kind = 'M'; e.a = x0; e.b = y0; e.c = x1; e.d = y1; e.dashed = dashed; e.col = col;
  s->elems.push_back(std::move(e));
}
void svgVecRingDashed(MyCanvas8 *c, int cx, int cy, int rr, uint8_t col) {
  SvgCanvas *s = static_cast<SvgCanvas *>(c);
  if (!s->recording) return;
  Elem e; e.kind = 'O'; e.a = cx; e.b = cy; e.c = rr; e.dashed = true; e.col = col;
  s->elems.push_back(std::move(e));
}

#include "instrument_drawer.ino"
#undef p     // the drawer's '#define p 3.1415926' must not leak into our code below
#undef min   // Arduino shim's min()/max() macros would break std::min/std::max
#undef max

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
      case 'M':   // map polyline segment, clipped to the radar circle
        snprintf(buf, sizeof buf,
          "<line x1=\"%g\" y1=\"%g\" x2=\"%g\" y2=\"%g\" stroke=\"%s\"%s clip-path=\"url(#ndclip)\"/>\n",
          e.a+0.5f, e.b+0.5f, e.c+0.5f, e.d+0.5f, col.c_str(),
          e.dashed ? " stroke-dasharray=\"3 3\"" : ""); o += buf; break;
      case 'O':   // map distance ring (dotted), clipped to the radar circle
        snprintf(buf, sizeof buf,
          "<circle cx=\"%g\" cy=\"%g\" r=\"%g\" fill=\"none\" stroke=\"%s\" "
          "stroke-dasharray=\"2 3\" clip-path=\"url(#ndclip)\"/>\n",
          e.a+0.5f, e.b+0.5f, e.c, col.c_str()); o += buf; break;
      case 'T': {  // text glyph
        std::string t = e.text; std::string esc;
        for (char ch : t) { if (ch=='&') esc+="&amp;"; else if (ch=='<') esc+="&lt;"; else if (ch=='>') esc+="&gt;"; else esc+=ch; }
        if (e.ang != 0.0f)
          snprintf(buf, sizeof buf,
            "<text x=\"%g\" y=\"%g\" font-family=\"monospace\" font-size=\"%g\" fill=\"%s\" "
            "transform=\"rotate(%g %g %g)\">%s</text>\n",
            e.a, e.b, e.fs, col.c_str(), e.ang, e.a, e.b, esc.c_str());
        else
          snprintf(buf, sizeof buf,
            "<text x=\"%g\" y=\"%g\" font-family=\"monospace\" font-size=\"%g\" fill=\"%s\">%s</text>\n",
            e.a, e.b, e.fs, col.c_str(), esc.c_str());
        o += buf; break;
      }
    }
  }
}

struct Group { std::vector<Elem> els; float dx, dy; };

static void writeSvg(const char *path, int W, int H, const std::vector<Group> &groups) {
  std::string o;
  char hdr[256];
  snprintf(hdr, sizeof hdr,
    "<svg viewBox=\"0 0 %d %d\" xmlns=\"http://www.w3.org/2000/svg\" stroke-width=\"1\">\n", W, H);
  o += hdr;
  o += "<defs>";
  snprintf(hdr, sizeof hdr, "<clipPath id=\"vp\"><rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/></clipPath>", W, H);
  o += hdr;
  if (gClipR > 0) {
    snprintf(hdr, sizeof hdr, "<clipPath id=\"ndclip\"><circle cx=\"%d\" cy=\"%d\" r=\"%d\"/></clipPath>", gClipX, gClipY, gClipR);
    o += hdr;
  }
  o += "</defs>\n";
  o += "<rect x=\"0\" y=\"0\" width=\"" + std::to_string(W) + "\" height=\"" +
       std::to_string(H) + "\" fill=\"#000000\"/>\n";
  o += "<g clip-path=\"url(#vp)\">\n";   // nothing overflows the panel edges
  for (const Group &g : groups) {
    char gt[64];
    snprintf(gt, sizeof gt, "<g transform=\"translate(%g %g)\">\n", g.dx, g.dy);
    o += gt;
    emitElems(o, coalesce(g.els));
    o += "</g>\n";
  }
  o += "</g></svg>\n";
  FILE *f = fopen(path, "w");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
  fwrite(o.data(), 1, o.size(), f); fclose(f);
  fprintf(stderr, "wrote %s (%zu bytes)\n", path, o.size());
}

// ---------------------------------------------------------------------------
//  Annotated ND legend  (single-panel config only)
// ---------------------------------------------------------------------------
#if COMBINED_DISPLAY
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
  o += "<defs>";
  snprintf(buf, sizeof buf, "<clipPath id=\"ndframe\"><rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"/></clipPath>", ox, oy, ndW, ndH);
  o += buf;
  if (gClipR > 0) {
    snprintf(buf, sizeof buf, "<clipPath id=\"ndclip\"><circle cx=\"%d\" cy=\"%d\" r=\"%d\"/></clipPath>", gClipX, gClipY, gClipR);
    o += buf;
  }
  o += "</defs>\n";
  snprintf(buf, sizeof buf, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"#000000\"/>\n", LW, LH);
  o += buf;
  // dim the busy map so the bright callouts/leaders/labels stand out on top
  snprintf(buf, sizeof buf, "<g clip-path=\"url(#ndframe)\"><g transform=\"translate(%d %d) scale(%g)\" opacity=\"0.7\">\n", ox, oy, f);
  o += buf;
  emitElems(o, coalesce(nd));
  o += "</g></g>\n";
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
#endif  // COMBINED_DISPLAY (legend)

// ---------------------------------------------------------------------------
//  Dual-display layout: two rounded-corner viewports (Waveshare 1.69" screens)
// ---------------------------------------------------------------------------
#if !COMBINED_DISPLAY
static void genDual(const char *path, const std::vector<Elem> &pe, int pw, int ph,
                    const std::vector<Elem> &ne, int nw, int nh) {
  const double f = 1.3;
  const int rx = (int)(15 * f);            // screen corner radius
  const int bez = 11;                       // bezel thickness
  const int margin = 40, gap = 56, labelH = 46;
  const int pW = (int)(pw*f), pH = (int)(ph*f), nW = (int)(nw*f), nH = (int)(nh*f);
  const int contentH = std::max(pH, nH);
  const int LW = margin + pW + gap + nW + margin;
  const int LH = margin + contentH + labelH;
  const int pX = margin,                pY = margin + (contentH - pH) / 2;
  const int nX = margin + pW + gap,     nY = margin + (contentH - nH) / 2;

  std::string o; char buf[1024];
  snprintf(buf, sizeof buf,
    "<svg viewBox=\"0 0 %d %d\" xmlns=\"http://www.w3.org/2000/svg\" "
    "font-family=\"Helvetica,Arial,sans-serif\" stroke-width=\"1\">\n", LW, LH);
  o += buf;
  o += "<defs>\n";
  if (gClipR > 0) {
    snprintf(buf, sizeof buf, "<clipPath id=\"ndclip\"><circle cx=\"%d\" cy=\"%d\" r=\"%d\"/></clipPath>\n", gClipX, gClipY, gClipR);
    o += buf;
  }
  snprintf(buf, sizeof buf, "<clipPath id=\"clipP\"><rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\"/></clipPath>\n", pX, pY, pW, pH, rx); o += buf;
  snprintf(buf, sizeof buf, "<clipPath id=\"clipN\"><rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\"/></clipPath>\n", nX, nY, nW, nH, rx); o += buf;
  o += "</defs>\n";
  snprintf(buf, sizeof buf, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"#0d0d0d\"/>\n", LW, LH); o += buf;
  // bezels (rounded rects around each screen, matching the Waveshare panels)
  snprintf(buf, sizeof buf, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" fill=\"#000\" stroke=\"#2c2c2c\" stroke-width=\"%d\"/>\n", pX-bez, pY-bez, pW+2*bez, pH+2*bez, rx+bez, bez); o += buf;
  snprintf(buf, sizeof buf, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" fill=\"#000\" stroke=\"#2c2c2c\" stroke-width=\"%d\"/>\n", nX-bez, nY-bez, nW+2*bez, nH+2*bez, rx+bez, bez); o += buf;
  // PFD screen (content clipped to the rounded screen)
  snprintf(buf, sizeof buf, "<g clip-path=\"url(#clipP)\"><g transform=\"translate(%d %d) scale(%g)\">\n", pX, pY, f); o += buf;
  emitElems(o, coalesce(pe));
  o += "</g></g>\n";
  // ND screen
  snprintf(buf, sizeof buf, "<g clip-path=\"url(#clipN)\"><g transform=\"translate(%d %d) scale(%g)\">\n", nX, nY, f); o += buf;
  emitElems(o, coalesce(ne));
  o += "</g></g>\n";
  // captions
  const int ly = margin + contentH + 30;
  snprintf(buf, sizeof buf, "<text x=\"%d\" y=\"%d\" fill=\"#c8c8c8\" font-size=\"19\" text-anchor=\"middle\">Primary Flight Display</text>\n", pX + pW/2, ly); o += buf;
  snprintf(buf, sizeof buf, "<text x=\"%d\" y=\"%d\" fill=\"#c8c8c8\" font-size=\"19\" text-anchor=\"middle\">Navigation Display</text>\n", nX + nW/2, ly); o += buf;
  o += "</svg>\n";

  FILE *fp = fopen(path, "w");
  if (!fp) { fprintf(stderr, "cannot open %s\n", path); return; }
  fwrite(o.data(), 1, o.size(), fp); fclose(fp);
  fprintf(stderr, "wrote %s (%zu bytes)\n", path, o.size());
}
#endif

// ---------------------------------------------------------------------------
int main() {
  // A representative in-flight state: wings level (so the attitude horizon is a
  // clean horizontal split — pixelated but not aliased), low pattern altitude.
  state s; init_state(&s);
  s.IMU = true; s.BPS = true; s.ASI = true; s.GPS = true;
  s.gx = 0.0f; s.gy = -1.0f; s.gz = 0.0f;          // wings level, no pitch
  s.g = 1.0f; s.max_g = 1.2f;
  s.ax = 0.0f; s.ay = -1.0f; s.az = 0.0f;          // slip ball centred
  s.air_speed = 110;
  s.alt = 850; s.home_alt = 480; s.vertical_speed = 0;   // 850 ft, level
  s.heading = 75; s.ground_track = 82; s.ground_speed = 118;   // ~7 deg crab -> track distinct
  s.lat = MAP_DEFAULT_LAT; s.lon = MAP_DEFAULT_LON;
  s.last_lat = MAP_DEFAULT_LAT; s.last_lon = MAP_DEFAULT_LON;   // ND centres on last_*
  s.home_lat = MAP_DEFAULT_LAT + 0.04f; s.home_lon = MAP_DEFAULT_LON + 0.06f;  // home NE -> green line visible
  s.gps_alt = 850; s.has_pos = true; s.sats = 11;

#if COMBINED_DISPLAY
  // ---- Single-panel config (BOARD_C / BOARD_D): PFD over ND ----
  static GFXcanvas8 incmap = generate_inc_map(0.6 * PFD_REGION_H, lyt::txtScale(RGB_WIDTH));
  SvgCanvas pfd(RGB_WIDTH, PFD_REGION_H);
  drawHorizonDisplay(&pfd, &incmap, &s, /*showCompass*/ false);
  SvgCanvas nd(RGB_WIDTH, ND_CANVAS_H);
  drawNavigationDisplay(&nd, &s);

  writeSvg("docs/pfd.svg", RGB_WIDTH, PFD_REGION_H, {{pfd.elems, 0, 0}});
  writeSvg("docs/nd.svg",  RGB_WIDTH, ND_CANVAS_H, {{nd.elems, 0, 0}});
  writeSvg("docs/combined.svg", RGB_WIDTH, RGB_HEIGHT,
           {{pfd.elems, 0, 0}, {nd.elems, 0, ND_TOP}});
  genLegend("docs/nd_legend.svg", nd.elems);
#else
  // ---- Dual-display config (BOARD_A): two separate ST7789 screens ----
  SvgCanvas pfd(SCREEN1_WIDTH, SCREEN1_HEIGHT);
  pfd.setRotation(PFD_CANVAS_ROTATION);
  static GFXcanvas8 incmap = generate_inc_map(0.6 * pfd.height(), lyt::txtScale(pfd.width()));
  drawHorizonDisplay(&pfd, &incmap, &s, /*showCompass*/ true);
  SvgCanvas nd(SCREEN2_WIDTH, SCREEN2_HEIGHT);
  nd.setRotation(ND_CANVAS_ROTATION);
  drawNavigationDisplay(&nd, &s);

  genDual("docs/dual.svg", pfd.elems, pfd.width(), pfd.height(), nd.elems, nd.width(), nd.height());
#endif
  return 0;
}
