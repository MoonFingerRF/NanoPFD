#pragma once
// ============================================================================
//  RemoteID.h — passive FAA Remote ID (ASTM F3411 / OpenDroneID) receiver.
//
//  Listens for drone/UAS Remote ID broadcasts on the ESP32's Bluetooth LE and
//  WiFi radios and maintains a small table of nearby aircraft (position +
//  altitude). The ND plots them as orange dots. See RemoteID.cpp.
// ============================================================================
#include "State.h"

// FLIGHT mode (no AP): start BLE and/or a standalone channel-hopping WiFi monitor per
// the runtime toggles (gRidBle / gRidWifi). Safe to call once; no-op if RID_ENABLE is 0.
void remoteid_begin();

// CONFIG mode (AP already up): start BLE and/or attach WiFi RID to the AP's radio (on its
// channel). Call AFTER webConfigBegin(). No-op if RID_ENABLE is 0.
void remoteid_begin_ap();

// Copy the currently-live targets (seen recently, with a position) into the
// shared state for the ND to draw. Ages out stale entries. Call periodically
// from the sensor task (it is cheap).
void remoteid_fill(state *s);
