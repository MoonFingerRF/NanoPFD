// ============================================================================
//  ICM.ino — ICM-20948 9-DOF IMU (e.g. the GY-912 module) via its on-chip DMP.
//
//  All the motion processing happens ON THE IMU: the ICM-20948's Digital Motion
//  Processor runs the sensor fusion (accel + gyro + AK09916 magnetometer) and
//  outputs a single magnetometer-referenced orientation quaternion (the 9-axis
//  "Game Rotation Vector", Quat9). The host does NO Kalman / complementary
//  filtering — it just reads the fused quaternion and reads attitude + a
//  magnetometer-referenced heading straight out of it.
//
//  Used as a PRIMARY IMU (IMU.ino failover tries it after the BNO08x, before the
//  QMI). Shares the sensor I2C bus; the DMP owns the AK09916 via the ICM's
//  internal I2C master (so we do NOT bypass to the mag ourselves).
//
//  Requires the SparkFun ICM-20948 library with DMP support enabled
//  (#define ICM_20948_USE_DMP in src/util/ICM_20948_C.h — already set here).
//
//  TUNE: the sensor->display axis map (the s->gx/gy/gz lines) and the heading
//  ICM_HEADING_SIGN / ICM_HEADING_OFFSET depend on how the board is mounted.
// ============================================================================

#if ENABLE_ICM20948
#include <ICM_20948.h>

ICM_20948_I2C icm20948;
bool          icm_present   = false;
unsigned long icm_last_data = 0;

// Detect the ICM (I2C 0x69 with AD0 high, else 0x68) and start its DMP streaming
// the 9-axis (magnetometer-referenced) orientation quaternion. True if found.
bool icmBegin() {
  icm_present = false;
  for (int ad0 = 1; ad0 >= 0; ad0--) {
    if (icm20948.begin(Wire, ad0) != ICM_20948_Stat_Ok) continue;
    // DMP bring-up: load the firmware, enable the 9-axis orientation sensor, run
    // it at the max rate, and start the FIFO. The library wires the AK09916 into
    // the DMP for us, so the quaternion is fully fused on-chip.
    if (icm20948.initializeDMP() != ICM_20948_Stat_Ok) continue;
    bool ok = true;
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_ORIENTATION) == ICM_20948_Stat_Ok);
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Quat9, 0) == ICM_20948_Stat_Ok);  // 0 = max rate
    ok &= (icm20948.enableFIFO()  == ICM_20948_Stat_Ok);
    ok &= (icm20948.enableDMP()   == ICM_20948_Stat_Ok);
    ok &= (icm20948.resetDMP()    == ICM_20948_Stat_Ok);
    ok &= (icm20948.resetFIFO()   == ICM_20948_Stat_Ok);
    if (!ok) continue;
    icm_present = true;
    return true;
  }
  return false;
}

// Drain the DMP FIFO (keep the freshest packet) and fill the shared state from the
// fused quaternion. Returns true when a 9-axis sample arrived. Only call this when
// the ICM is the active source (IMU.ino).
bool icmUpdate(state *s) {
  icm_20948_DMP_data_t d;
  float q1 = 0, q2 = 0, q3 = 0;
  bool got = false;
  int guard = 0;
  do {
    icm20948.readDMPdataFromFIFO(&d);
    if ((icm20948.status == ICM_20948_Stat_Ok || icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail)
        && (d.header & DMP_header_bitmap_Quat9)) {
      // Q1..Q3 are the vector part, Q30 fixed-point; q0 is recovered from |q| = 1.
      q1 = (float)d.Quat9.Data.Q1 / 1073741824.0f;
      q2 = (float)d.Quat9.Data.Q2 / 1073741824.0f;
      q3 = (float)d.Quat9.Data.Q3 / 1073741824.0f;
      got = true;
    }
  } while (icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail && ++guard < 32);
  if (!got) return false;

  float s2 = 1.0f - (q1 * q1 + q2 * q2 + q3 * q3);
  float q0 = s2 > 0 ? sqrt(s2) : 0;

  // Gravity-reaction "up" direction in the sensor body frame (3rd row of the
  // rotation matrix). At rest, level, board flat -> (0, 0, 1).
  float ux = 2 * (q1 * q3 - q0 * q2);
  float uy = 2 * (q0 * q1 + q2 * q3);
  float uz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

  // ---- sensor body frame -> display frame (mounting knobs in config.h) ------
  // Default: roll<-sensorX, pitch<-sensorY, vertical<-(-sensorZ) so level reads
  // (0, -1, 0). The ICM_FLIP_* / ICM_SWAP_ROLL_PITCH defines flip whatever the
  // physical mounting reads backwards.
  float roll = ux, pitch = uy, vert = uz;        // bank / pitch / gravity-up (level: vert = +1)
#if ICM_SWAP_ROLL_PITCH
  { float t = roll; roll = pitch; pitch = t; }
#endif
  s->gx = (ICM_FLIP_ROLL)     ? -roll  :  roll;
  s->gy = (ICM_FLIP_VERTICAL) ?  vert  : -vert;
  s->gz = (ICM_FLIP_PITCH)    ? -pitch :  pitch;
  // The DMP gives a unit gravity direction (not specific force), so we can't read
  // true load factor / lateral slip from it. Hold g at 1 and point the slip-ball
  // accel at the same gravity vector (so the ball tracks bank). TODO: enable a raw
  // DMP accel sensor if a live g-meter / slip ball is wanted.
  s->ax = s->gx; s->ay = s->gy; s->az = s->gz;
  s->g  = s->g * (1 - ALPHA_GFORCE) + ALPHA_GFORCE * 1.0f;

  // ---- heading: yaw of the magnetometer-referenced quaternion (absolute) -----
  float yaw = atan2(2 * (q0 * q3 + q1 * q2), 1 - 2 * (q2 * q2 + q3 * q3)) * 180.0f / PI;
  float h = ICM_HEADING_SIGN * yaw + ICM_HEADING_OFFSET;
  while (h < 0)       h += 360.0f;
  while (h >= 360.0f) h -= 360.0f;
  s->heading = h;

  icm_last_data = millis();
  return true;
}

#else   // ENABLE_ICM20948 == 0  ->  inert stubs so the rest of the code links
bool          icm_present   = false;
unsigned long icm_last_data = 0;
bool icmBegin()          { return false; }
bool icmUpdate(state *s) { return false; }
#endif
