#pragma once
// ============================================================================
//  layout.h — resolution-independent layout math for the instrument drawers.
//
//  Pure functions, NO Arduino/GFX dependencies, so they can be host unit-tested
//  (tests/test_layout.cpp) and reused by drawHorizonDisplay/drawNavigationDisplay
//  at any canvas size. The reference design size is the original BOARD_A PFD,
//  280x240 (REF_W x REF_H): at that size every helper reproduces the original
//  hand-tuned numbers, which the unit tests verify so the dual-display build can
//  never regress.
//
//  Two ideas make a drawer scale on its own:
//    1. Sizes/offsets are expressed against the reference and multiplied by an
//       integer text scale derived from the canvas width (txtScale). At the
//       reference width that factor is 1, so scaled(px) == px.
//    2. Text is *placed* from its measured bounds (getTextBounds) via alignCursor,
//       so a label sits correctly regardless of how big the glyphs actually are.
// ============================================================================

namespace lyt {

constexpr int REF_W = 280;   // reference design width  (original PFD canvas)
constexpr int REF_H = 240;   // reference design height

// Integer text magnification for a canvas of the given width (1x at REF_W,
// rounding up at the half-step: 280->1, 480->2, 700->3).
inline int txtScale(int width) {
  int s = (width + REF_W / 2) / REF_W;   // round(width / REF_W)
  return s < 1 ? 1 : s;
}

// A reference-design pixel length scaled to this canvas. Tracks the glyph size so
// gaps/insets grow with the text. Equals `px` at the reference width.
inline int scaled(int px, int width) { return px * txtScale(width); }

// Text box alignment about an anchor point.
enum HAlign { HL, HC, HR };   // anchor at the text's left / center / right edge
enum VAlign { VT, VC, VB };   // anchor at the text's top  / middle / bottom edge

// Convert an anchor (ax,ay) + a measured glyph box (bx,by,bw,bh, as returned by
// Adafruit_GFX::getTextBounds for cursor (0,0)) into the setCursor() position that
// lands the box on the anchor with the requested alignment.
inline void alignCursor(int ax, int ay, int bx, int by, int bw, int bh,
                        HAlign h, VAlign v, int &cx, int &cy) {
  switch (h) {
    case HL: cx = ax - bx;            break;
    case HC: cx = ax - bx - bw / 2;   break;
    case HR: cx = ax - bx - bw;       break;
  }
  switch (v) {
    case VT: cy = ay - by;            break;
    case VC: cy = ay - by - bh / 2;   break;
    case VB: cy = ay - by - bh;       break;
  }
}

// Analytic bounds of the built-in 6x8 GFX font for an N-char string at size s.
// Mirrors what getTextBounds returns for the classic font (origin 0,0), so the
// tests can predict text positions without linking the font library.
inline void classicBounds(int nchars, int s, int &bx, int &by, int &bw, int &bh) {
  bx = 0;
  by = 0;
  bw = nchars * 6 * s;
  bh = 8 * s;
}

}  // namespace lyt
