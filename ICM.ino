// ============================================================================
//  ICM.ino — ICM-20948 9-DOF IMU (e.g. the GY-912 module) via its on-chip DMP.
//
//  All the motion processing happens ON THE IMU: the ICM-20948's Digital Motion
//  Processor runs the sensor fusion (accel + gyro + AK09916 magnetometer) and
//  outputs a single magnetometer-referenced orientation quaternion (the 9-axis
//  "Game Rotation Vector", Quat9). The host does NO Kalman / complementary
//  filtering — it just reads attitude + a magnetometer-referenced heading out of
//  the fused quaternion, plus the DMP's raw accelerometer (same FIFO) for a live
//  g-meter / slip ball.
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
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_RAW_ACCELEROMETER) == ICM_20948_Stat_Ok);  // g-meter / slip ball
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Quat9, 0) == ICM_20948_Stat_Ok);  // 0 = max rate
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Accel, 0) == ICM_20948_Stat_Ok);
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

// Drain the DMP FIFO (keep the freshest of each) and fill the shared state: the
// fused quaternion -> attitude + heading, and the raw accelerometer -> live g-meter
// + slip ball. Returns true when a sample arrived. Call only when the ICM is the
// active source (IMU.ino).
bool icmUpdate(state *s) {
  icm_20948_DMP_data_t d;
  float   q1 = 0, q2 = 0, q3 = 0;
  int16_t rax = 0, ray = 0, raz = 0;
  bool gotQ = false, gotA = false;
  int guard = 0;
  do {
    icm20948.readDMPdataFromFIFO(&d);
    if (icm20948.status == ICM_20948_Stat_Ok || icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail) {
      if (d.header & DMP_header_bitmap_Quat9) {   // Q1..Q3: vector part, Q30 fixed-point
        q1 = (float)d.Quat9.Data.Q1 / 1073741824.0f;
        q2 = (float)d.Quat9.Data.Q2 / 1073741824.0f;
        q3 = (float)d.Quat9.Data.Q3 / 1073741824.0f;
        gotQ = true;
      }
      if (d.header & DMP_header_bitmap_Accel) {    // raw accel, same sensor body frame
        rax = d.Raw_Accel.Data.X; ray = d.Raw_Accel.Data.Y; raz = d.Raw_Accel.Data.Z;
        gotA = true;
      }
    }
  } while (icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail && ++guard < 32);
  if (!gotQ && !gotA) return false;

  // ---- attitude + heading from the fused quaternion -------------------------
  if (gotQ) {
    float s2 = 1.0f - (q1 * q1 + q2 * q2 + q3 * q3);
    float q0 = s2 > 0 ? sqrt(s2) : 0;
    // Gravity-reaction "up" direction in the sensor body frame (3rd row of R).
    // At rest, level, board flat -> (0, 0, 1).
    float ux = 2 * (q1 * q3 - q0 * q2);
    float uy = 2 * (q0 * q1 + q2 * q3);
    float uz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
    // sensor body frame -> display frame (mounting knobs in config.h). Default:
    // roll<-X, pitch<-Y, vertical<-(-Z) so level reads (0, -1, 0).
    float roll = ux, pitch = uy, vert = uz;
#if ICM_SWAP_ROLL_PITCH
    { float t = roll; roll = pitch; pitch = t; }
#endif
    s->gx = (ICM_FLIP_ROLL)     ? -roll  :  roll;
    s->gy = (ICM_FLIP_VERTICAL) ?  vert  : -vert;
    s->gz = (ICM_FLIP_PITCH)    ? -pitch :  pitch;

    // heading: yaw of the magnetometer-referenced quaternion (absolute)
    float yaw = atan2(2 * (q0 * q3 + q1 * q2), 1 - 2 * (q2 * q2 + q3 * q3)) * 180.0f / PI;
    float h = ICM_HEADING_SIGN * yaw + ICM_HEADING_OFFSET;
    while (h < 0)       h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    s->heading = h;
  }

  // ---- live load factor + slip ball from the raw accelerometer --------------
  if (gotA) {
    const float as = 1.0f / 8192.0f;                       // DMP accel FSR = +/-4 g -> 8192 LSB/g
    float arx = rax * as, apy = ray * as, avz = raz * as;  // roll/pitch/vertical, g, sensor frame
    float amag = sqrt(arx * arx + apy * apy + avz * avz);  // load factor (g), remap-invariant
#if ICM_SWAP_ROLL_PITCH
    { float t = arx; arx = apy; apy = t; }
#endif
    // Specific force points UP at rest (level: +avz), so the ball reads centred —
    // the vertical sign is opposite the gravity vector's, matching the BNO path.
    float ax = (ICM_FLIP_ROLL)     ? -arx :  arx;
    float ay = (ICM_FLIP_VERTICAL) ? -avz :  avz;
    float az = (ICM_FLIP_PITCH)    ? -apy :  apy;
    s->ax = s->ax * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ax;
    s->ay = s->ay * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ay;
    s->az = s->az * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * az;
    s->g  = s->g  * (1 - ALPHA_GFORCE)   + ALPHA_GFORCE  * amag;
    if (s->g > s->max_g) s->max_g = s->g;
  }

  icm_last_data = millis();
  return true;
}

#else   // ENABLE_ICM20948 == 0  ->  inert stubs so the rest of the code links
bool          icm_present   = false;
unsigned long icm_last_data = 0;
bool icmBegin()          { return false; }
bool icmUpdate(state *s) { return false; }
#endif
