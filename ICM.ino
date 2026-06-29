// ============================================================================
//  ICM.ino — ICM-20948 9-DOF IMU (e.g. the GY-912 module) via its on-chip DMP.
//
//  All the motion processing happens ON THE IMU: the ICM-20948's Digital Motion
//  Processor runs the sensor fusion (accel + gyro + AK09916 magnetometer) and
//  outputs a single magnetometer-referenced orientation quaternion (the 9-axis
//  "Game Rotation Vector", Quat9). The host does NO Kalman / complementary
//  filtering — it reads attitude from the fused quaternion, the DMP's raw
//  accelerometer (same FIFO) for a live g-meter / slip ball, and the DMP's
//  CALIBRATED magnetometer for a tilt-compensated heading. (Heading comes from the
//  calibrated mag, not the quaternion yaw, because the 9-axis fusion yaw randomly
//  drifts / flips 180 deg; a direct mag compass can't.)
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
#include <Preferences.h>      // NVS storage for the DMP bias (compass/gyro/accel cal)

ICM_20948_I2C icm20948;
bool          icm_present   = false;
unsigned long icm_last_data = 0;

// ---- DMP calibration persistence -------------------------------------------
// The DMP auto-calibrates its compass/gyro/accel bias but forgets it on power-off
// (no on-chip NVM). So save the bias to the ESP32's flash and restore it at boot,
// so the compass is good immediately after a power cycle.
Preferences   icmPrefs;
const char   *ICM_BIAS_KEYS[9] = {"cgx","cgy","cgz","cax","cay","caz","cmx","cmy","cmz"};

