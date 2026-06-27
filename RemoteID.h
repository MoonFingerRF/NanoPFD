#pragma once
// ============================================================================
//  RemoteID.h — passive FAA Remote ID (ASTM F3411 / OpenDroneID) receiver.
//
//  Listens for drone/UAS Remote ID broadcasts on the ESP32's Bluetooth LE and
//  WiFi radios and maintains a small table of nearby aircraft (position +
//  altitude). The ND plots them as orange dots. See RemoteID.ino.
// ============================================================================
#include "State.h"

// Start the BLE scanner + WiFi promiscuous receiver + the channel-hop task.
// Safe to call once from setup(); a no-op if RID_ENABLE is 0.
void remoteid_begin();

// Copy the currently-live targets (seen recently, with a position) into the
// shared state for the ND to draw. Ages out stale entries. Call periodically
// from the sensor task (it is cheap).
void remoteid_fill(state *s);
