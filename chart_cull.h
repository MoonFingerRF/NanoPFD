#pragma once
// ============================================================================
//  chart_cull.h — fast latitude-band cull for the per-LOD moving-map data.
//  Each LOD's point/ring/runway array (chart_data.h) is emitted sorted by
//  latitude, so a per-frame loop binary-searches the first feature in the visible
//  latitude band and stops at the last — cost tracks what's ON SCREEN, not the LOD
//  size. (In a header, not the .ino, so the Arduino auto-prototype generator
//  doesn't choke on the template.)
// ============================================================================
template <typename T>
static inline int chartLatLower(const T *a, int n, float v) {
  int lo = 0, hi = n;
  while (lo < hi) { int m = (lo + hi) >> 1; if (a[m].lat < v) lo = m + 1; else hi = m; }
  return lo;
}
