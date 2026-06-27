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

// ============================================================================
//  Flat per-LOD 2-D grid cull (the numerous point-type arrays: points/rings/
//  runways). Each LOD stores its features sorted by row-major grid cell, with a
//  cellOff[] prefix table (chart_data.h LOD_*_CELL[lod]); cell size ~ that LOD's
//  view radius (LOD_GRID_DEG[lod]). The view box (clat/clon ± dLatMax/dLonMax)
//  maps to a small rectangle of cells, so per-frame cost tracks what's ON SCREEN
//  — O(visible cells + features in them), not the country-wide latitude strip the
//  old lat-band scan walked. MARGIN adds cells of reach for features whose drawn
//  geometry extends past their indexed point (e.g. distance rings).
//  GRID_LA0 / GRID_LO0 / LOD_GRID_* come from chart_data.h (included first).
// ============================================================================
static inline int chartGridX(float lon, float deg, int nx, int d) {
  int c = (int)floorf((lon - GRID_LO0) / deg) + d;
  return c < 0 ? 0 : (c >= nx ? nx - 1 : c);
}
static inline int chartGridY(float lat, float deg, int ny, int d) {
  int c = (int)floorf((lat - GRID_LA0) / deg) + d;
  return c < 0 ? 0 : (c >= ny ? ny - 1 : c);
}
// Open a scoped block computing the visible cell rectangle for LOD/MARGIN. Needs
// clat/clon/dLatMax/dLonMax in scope. Pair with CHART_GRID_END.
#define CHART_GRID_BEGIN(LOD, MARGIN) {                                          \
    const float _gd = LOD_GRID_DEG[LOD]; const int _gnx = LOD_GRID_NX[LOD];      \
    const int _gx0 = chartGridX(clon - dLonMax, _gd, _gnx, -(MARGIN));           \
    const int _gx1 = chartGridX(clon + dLonMax, _gd, _gnx,  (MARGIN));           \
    const int _gy0 = chartGridY(clat - dLatMax, _gd, LOD_GRID_NY[LOD], -(MARGIN));\
    const int _gy1 = chartGridY(clat + dLatMax, _gd, LOD_GRID_NY[LOD],  (MARGIN));
// Walk the cell rectangle: features in one cell-row are contiguous in OFF. IDX is
// the feature index. Follow with a { ... } body (continue == next feature).
#define CHART_GRID_ROWS(OFF, IDX)                                               \
    for (int _cy = _gy0; _cy <= _gy1; _cy++)                                    \
      for (int IDX = (OFF)[_cy*_gnx+_gx0], _ge = (OFF)[_cy*_gnx+_gx1+1]; IDX < _ge; IDX++)
#define CHART_GRID_END }
