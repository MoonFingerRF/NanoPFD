// ============================================================================
//  ICM.ino — ICM-20948 9-DOF IMU via its on-chip DMP (Digital Motion Processor)
//
//  Like the BNO08x, the ICM-20948's DMP outputs a fully fused orientation
//  quaternion, so NO host-side Kalman/complementary filtering is needed. It is
//  used as a primary IMU: tried after the BNO, before the QMI (Mahony) fallback.
//
//  ---- BRING-UP (one-time, required) ----------------------------------------
//   1. "SparkFun 9DoF IMU Breakout - ICM 20948" library installed (done).
//   2. The DMP ships DISABLED. Edit the library file
//        .../SparkFun_..._ICM_20948_.../src/util/ICM_20948_C.h
//      and uncomment:   #define ICM_20948_USE_DMP
//   3. Set ENABLE_ICM20948 to 1 in config.h, then flash.
//
//  NOTE: untested on hardware here (no ICM connected). The sensor->display axis
//  remap and heading sign below are a first guess matching the BNO's frame --
//  expect to flip a sign/swap once an ICM is mounted (see the marked lines).
// ============================================================================

#if ENABLE_ICM20948
#include <ICM_20948.h>

ICM_20948_I2C icm20948;
bool          icm_present   = false;
unsigned long icm_last_data = 0;

// Detect the ICM (I2C 0x69 with AD0 high, else 0x68) and start its DMP for the
// 9-axis (mag-referenced) orientation quaternion. True if found + configured.
bool icmBegin() {
  icm_present = false;
  for (int ad0 = 1; ad0 >= 0; ad0--) {
    if (icm20948.begin(Wire, ad0) != ICM_20948_Stat_Ok) continue;
    if (icm20948.initializeDMP() != ICM_20948_Stat_Ok)  continue;
    icm20948.enableDMPSensor(INV_ICM20948_SENSOR_ORIENTATION);   // 9-axis quaternion
    icm20948.setDMPODRrate(DMP_ODR_Reg_Quat9, 0);                // max rate
    icm20948.enableFIFO();
    icm20948.enableDMP();
    icm20948.resetDMP();
    icm20948.resetFIFO();
    icm_present = true;
    return true;
  }
  return false;
}

// Pull the latest DMP quaternion and fill the shared state (gravity + heading +
// g). Returns true when a new sample arrived (the FIFO may be empty between
// calls). Only call this when the ICM is the active source.
bool icmUpdate(state *s) {
  icm_20948_DMP_data_t d;
  icm20948.readDMPdataFromFIFO(&d);
  if (!((icm20948.status == ICM_20948_Stat_Ok || icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail)
        && (d.header & DMP_header_bitmap_Quat9)))
    return false;

  // DMP quaternion is Q30 fixed point; q0 reconstructed from unit norm.
  float q1 = (float)d.Quat9.Data.Q1 / 1073741824.0f;
  float q2 = (float)d.Quat9.Data.Q2 / 1073741824.0f;
  float q3 = (float)d.Quat9.Data.Q3 / 1073741824.0f;
  float s2 = 1.0f - (q1*q1 + q2*q2 + q3*q3);
  float q0 = s2 > 0 ? sqrt(s2) : 0;

  // gravity-reaction ("up") direction in the sensor body frame
  float ux = 2*(q1*q3 - q0*q2);
  float uy = 2*(q0*q1 + q2*q3);
  float uz = q0*q0 - q1*q1 - q2*q2 + q3*q3;

  // ---- sensor frame -> display frame (TUNE for your mounting) --------------
  // mirrors the BNO's (x, z, y) remap; flip a sign / swap a pair if the horizon
  // or turn coordinator come out wrong.
  s->gx =  ux;
  s->gy = -uz;
  s->gz =  uy;
  s->ax = s->gx; s->ay = s->gy; s->az = s->gz;   // accel ~ gravity (DMP is fused)
  s->g  = s->g * (1 - ALPHA_GFORCE) + ALPHA_GFORCE * 1.0f;   // TODO: raw accel for live g

  // heading from yaw (absolute, mag-referenced via the 9-axis DMP)
  float yaw = atan2(2*(q0*q3 + q1*q2), 1 - 2*(q2*q2 + q3*q3)) * 180.0f / PI;
  if (yaw < 0) yaw += 360.0f;
  s->heading = yaw;                               // TUNE: offset/direction

  icm_last_data = millis();
  return true;
}

#else   // ENABLE_ICM20948 == 0  ->  inert stubs so the rest of the code links
bool          icm_present   = false;
unsigned long icm_last_data = 0;
bool icmBegin()          { return false; }
bool icmUpdate(state *s) { return false; }
#endif
