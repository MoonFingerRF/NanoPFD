// ============================================================================
//  ICM.ino — ICM-20948 9-DOF IMU (e.g. the GY-912 module), self-contained I2C
//  register driver. No SparkFun library and no DMP firmware upload: the DMP path
//  needed a hand-patched library header (ICM_20948_USE_DMP) and never ran here.
//
//  Instead this reads the raw sensors over Wire (like the QMI fallback driver in
//  IMU.ino) and fuses on the host:
//    - attitude : accel + gyro -> Mahony complementary filter -> gravity vector
//    - g-force  : accelerometer magnitude -> load factor (real, not faked)
//    - heading  : AK09916 magnetometer, tilt-compensated by the gravity vector
//
//  Used as a PRIMARY IMU: IMU.ino's failover tries it after the BNO08x and
//  before the QMI, so a board with ONLY a GY-912 is auto-detected and runs on it,
//  while a board with a BNO uses the BNO. Both live on the same I2C bus.
//
//  The ICM and AK09916 share I2C with the BNO/BMP (different addresses), so all
//  the supported IMUs coexist and the right one is selected at runtime.
// ============================================================================

#if ENABLE_ICM20948
#include <Wire.h>

// ---- ICM-20948 registers (the chip is BANK-SWITCHED via REG_BANK_SEL) -------
#define ICM_REG_BANK_SEL 0x7F   // value = bank << 4 (accessible from any bank)
// bank 0
#define ICM_WHO_AM_I     0x00   // -> 0xEA
#define ICM_PWR_MGMT_1   0x06
#define ICM_PWR_MGMT_2   0x07
#define ICM_INT_PIN_CFG  0x0F   // BYPASS_EN (bit1) exposes the AK09916 on the main bus
#define ICM_ACCEL_XOUT_H 0x2D   // accel(6) + gyro(6) contiguous, big-endian
// bank 2
#define ICM_GYRO_CONFIG_1 0x01
#define ICM_ACCEL_CONFIG  0x14
// AK09916 magnetometer (reachable at I2C 0x0C once bypass is on)
#define AK_ADDR  0x0C
#define AK_WIA2  0x01           // -> 0x09
#define AK_ST1   0x10           // DRDY = bit0
#define AK_HXL   0x11           // HXL..HZH (LE) then TMPS, ST2 (HOFL = bit3)
#define AK_CNTL2 0x31           // 0x08 = continuous mode 4 (100 Hz)
#define AK_CNTL3 0x32           // 0x01 = soft reset

uint8_t       icm_addr      = 0;       // 0x68 or 0x69 once detected (0 = not found)
bool          icm_present   = false;
bool          icm_mag_ok    = false;
unsigned long icm_last_data = 0;

// Mahony filter state (its own quaternion, independent of the QMI's q_qmi).
float         q_icm[4]    = {1, 0, 0, 0};
unsigned long icm_ahrs_us = 0;
#define ICM_MAHONY_KP 2.0f

