#pragma once
// ============================================================================
//  Touch.h — capacitive-touch zoom input for the two touch boards.
//    BOARD_C (2.8B):  GT911  @ I2C 0x14 on the sensor bus (15/7)
//    BOARD_D (AMOLED): CST226 @ I2C 0x5A on the sensor bus (6/7)
//  Top half of the display = zoom IN, bottom half = zoom OUT (see map_zoom.h).
//  BOARD_A has no touch — both calls compile to no-ops.
// ============================================================================
struct state;
void touchInit();                 // call once after Wire.begin()
void touchPoll(struct state *s);  // poll from the sensor task; s gives the live map centre
