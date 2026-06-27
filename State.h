#pragma once
// ============================================================================
//  State.h — the shared flight-state snapshot.
//
//  This is plain-old-data: every field is a scalar, so the whole struct can be
//  copied with a single assignment. The sensor task fills one of these and
//  hands it to the display tasks through the mutex-guarded global `State`
//  (see getState()/setState() in the main sketch).
// ============================================================================

#ifndef RID_MAX
#define RID_MAX 12                 // max simultaneously-tracked Remote ID targets
#endif
struct rid_target { float lat, lon; int alt_ft; };

struct state {
  // --- sensor presence flags ------------------------------------------------
  bool  IMU, BPS, ASI, GPS;

  // --- attitude: normalized gravity ("down") vector in the display frame ----
  float gx, gy, gz;

  // --- g-force --------------------------------------------------------------
  float g, max_g;                  // current load factor, peak since power-on

  // --- accelerometer (m/s^2, reordered into the display frame) --------------
  float ax, ay, az;

  // --- magnetometer (uT, reordered into the display frame) ------------------
  float mx, my, mz;
  float heading;                   // tilt-compensated magnetic heading (deg, 0..360)

  // --- barometric altitude --------------------------------------------------
  float alt, home_alt;             // current MSL altitude (ft), ground reference
  float vertical_speed;            // ft/min (climb positive)

  // --- speeds ---------------------------------------------------------------
  float ground_speed, air_speed;   // GPS ground speed (kt), indicated airspeed (mph)
  float ground_track;              // GPS course over ground (deg, 0..360)

  // --- GPS position ---------------------------------------------------------
  float lat, lon, gps_alt;
  float home_lat, home_lon, home_gps_alt;
  float last_lat, last_lon;        // last position with a GPS lock; persists to NVS,
  bool  has_pos;                   //   used as the map center when GPS is lost
  int   sats;                      // GPS satellites in use (from GGA)

  // --- Remote ID traffic (ASTM F3411 / OpenDroneID) -------------------------
  // Drones/aircraft broadcasting their position + altitude over BLE / WiFi,
  // received by RemoteID.ino. alt_ft is height AGL (Remote ID is capped 400 ft).
  rid_target rid[RID_MAX];
  int        n_rid;                // entries in rid[] that have a position (drawn as dots)
  int        n_rid_seen;           // total Remote ID targets being tracked (drives PROXIMITY)
};

// Zero every field, then apply "level and stationary" defaults so the PFD draws
// an upright horizon before the first IMU sample arrives.
inline void init_state(state *a) {
  *a = state{};        // value-initialize: all fields 0 / false
  a->gy = -1.0f;       // gravity points "down" in the body frame
  a->ay = -9.8f;
  a->mz =  1.0f;
}

// `state` is POD, so a single assignment copies it correctly and never drifts
// when fields are added or removed.
inline void copy_state(state *dst, state *src) { *dst = *src; }
