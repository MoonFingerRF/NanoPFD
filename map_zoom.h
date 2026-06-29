#pragma once
// ============================================================================
//  map_zoom.h — runtime touch-zoom state for the ND moving map.
//
//  The ND used to be fixed-zoom (a compile-time MAP_RANGE_M). It is
//  now driven at runtime by touch: top half of the screen = zoom in, bottom half
//  = zoom out (see Touch.cpp). Range DOUBLES/HALVES per step, from 625 m out to
//  the whole US (~2560 km). gMapLod selects one of the pre-built per-LOD datasets
//  (chart_data.h LOD_*[lod]); each LOD has its own feature set + geometry resolution,
//  so context persists far out while staying cheap to draw.
//
//  Below FIELD_THRESH_M the real chart is basically empty (a small RC field), so
//  "field mode" renders the chart zoomed to FIELD_BASE_RANGE_M around a frozen
//  point P (the centre when you crossed the threshold) so there is context to
//  see — while the range rings and Remote ID dots stay at the REAL range (RID is
//  never magnified). See drawNavigationDisplay().
// ============================================================================
#include <stdint.h>

// ---- runtime state. Single writer (the touch poll in sensorTask), read by the
//      render tasks. Plain volatile is adequate for these coarse scalar values. -
extern volatile int     gMapRangeM;        // actual radar-edge range, metres (rings/RID)
extern volatile uint8_t gMapLod;           // which per-LOD dataset to draw (chart_data.h)
extern volatile bool    gMapFieldActive;   // field-magnification mode active
extern volatile float   gMapFieldLat;      // frozen field reference point ("fixed point P")
extern volatile float   gMapFieldLon;

#ifndef FIELD_THRESH_M
#define FIELD_THRESH_M      2500     // enter field mode below 2.5 km actual range
#endif
#ifndef FIELD_MAP_MULT
#define FIELD_MAP_MULT      64       // field basemap range = actual range x 64 (1.25 km -> 80 km)
#endif
#ifndef MAP_ZOOM_DEFAULT_IDX
#define MAP_ZOOM_DEFAULT_IDX 7        // boot view (= 20 km in the ladder)
#endif

void mapZoomInit();                  // set the boot zoom level (call once in setup)
// One zoom step: dir < 0 = zoom in (top-half tap), dir > 0 = zoom out (bottom-half).
// curLat/curLon = the live map centre, frozen as P when crossing into field mode.
void mapZoom(int dir, float curLat, float curLon);

// Config-portal helpers: jump to a level + query the ladder.
void mapZoomSet(int idx);
int  mapZoomIdx();
int  mapZoomCount();
int  mapZoomRangeM(int idx);
