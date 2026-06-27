// ============================================================================
//  map_zoom.cpp — the power-of-2 zoom ladder + field-mode state machine.
//  (A .cpp so it is its own translation unit; the globals are defined here for
//   the device build. The svggen host build defines its own copies — see
//   tools/svggen — since it never zooms.)
// ============================================================================
#include "map_zoom.h"
#include "config.h"

volatile int     gMapRangeM      = MAP_RANGE_M;   // overwritten by mapZoomInit()
volatile uint8_t gMapLod         = 6;
volatile bool    gMapFieldActive = false;
volatile float   gMapFieldLat    = 0.0f;
volatile float   gMapFieldLon    = 0.0f;

// Power-of-2 ladder (range halves per zoom-in step), ascending so index 0 is the most
// zoomed IN. lod selects the per-band dataset (chart_data.h LOD_*[lod]); one band per
// step adds detail. Below FIELD_THRESH_M the map basemap restarts at range*FIELD_MAP_MULT
// (= 80 km) and zooms in from there (the lod here is already that basemap's lod), while
// the range rings / Remote ID stay at the REAL range. Floor = 156 m.
struct ZoomLevel { int range_m; uint8_t lod; };
static const ZoomLevel LEVELS[] = {
  {     156, 6 },   // field: basemap 10 km  (zoom floor)
  {     312, 6 },   // field: basemap 20 km
  {     625, 6 },   // field: basemap 40 km
  {    1250, 5 },   // field: basemap 80 km
  {    2500, 6 },   // 1.35 NM  (BELOW here = field mode)
  {    5000, 6 },   // 2.7  NM
  {   10000, 6 },   // 5.4  NM
  {   20000, 6 },   // 11   NM
  {   40000, 6 },   // 22   NM  full detail: small airports + distance circles
  {   80000, 5 },   // 44   NM  + decommissioned airports, full road/river geometry
  {  160000, 4 },   // 88   NM  + medium airports, more roads, major landmarks
  {  320000, 3 },   // 176  NM  + airport models (strips/glides/rings), interstates
  {  640000, 2 },   // 352  NM  + major-airport locations, restricted airspace
  { 1280000, 1 },   // 691  NM  + major rivers, major cities
  { 2560000, 0 },   // 1382 NM  capitals + borders + water
};
static const int NLEV = (int)(sizeof(LEVELS) / sizeof(LEVELS[0]));
static int gIdx = MAP_ZOOM_DEFAULT_IDX;

static void apply() {
  gMapRangeM = LEVELS[gIdx].range_m;
  gMapLod    = LEVELS[gIdx].lod;
}

void mapZoomInit() {
  gIdx = MAP_ZOOM_DEFAULT_IDX;
  if (gIdx < 0) gIdx = 0;
  if (gIdx >= NLEV) gIdx = NLEV - 1;
  gMapFieldActive = false;
  apply();
}

void mapZoom(int dir, float curLat, float curLon) {
  int ni = gIdx + (dir < 0 ? -1 : +1);
  if (ni < 0) ni = 0;
  if (ni >= NLEV) ni = NLEV - 1;
  if (ni == gIdx) return;                       // already at the end stop

  bool wasField = LEVELS[gIdx].range_m < FIELD_THRESH_M;
  bool nowField = LEVELS[ni].range_m  < FIELD_THRESH_M;
  if (nowField && !wasField) {                  // crossing IN: freeze the field point P
    gMapFieldLat = curLat;
    gMapFieldLon = curLon;
    gMapFieldActive = true;
  } else if (!nowField) {                       // back above the threshold: live GPS again
    gMapFieldActive = false;
  }
  gIdx = ni;
  apply();
}
