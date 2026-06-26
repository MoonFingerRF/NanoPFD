// ============================================================================
//  test_layout.cpp — host unit tests for layout.h
//
//  Build + run:  g++ -std=c++11 tests/test_layout.cpp -o /tmp/test_layout && /tmp/test_layout
//
//  The key guarantee for the dual-display (BOARD_A) build: at the reference width
//  (280) txtScale()==1 and scaled(px)==px, so every size/offset the drawers feed
//  through these helpers is byte-identical to the original hand-tuned value. The
//  alignCursor tests pin the text-placement math.
// ============================================================================
#include "../layout.h"
#include <cstdio>
using namespace lyt;

static int fails = 0;
#define EQ(a, b, msg) do { long _a=(a),_b=(b); \
  if (_a!=_b){ printf("FAIL: %-46s got %ld want %ld\n", msg,_a,_b); fails++; } \
  else        printf("ok  : %s\n", msg); } while(0)

int main() {
  // ---- txtScale: 1x at the reference, rounds up at the half-step ------------
  EQ(txtScale(280), 1, "txtScale(280)=1  (reference PFD)");
  EQ(txtScale(240), 1, "txtScale(240)=1  (rotated ND / small)");
  EQ(txtScale(480), 2, "txtScale(480)=2  (combined 2.8B)");
  EQ(txtScale(420), 2, "txtScale(420)=2  (round up at 1.5x)");
  EQ(txtScale(700), 3, "txtScale(700)=3");
  EQ(txtScale(10),  1, "txtScale(tiny)>=1");

  // ---- scaled: IDENTITY at the reference (this is the no-regression pin) ----
  EQ(scaled(20, 280),  20, "scaled(20,280)=20   (offset unchanged at ref)");
  EQ(scaled(-3, 280),  -3, "scaled(-3,280)=-3   (vcenter unchanged at ref)");
  EQ(scaled(19, 280),  19, "scaled(19,280)=19   (GS divider unchanged at ref)");
  EQ(scaled(10, 280),  10, "scaled(10,280)=10   (GS row gap unchanged at ref)");
  EQ(scaled(20, 480),  40, "scaled(20,480)=40   (doubles on combined)");
  EQ(scaled(-3, 480),  -6, "scaled(-3,480)=-6   (vcenter doubles)");

  // ---- classicBounds: matches the built-in 6x8 font -----------------------
  int bx, by, bw, bh, cx, cy;
  classicBounds(3, 1, bx, by, bw, bh);
  EQ(bw, 18, "classicBounds(3,1) w=18");
  EQ(bh,  8, "classicBounds(3,1) h=8");
  classicBounds(3, 2, bx, by, bw, bh);
  EQ(bw, 36, "classicBounds(3,2) w=36 (2x)");
  EQ(bh, 16, "classicBounds(3,2) h=16 (2x)");

  // ---- alignCursor: anchor at (100,100), classic 3-char box (18x8) ----------
  classicBounds(3, 1, bx, by, bw, bh);
  alignCursor(100, 100, bx, by, bw, bh, HL, VT, cx, cy);
  EQ(cx, 100, "HL: left edge at anchor");
  EQ(cy, 100, "VT: top edge at anchor");
  alignCursor(100, 100, bx, by, bw, bh, HR, VB, cx, cy);
  EQ(cx, 82, "HR: right edge at anchor (100-18)");
  EQ(cy, 92, "VB: bottom edge at anchor (100-8)");
  alignCursor(100, 100, bx, by, bw, bh, HC, VC, cx, cy);
  EQ(cx, 91, "HC: centered (100-9)");
  EQ(cy, 96, "VC: middle (100-4)");

  // alignCursor honors a non-zero glyph origin (custom fonts report bx/by).
  alignCursor(100, 100, -2, -5, 18, 8, HL, VT, cx, cy);
  EQ(cx, 102, "HL with bx=-2 compensates origin");
  EQ(cy, 105, "VT with by=-5 compensates origin");

  // ---- reference equivalence: a right-aligned tape number at width 280 ------
  // NEW: anchor = (tape edge) - scaled(2), right-align a 3-digit number.
  // OLD: cursor_x = (tape edge) - 20  (3-digit @ size 1 is 18 wide + 2 gap).
  {
    int edge = 137;
    classicBounds(3, 1, bx, by, bw, bh);
    alignCursor(edge - scaled(2, 280), 0, bx, by, bw, bh, HR, VT, cx, cy);
    EQ(cx, edge - 20, "speed tape 3-digit: new == old (edge-20) at 280");
  }

  if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
  printf("\nALL TESTS PASS\n");
  return 0;
}