// --- raw register helpers (bank-aware) -------------------------------------
static void icmBankSel(uint8_t b) {
  Wire.beginTransmission(icm_addr); Wire.write(ICM_REG_BANK_SEL); Wire.write(b << 4); Wire.endTransmission();
}
static void icmW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(icm_addr); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static uint8_t icmR(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(icm_addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;                 // repeated start
  uint8_t n = Wire.requestFrom((int)icm_addr, (int)len);
  for (uint8_t i = 0; i < n && i < len; i++) buf[i] = Wire.read();
  return n;
}
// AK09916 over the bypass-exposed main bus (its own I2C address)
static void akW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AK_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static uint8_t akR(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(AK_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  uint8_t n = Wire.requestFrom((int)AK_ADDR, (int)len);
  for (uint8_t i = 0; i < n && i < len; i++) buf[i] = Wire.read();
  return n;
}

// Detect the ICM (I2C 0x68 with AD0 low, else 0x69), wake it, set the ranges
// (accel +/-8 g, gyro +/-2000 dps), and bring up the AK09916 magnetometer via
// bypass. True if the IMU is found (the mag is optional -> icm_mag_ok).
bool icmBegin() {
  icm_present = false; icm_mag_ok = false;
  uint8_t addrs[2] = {0x68, 0x69};
  for (int i = 0; i < 2; i++) {
    icm_addr = addrs[i];
    uint8_t who = 0;
    icmBankSel(0);
    if (icmR(ICM_WHO_AM_I, &who, 1) != 1 || who != 0xEA) continue;

    icmW(ICM_PWR_MGMT_1, 0x80); delay(50);     // soft reset
    icmBankSel(0);
    icmW(ICM_PWR_MGMT_1, 0x01); delay(10);     // auto clock, clear sleep
    icmW(ICM_PWR_MGMT_2, 0x00);                // accel + gyro enabled
    icmW(ICM_INT_PIN_CFG, 0x02);               // BYPASS_EN -> AK09916 on the main bus
    icmBankSel(2);
    icmW(ICM_GYRO_CONFIG_1, 0x07);             // gyro +/-2000 dps, DLPF on
    icmW(ICM_ACCEL_CONFIG,  0x05);             // accel +/-8 g, DLPF on
    icmBankSel(0);
    delay(10);

    uint8_t wia = 0;                           // magnetometer (optional)
    if (akR(AK_WIA2, &wia, 1) == 1 && wia == 0x09) {
      akW(AK_CNTL3, 0x01); delay(10);          // soft reset
      akW(AK_CNTL2, 0x08);                     // continuous mode, 100 Hz
      icm_mag_ok = true;
    }
    icm_present = true;
    return true;
  }
  icm_addr = 0;
  return false;
}

// Read accel (m/s^2) + gyro (rad/s) in the ICM body frame. False on bus error.
static bool icmReadAG(float *ax, float *ay, float *az, float *gx, float *gy, float *gz) {
  uint8_t b[12];
  icmBankSel(0);
  if (icmR(ICM_ACCEL_XOUT_H, b, 12) != 12) return false;   // accel(6)+gyro(6), big-endian
  int16_t rax = (int16_t)((b[0]  << 8) | b[1]),  ray = (int16_t)((b[2]  << 8) | b[3]),  raz = (int16_t)((b[4]  << 8) | b[5]);
  int16_t rgx = (int16_t)((b[6]  << 8) | b[7]),  rgy = (int16_t)((b[8]  << 8) | b[9]),  rgz = (int16_t)((b[10] << 8) | b[11]);
  const float as = 9.80665f / 4096.0f;            // +/-8 g     -> 4096 LSB/g
  const float gs = (1.0f / 16.4f) * (PI / 180.0f); // +/-2000 dps -> 16.4 LSB/dps -> rad/s
  *ax = rax * as; *ay = ray * as; *az = raz * as;
  *gx = rgx * gs; *gy = rgy * gs; *gz = rgz * gs;
  return true;
}

// Read the AK09916 (uT), aligned into the ICM accel/gyro body frame. False if no
// new sample / overflow / mag absent.
static bool icmReadMag(float *mx, float *my, float *mz) {
  if (!icm_mag_ok) return false;
  uint8_t st1 = 0;
  if (akR(AK_ST1, &st1, 1) != 1 || !(st1 & 0x01)) return false;   // DRDY
  uint8_t b[8];
  if (akR(AK_HXL, b, 8) != 8) return false;                       // HXL..ST2 (read ST2 to release)
  if (b[7] & 0x08) return false;                                  // ST2 HOFL = magnetic overflow
  int16_t rmx = (int16_t)((b[1] << 8) | b[0]);                    // little-endian
  int16_t rmy = (int16_t)((b[3] << 8) | b[2]);
  int16_t rmz = (int16_t)((b[5] << 8) | b[4]);
  const float ms = 0.15f;                                         // uT/LSB
  // AK09916 axes -> ICM accel/gyro frame: swap X/Y, negate Z (InvenSense layout).
  *mx = rmy * ms; *my = rmx * ms; *mz = -rmz * ms;
  return true;
}

// Pull a fresh sample, fuse it, and fill the shared state. Returns true when a
// sample arrived. Only called while the ICM is the active source (IMU.ino).
bool icmUpdate(state *s) {
  float ax, ay, az, gx, gy, gz;
  if (!icmReadAG(&ax, &ay, &az, &gx, &gy, &gz)) return false;

  // --- Mahony (accel + gyro) -> q_icm (body -> earth) -----------------------
  unsigned long now = micros();
  float dt = icm_ahrs_us ? (now - icm_ahrs_us) * 1e-6f : 0.02f;
  icm_ahrs_us = now;
  if (dt > 0.2f) dt = 0.2f;                          // clamp after long gaps
  {
    float an = sqrt(ax * ax + ay * ay + az * az);
    if (an > 0) {
      float nax = ax / an, nay = ay / an, naz = az / an;
      float vx = 2 * (q_icm[1] * q_icm[3] - q_icm[0] * q_icm[2]);
      float vy = 2 * (q_icm[0] * q_icm[1] + q_icm[2] * q_icm[3]);
      float vz = q_icm[0] * q_icm[0] - q_icm[1] * q_icm[1] - q_icm[2] * q_icm[2] + q_icm[3] * q_icm[3];
      gx += ICM_MAHONY_KP * (nay * vz - naz * vy);   // error = accel x estimated_up
      gy += ICM_MAHONY_KP * (naz * vx - nax * vz);
      gz += ICM_MAHONY_KP * (nax * vy - nay * vx);
    }
    float qa = q_icm[0], qb = q_icm[1], qc = q_icm[2], qd = q_icm[3];
    q_icm[0] += (-qb * gx - qc * gy - qd * gz) * 0.5f * dt;
    q_icm[1] += ( qa * gx + qc * gz - qd * gy) * 0.5f * dt;
    q_icm[2] += ( qa * gy - qb * gz + qd * gx) * 0.5f * dt;
    q_icm[3] += ( qa * gz + qb * gy - qc * gx) * 0.5f * dt;
    float qn = sqrt(q_icm[0]*q_icm[0] + q_icm[1]*q_icm[1] + q_icm[2]*q_icm[2] + q_icm[3]*q_icm[3]);
    if (qn > 0) { q_icm[0]/=qn; q_icm[1]/=qn; q_icm[2]/=qn; q_icm[3]/=qn; }
  }

  // earth-up (gravity reaction) in the ICM body frame
  float w = q_icm[0], x = q_icm[1], y = q_icm[2], z = q_icm[3];
  float ux = 2 * (x * z - w * y);
  float uy = 2 * (w * x + y * z);
  float uz = w * w - x * x - y * y + z * z;

  // ---- ICM body frame -> display frame (TUNE for your mounting) ------------
  // Same axis permutation the BNO uses (x, z, y). If the horizon or turn
  // coordinator come out wrong on hardware, flip a sign / swap a pair here AND
  // on s->ax/ay/az below so attitude and the slip ball stay consistent.
  s->gx = ux;
  s->gy = uz;
  s->gz = uy;
  // slip/turn-coordinator: the live accelerometer vector, same remap, smoothed.
  s->ax = s->ax * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ax;
  s->ay = s->ay * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * az;
  s->az = s->az * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ay;
  // real load factor from the accel magnitude (g)
  float am = sqrt(ax * ax + ay * ay + az * az);
  s->g = s->g * (1 - ALPHA_GFORCE) + ALPHA_GFORCE * am / 9.80665f;
  if (s->g > s->max_g) s->max_g = s->g;

  // ---- tilt-compensated heading from the magnetometer ----------------------
  // forward = the ICM +X axis; project mag + forward onto the horizontal plane
  // (perpendicular to the gravity-reaction up vector). TUNE via ICM_HEADING_*.
  float mx, my, mz;
  if (icmReadMag(&mx, &my, &mz)) {
    float un = sqrt(ux * ux + uy * uy + uz * uz);
    if (un > 0) {
      float Ux = ux / un, Uy = uy / un, Uz = uz / un;
      float md = mx * Ux + my * Uy + mz * Uz;                 // mag . up
      float nx = mx - md * Ux, ny = my - md * Uy, nz = mz - md * Uz;   // mag, horizontal
      float fd = Ux;                                          // (1,0,0) . up
      float fx = 1 - fd * Ux, fy = -fd * Uy, fz = -fd * Uz;   // forward, horizontal
      float fn = sqrt(fx * fx + fy * fy + fz * fz);
      if (fn > 1e-3f) {
        fx /= fn; fy /= fn; fz /= fn;
        float rx = Uy * fz - Uz * fy, ry = Uz * fx - Ux * fz, rz = Ux * fy - Uy * fx;  // up x forward
        float north = nx * fx + ny * fy + nz * fz;
        float east  = nx * rx + ny * ry + nz * rz;
        float hd = ICM_HEADING_SIGN * atan2(east, north) * 180.0f / PI + ICM_HEADING_OFFSET;
        while (hd < 0)       hd += 360.0f;
        while (hd >= 360.0f) hd -= 360.0f;
        s->heading = hd;
      }
    }
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