static void icmGetBias(int32_t b[9]) {
  icm20948.getBiasGyroX(&b[0]);  icm20948.getBiasGyroY(&b[1]);  icm20948.getBiasGyroZ(&b[2]);
  icm20948.getBiasAccelX(&b[3]); icm20948.getBiasAccelY(&b[4]); icm20948.getBiasAccelZ(&b[5]);
  icm20948.getBiasCPassX(&b[6]); icm20948.getBiasCPassY(&b[7]); icm20948.getBiasCPassZ(&b[8]);
}
static void icmSetBias(const int32_t b[9]) {
  icm20948.setBiasGyroX(b[0]);  icm20948.setBiasGyroY(b[1]);  icm20948.setBiasGyroZ(b[2]);
  icm20948.setBiasAccelX(b[3]); icm20948.setBiasAccelY(b[4]); icm20948.setBiasAccelZ(b[5]);
  icm20948.setBiasCPassX(b[6]); icm20948.setBiasCPassY(b[7]); icm20948.setBiasCPassZ(b[8]);
}
// Restore the saved bias into the DMP (call after initializeDMP, before enableDMP).
static void icmLoadBias() {
  icmPrefs.begin("icmcal", true);
  if (icmPrefs.getBool("valid", false)) {
    int32_t b[9];
    for (int i = 0; i < 9; i++) b[i] = icmPrefs.getInt(ICM_BIAS_KEYS[i], 0);
    icmSetBias(b);
  }
  icmPrefs.end();
}
// Persist the DMP's current bias if it has changed meaningfully (called throttled).
static void icmSaveBias() {
  int32_t b[9];
  icmGetBias(b);
  icmPrefs.begin("icmcal", false);
  for (int i = 0; i < 9; i++) icmPrefs.putInt(ICM_BIAS_KEYS[i], b[i]);
  icmPrefs.putBool("valid", true);
  icmPrefs.end();
}

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
    icmLoadBias();                          // restore saved compass/gyro/accel cal (if any)
    bool ok = true;
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_ORIENTATION) == ICM_20948_Stat_Ok);         // attitude (Quat9)
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_RAW_ACCELEROMETER) == ICM_20948_Stat_Ok);   // g-meter / slip ball
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_RAW_GYROSCOPE) == ICM_20948_Stat_Ok);       // yaw rate -> heading fusion
    ok &= (icm20948.enableDMPSensor(INV_ICM20948_SENSOR_GEOMAGNETIC_FIELD) == ICM_20948_Stat_Ok);   // calibrated compass -> heading
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Quat9, 0) == ICM_20948_Stat_Ok);  // 0 = max rate
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Accel, 0) == ICM_20948_Stat_Ok);
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Gyro,  0) == ICM_20948_Stat_Ok);
    ok &= (icm20948.setDMPODRrate(DMP_ODR_Reg_Cpass_Calibr, 0) == ICM_20948_Stat_Ok);
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
// fused quaternion -> attitude, the raw accelerometer -> live g-meter + slip ball,
// and the CALIBRATED magnetometer -> tilt-compensated heading. Returns true when a
// sample arrived. Call only when the ICM is the active source (IMU.ino).
bool icmUpdate(state *s) {
  static float up_x = 0, up_y = 0, up_z = 1;   // latest sensor-frame up vector
  static float magH = 0;                       // latest tilt-comp mag heading (compass)
  static int16_t gxr = 0, gyr = 0, gzr = 0;    // latest raw gyro (sensor frame)
  icm_20948_DMP_data_t d;
  float   q1 = 0, q2 = 0, q3 = 0;
  int16_t rax = 0, ray = 0, raz = 0;
  int32_t mcx = 0, mcy = 0, mcz = 0;
  bool gotQ = false, gotA = false, gotM = false;
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
      if (d.header & DMP_header_bitmap_Gyro) {     // raw gyro (sensor frame), +/-2000 dps
        gxr = d.Raw_Gyro.Data.X; gyr = d.Raw_Gyro.Data.Y; gzr = d.Raw_Gyro.Data.Z;
      }
      if (d.header & DMP_header_bitmap_Compass_Calibr) {   // calibrated mag (device frame)
        mcx = d.Compass_Calibr.Data.X; mcy = d.Compass_Calibr.Data.Y; mcz = d.Compass_Calibr.Data.Z;
        gotM = true;
      }
    }
  } while (icm20948.status == ICM_20948_Stat_FIFOMoreDataAvail && ++guard < 32);
  if (!gotQ && !gotA && !gotM) return false;

  // ---- attitude from the fused quaternion (gyro-stabilized) -----------------
  if (gotQ) {
    float s2 = 1.0f - (q1 * q1 + q2 * q2 + q3 * q3);
    float q0 = s2 > 0 ? sqrt(s2) : 0;
    // Gravity-reaction "up" in the sensor body frame (3rd row of R). Level -> (0,0,1).
    up_x = 2 * (q1 * q3 - q0 * q2);
    up_y = 2 * (q0 * q1 + q2 * q3);
    up_z = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
    // sensor body frame -> display frame (mounting knobs in config.h). Default:
    // roll<-X, pitch<-Y, vertical<-(-Z) so level reads (0, -1, 0).
    float roll = up_x, pitch = up_y, vert = up_z;
#if ICM_SWAP_ROLL_PITCH
    { float t = roll; roll = pitch; pitch = t; }
#endif
    s->gx = (ICM_FLIP_ROLL)     ? -roll  :  roll;
    s->gy = (ICM_FLIP_VERTICAL) ?  vert  : -vert;
    s->gz = (ICM_FLIP_PITCH)    ? -pitch :  pitch;
  }

  // ---- heading: tilt-comp CALIBRATED magnetometer (absolute, can't flip 180),
  //      fused with the gyro yaw rate (smooth/responsive). The DMP still fuses the
  //      mag into the quaternion above; we just take heading from the cal'd mag. ---
  if (gotM) {
    magH = tiltCompassDeg(up_x, up_y, up_z, (float)mcx, (float)mcy, (float)mcz,
                          ICM_HEADING_SIGN, ICM_HEADING_OFFSET, magH);
  }
  {
    const float gs = 1.0f / 16.384f;                       // raw gyro -> deg/s (+/-2000 dps)
    float un = sqrtf(up_x * up_x + up_y * up_y + up_z * up_z);
    float yawRate = 0.0f;                                  // deg/s about the vertical axis
    if (un > 0) yawRate = ICM_HEADING_GYRO_SIGN *
        (gxr * gs * up_x + gyr * gs * up_y + gzr * gs * up_z) / un;
    s->heading = fuseHeading(magH, yawRate);
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

  // Persist the DMP's refined cal bias every 30 s, but only when it has actually
  // drifted, so we don't wear the flash writing the same values.
  static unsigned long lastSave = 0;
  static int32_t savedB[9] = {0};
  if (millis() - lastSave > 30000) {
    lastSave = millis();
    int32_t b[9]; icmGetBias(b);
    long diff = 0; for (int i = 0; i < 9; i++) diff += labs((long)b[i] - (long)savedB[i]);
    if (diff > 200) { icmSaveBias(); for (int i = 0; i < 9; i++) savedB[i] = b[i]; }
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
