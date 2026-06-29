// ============================================================================
//  IMU.ino — IMU drivers with automatic failover
//
//  Primary: BNO08x 9-DOF (external, I2C 0x4A). Provides via the shared state:
//    - attitude : GRAVITY report -> "down" vector (s->gx/gy/gz)
//    - g-force  : ACCELEROMETER magnitude -> load factor (s->g) + peak (max_g)
//    - heading  : MAGNETIC_FIELD_CALIBRATED -> compass (s->mx/my/mz)
//
//  Fallback: onboard QMI8658C (I2C 0x6B). 6-axis, no magnetometer -> attitude
//  (from accel) + g-force only; heading freezes while on the fallback.
//
//  updateIMU() uses the BNO whenever it is streaming, and automatically switches
//  to the QMI when the BNO drops out, switching back when the BNO returns.
//
//  BNO axes are reordered (x, z, y) so the sensor frame maps onto the display
//  frame used by the renderers.
// ============================================================================
#include <Adafruit_BNO08x.h>
#include <Preferences.h>      // NVS storage for the QMI orientation calibration

#define BNO08X_ADDR 0x4A

Adafruit_BNO08x bno08x(-1);
sh2_SensorValue_t sensorValue;

// True once begin_I2C() has actually succeeded. The sh2 report API
// (enableReport / getSensorEvent) dereferences a transport pointer that only
// exists after a successful begin — calling it on an absent device crashes
// (LoadProhibited). Everything below is gated on this flag.
bool imu_began = false;
bool imu_ever_began = false;        // sh2 session has been opened at least once
unsigned long imu_last_begin = 0;   // throttles re-init attempts (hot-plug)
unsigned long imu_last_data  = 0;   // millis() of the last valid sensor event

// How long without ANY event before we consider the IMU disconnected. The
// BNO08x streams accel/gravity at ~500 Hz, so this only trips on a real fault.
// (getSensorEvent() returns false on every idle cycle, which is normal and must
//  NOT be read as a disconnect — that was the old false-disconnect bug.)
#define IMU_TIMEOUT_MS 750

// Enable ALL reports unconditionally — NO early-return. On a flaky reconnect a
// single enableReport() may fail; if we bail early, later reports never get
// enabled. That left GRAVITY (the horizon) off while ACCELEROMETER (the g-meter)
// was on — the "g-meter updates but the horizon is frozen" reconnect symptom.
// Heading now comes from the fused ROTATION_VECTOR (9-axis accel+gyro+mag, the
// BNO's internal Kalman) instead of the raw magnetometer, which is far quieter.
bool setIMUReports() {
  bool a = bno08x.enableReport(SH2_ACCELEROMETER, 2000);              // g-meter / slip
  bool g = bno08x.enableReport(SH2_GRAVITY, 2000);                    // horizon / attitude
  // Heading comes from the CALIBRATED magnetometer tilt-compensated by gravity, NOT
  // the fused rotation-vector yaw: the fusion yaw was randomly drifting / flipping
  // 180 deg. A direct mag compass can't flip — it just needs the chip's running
  // hard/soft-iron cal (which the BNO applies to this report).
  bool m = bno08x.enableReport(SH2_MAGNETIC_FIELD_CALIBRATED, 10000);
  return a && g && m;
}

// Bring the BNO08x up. Safe to call repeatedly; only enables reports once the
// device has actually been detected on I2C.
bool tryBeginIMU() {
  imu_began = bno08x.begin_I2C();
  if (imu_began)
    imu_began = setIMUReports();
  return imu_began;
}

// ----------------------------------------------------------------------------
//  Onboard QMI8658C — fallback IMU. 6-axis (accel + gyro), NO magnetometer and
//  no onboard fusion, so it provides attitude (from the accelerometer) and
//  g-force, but NOT heading. Minimal register-level driver over Wire.
// ----------------------------------------------------------------------------
#define QMI_WHO_AM_I 0x00     // -> 0x05
#define QMI_CTRL1    0x02
#define QMI_CTRL2    0x03     // accel config
#define QMI_CTRL3    0x04     // gyro config
#define QMI_CTRL7    0x08     // sensor enables
#define QMI_AX_L     0x35     // accel 0x35..0x3A then gyro 0x3B..0x40 (12 bytes)

uint8_t qmi_addr = 0;         // 0x6B or 0x6A once detected (0 = not found)

// Which IMU is currently feeding the state (for telemetry / indication).
enum ImuSource { IMU_SRC_NONE, IMU_SRC_BNO, IMU_SRC_QMI, IMU_SRC_ICM };
ImuSource imu_source = IMU_SRC_NONE;
bool qmi_present = false;

int imuSource() { return (int)imu_source; }   // 0=none 1=BNO 2=QMI 3=ICM

// QMI attitude is estimated with a Mahony complementary filter (accel + gyro
// fusion) -> a full orientation quaternion q_qmi (body -> earth). A fixed
// alignment quaternion q_align maps the QMI body frame onto the display frame;
// it is learned by comparing the QMI's gravity direction to the BNO's once per
// boot (so it captures whatever orientation the onboard IMU is mounted at) and
// is saved to NVS.
float q_qmi[4]   = {1, 0, 0, 0};
float q_align[4] = {1, 0, 0, 0};
float qmi_acc[3] = {0, 0, 0};         // last QMI accel (m/s^2, QMI body frame)
unsigned long qmi_ahrs_us = 0;        // for the Mahony dt
#define MAHONY_KP   2.0f
#define ALIGN_GAIN  0.15f

// Once-per-boot auto-calibration (while the BNO streams).
double        cal_gsum[3] = {0, 0, 0};   // mean gravity dir (to detect "too still")
long          cal_count   = 0;
unsigned long cal_start   = 0;
bool          cal_running = false;
bool          cal_done    = false;
#define CAL_DURATION_MS  10000
#define CAL_MIN_SAMPLES  100
Preferences   qmiPrefs;

// Throttle QMI I2C reads so they don't saturate the bus shared with the BNO/BMP.
unsigned long qmi_last_read = 0;
#define QMI_READ_MS 20               // ~50 Hz (warm filter + cal + fallback)

// --- quaternion helpers (q = {w,x,y,z}) -----------------------------------
void qnorm(float *q) {
  float n = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
  if (n > 0) { q[0]/=n; q[1]/=n; q[2]/=n; q[3]/=n; }
}
void qmul(const float *a, const float *b, float *o) {
  o[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
  o[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
  o[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
  o[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}
void qrot(const float *q, const float *v, float *o) {   // rotate vector v by q
  float w=q[0],x=q[1],y=q[2],z=q[3], vx=v[0],vy=v[1],vz=v[2];
  o[0] = (1-2*(y*y+z*z))*vx + 2*(x*y-w*z)*vy + 2*(x*z+w*y)*vz;
  o[1] = 2*(x*y+w*z)*vx + (1-2*(x*x+z*z))*vy + 2*(y*z-w*x)*vz;
  o[2] = 2*(x*z-w*y)*vx + 2*(y*z+w*x)*vy + (1-2*(x*x+y*y))*vz;
}
// earth-up (gravity-reaction) direction expressed in the QMI body frame
void qmiUpBody(float *o) {
  float w=q_qmi[0],x=q_qmi[1],y=q_qmi[2],z=q_qmi[3];
  o[0] = 2*(x*z - w*y);
  o[1] = 2*(w*x + y*z);
  o[2] = w*w - x*x - y*y + z*z;
}
// Mahony 6-axis filter update. gyro in rad/s, accel any unit (QMI body frame).
void mahonyUpdate(float gx, float gy, float gz, float ax, float ay, float az, float dt) {
  float an = sqrt(ax*ax + ay*ay + az*az);
  if (an > 0) {
    ax/=an; ay/=an; az/=an;
    float vx = 2*(q_qmi[1]*q_qmi[3] - q_qmi[0]*q_qmi[2]);
    float vy = 2*(q_qmi[0]*q_qmi[1] + q_qmi[2]*q_qmi[3]);
    float vz = q_qmi[0]*q_qmi[0] - q_qmi[1]*q_qmi[1] - q_qmi[2]*q_qmi[2] + q_qmi[3]*q_qmi[3];
    gx += MAHONY_KP * (ay*vz - az*vy);   // error = accel x estimated_up
    gy += MAHONY_KP * (az*vx - ax*vz);
    gz += MAHONY_KP * (ax*vy - ay*vx);
  }
  float qa=q_qmi[0], qb=q_qmi[1], qc=q_qmi[2], qd=q_qmi[3];
  q_qmi[0] += (-qb*gx - qc*gy - qd*gz) * 0.5f * dt;
  q_qmi[1] += ( qa*gx + qc*gz - qd*gy) * 0.5f * dt;
  q_qmi[2] += ( qa*gy - qb*gz + qd*gx) * 0.5f * dt;
  q_qmi[3] += ( qa*gz + qb*gy - qc*gx) * 0.5f * dt;
  qnorm(q_qmi);
}

uint8_t qmiRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(qmi_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;   // repeated start
  uint8_t n = Wire.requestFrom((int)qmi_addr, (int)len);
  for (uint8_t i = 0; i < n && i < len; i++) buf[i] = Wire.read();
  return n;
}
void qmiWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(qmi_addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Detect + configure the QMI8658 accelerometer (+/-8 g). True if found.
bool qmiBegin() {
  uint8_t addrs[2] = {0x6B, 0x6A};
  for (int i = 0; i < 2; i++) {
    qmi_addr = addrs[i];
    uint8_t who = 0;
    if (qmiRead(QMI_WHO_AM_I, &who, 1) == 1 && who == 0x05) {
      qmiWrite(QMI_CTRL1, 0x40);   // address auto-increment, little-endian, enabled
      qmiWrite(QMI_CTRL2, 0x25);   // accel: +/-8 g, ODR ~250 Hz
      qmiWrite(QMI_CTRL3, 0x55);   // gyro:  +/-512 dps, ODR ~250 Hz
      qmiWrite(QMI_CTRL7, 0x03);   // enable accelerometer + gyroscope
      delay(10);
      return true;
    }
  }
  qmi_addr = 0;
  return false;
}

// Read accel (m/s^2) + gyro (rad/s) from the QMI (native axes). False on error.
bool qmiReadAG(float *ax, float *ay, float *az, float *gx, float *gy, float *gz) {
  if (!qmi_addr) return false;
  uint8_t b[12];
  if (qmiRead(QMI_AX_L, b, 12) != 12) return false;   // accel(6) + gyro(6), contiguous
  int16_t rax=(int16_t)((b[1]<<8)|b[0]),  ray=(int16_t)((b[3]<<8)|b[2]),  raz=(int16_t)((b[5]<<8)|b[4]);
  int16_t rgx=(int16_t)((b[7]<<8)|b[6]),  rgy=(int16_t)((b[9]<<8)|b[8]),  rgz=(int16_t)((b[11]<<8)|b[10]);
  const float as = 9.80665f / 4096.0f;            // +/-8 g     -> 4096 LSB/g
  const float gs = (1.0f / 64.0f) * (PI/180.0f);  // +/-512 dps -> 64 LSB/dps -> rad/s
  *ax=rax*as; *ay=ray*as; *az=raz*as;
  *gx=rgx*gs; *gy=rgy*gs; *gz=rgz*gs;
  return true;
}

// Read the QMI, advance the Mahony filter, and (when calibrating) nudge q_align
// toward the BNO's gravity vector. Returns false on bus error.
bool qmiUpdate(state *s, bool calibrating) {
  float ax, ay, az, gx, gy, gz;
  if (!qmiReadAG(&ax, &ay, &az, &gx, &gy, &gz)) return false;
  qmi_acc[0]=ax; qmi_acc[1]=ay; qmi_acc[2]=az;
  unsigned long now = micros();
  float dt = qmi_ahrs_us ? (now - qmi_ahrs_us) * 1e-6f : 0.02f;
  qmi_ahrs_us = now;
  if (dt > 0.2f) dt = 0.2f;                          // clamp after long gaps
  mahonyUpdate(gx, gy, gz, ax, ay, az, dt);

  if (calibrating) {
    float up[3];   qmiUpBody(up);                    // QMI body "up"
    float pred[3]; qrot(q_align, up, pred);          // -> display frame (current guess)
    float tgt[3] = {s->gx, s->gy, s->gz};            // BNO display "up"
    // nudge q_align (left-multiply) so pred rotates toward tgt (axis = pred x tgt)
    float dq[4] = {1.0f,
                   0.5f*ALIGN_GAIN*(pred[1]*tgt[2] - pred[2]*tgt[1]),
                   0.5f*ALIGN_GAIN*(pred[2]*tgt[0] - pred[0]*tgt[2]),
                   0.5f*ALIGN_GAIN*(pred[0]*tgt[1] - pred[1]*tgt[0])};
    float nq[4]; qmul(dq, q_align, nq);
    q_align[0]=nq[0]; q_align[1]=nq[1]; q_align[2]=nq[2]; q_align[3]=nq[3];
    qnorm(q_align);
    cal_gsum[0]+=tgt[0]; cal_gsum[1]+=tgt[1]; cal_gsum[2]+=tgt[2];
    cal_count++;
  }
  return true;
}

void loadQmiCal() {
  qmiPrefs.begin("imucal", true);                 // read-only
  if (qmiPrefs.getBool("qvalid", false))
    qmiPrefs.getBytes("qalign", q_align, sizeof(q_align));
  qmiPrefs.end();
}
void saveQmiCal() {
  qmiPrefs.begin("imucal", false);
  qmiPrefs.putBytes("qalign", q_align, sizeof(q_align));
  qmiPrefs.putBool("qvalid", true);
  qmiPrefs.end();
}

// Finalize the once-per-boot calibration: refuse to save if the board barely
// moved (gravity stayed in ~one direction, so the about-gravity axis can't be
// resolved); otherwise persist the learned alignment quaternion.
void qmiCalFinalize() {
  if (cal_count < CAL_MIN_SAMPLES) return;
  double mm = sqrt(cal_gsum[0]*cal_gsum[0] + cal_gsum[1]*cal_gsum[1] + cal_gsum[2]*cal_gsum[2]) / cal_count;
  if (mm > 0.9) {
    USBSerial.println("QMI cal skipped: board too still — tilt it (pitch+roll) during the 10 s window");
    return;
  }
  saveQmiCal();
  USBSerial.printf("QMI cal saved: q_align %.3f %.3f %.3f %.3f (n=%ld)\n",
                   q_align[0], q_align[1], q_align[2], q_align[3], cal_count);
}

void initIMU(state *s) {
  for (int n = 0; n < 10; n++) {
    delay(10);
    if (tryBeginIMU())
      break;
  }
  if (imu_began) { imu_last_data = millis(); imu_ever_began = true; }
  qmi_present = qmiBegin();              // detect the onboard fallback IMU
  loadQmiCal();                          // restore saved QMI orientation (if any)
  icmBegin();                            // detect optional ICM-20948 (DMP) primary
  s->IMU = imu_began || qmi_present || icm_present;
}

// Tilt-compensated magnetic heading (deg, 0..360). `up` = gravity-reaction vector
// and `m` = magnetometer, BOTH in the same frame (any units — only direction
// matters). Forward reference is that frame's +X axis; `offset`/`sign` rotate it to
// true north and pick CW/CCW. Unlike a fused yaw this is deterministic, so it never
// drifts or flips 180 deg. Returns `fallback` when the nose is ~vertical (the
// forward axis degenerates) so the heading just holds. Shared by the BNO + ICM.
float tiltCompassDeg(float ux, float uy, float uz, float mx, float my, float mz,
                     float sign, float offset, float fallback) {
  float un = sqrtf(ux * ux + uy * uy + uz * uz);
  if (un <= 0) return fallback;
  ux /= un; uy /= un; uz /= un;
  float md = mx * ux + my * uy + mz * uz;                  // mag . up
  float hx = mx - md * ux, hy = my - md * uy, hz = mz - md * uz;   // horizontal field
  float fd = ux;                                           // (1,0,0) . up
  float fx = 1.0f - fd * ux, fy = -fd * uy, fz = -fd * uz; // forward (+X) -> horizontal
  float fn = sqrtf(fx * fx + fy * fy + fz * fz);
  if (fn < 1e-4f) return fallback;                         // nose ~vertical -> hold
  fx /= fn; fy /= fn; fz /= fn;
  float rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx; // up x fwd
  float north = hx * fx + hy * fy + hz * fz;
  float east  = hx * rx + hy * ry + hz * rz;
  float h = sign * atan2f(east, north) * 180.0f / PI + offset;
  while (h < 0)       h += 360.0f;
  while (h >= 360.0f) h -= 360.0f;
  return h;
}

void updateIMU(state *s) {
  // --- service the BNO08x (primary), if it has ever initialized -------------
  if (imu_began) {
    if (bno08x.wasReset()) {
      USBSerial.println("IMU Reset");
      setIMUReports();               // re-enable reports; NOT a disconnect
    }
    // DRAIN the whole FIFO each cycle, not one event. The BNO streams ~1100 events/s
    // (accel 500 + gravity 500 + rotation 100 Hz); processing one per loop lets the FIFO
    // back up and the attitude LAG. Draining keeps the freshest sample so the published
    // state is always current — effective sensor rate = the loop rate (~kHz), far above
    // any display fps. The cap bounds the worst case (a large backlog after a stall).
    int _imuDrain = 0;
    while (bno08x.getSensorEvent(&sensorValue) && ++_imuDrain <= 64) {
    imu_last_data = millis();
    switch (sensorValue.sensorId) {
      case SH2_ACCELEROMETER:
        {
          // roll/pitch/vertical sources, then the same config mounting flips as the
          // gravity case so the slip ball stays consistent with the horizon.
          float roll  = sensorValue.un.accelerometer.x;
          float pitch = sensorValue.un.accelerometer.y;
          float vert  = sensorValue.un.accelerometer.z;
          float amag  = sqrt(roll * roll + pitch * pitch + vert * vert);
#if BNO_SWAP_ROLL_PITCH
          { float t = roll; roll = pitch; pitch = t; }
#endif
          float ax = BNO_FLIP_ROLL     ? -roll  :  roll;
          float ay = BNO_FLIP_VERTICAL ? -vert  :  vert;
          float az = BNO_FLIP_PITCH    ? -pitch :  pitch;
          s->ax = s->ax * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ax;
          s->ay = s->ay * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * ay;
          s->az = s->az * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * az;
          s->g  = s->g  * (1 - ALPHA_GFORCE)   + ALPHA_GFORCE  * amag / 9.8;
          if (s->g > s->max_g) s->max_g = s->g;   // track peak load factor
          break;
        }
      case SH2_MAGNETIC_FIELD_CALIBRATED:
        {
          float mx = sensorValue.un.magneticField.x;
          float my = sensorValue.un.magneticField.z;
          float mz = sensorValue.un.magneticField.y;
          s->mx = s->mx * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * mx;
          s->my = s->my * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * my;
          s->mz = s->mz * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * mz;
          break;
        }
      case SH2_LINEAR_ACCELERATION:
        {
          float lx = sensorValue.un.linearAcceleration.x;
          float ly = sensorValue.un.linearAcceleration.z;
          float lz = sensorValue.un.linearAcceleration.y;
          break;
        }
      case SH2_GRAVITY:
        {
          float gx = sensorValue.un.gravity.x;
          float gy = sensorValue.un.gravity.y;
          float gz = sensorValue.un.gravity.z;
          float gmag = sqrt(gx * gx + gy * gy + gz * gz);
          if (gmag <= 0) gmag = 1;
          // up vector (roll/pitch/vertical sources), then config mounting flips
          float roll = -gx / gmag, pitch = -gy / gmag, vert = -gz / gmag;
#if BNO_SWAP_ROLL_PITCH
          { float t = roll; roll = pitch; pitch = t; }
#endif
          s->gx = BNO_FLIP_ROLL     ? -roll  :  roll;
          s->gy = BNO_FLIP_VERTICAL ? -vert  :  vert;
          s->gz = BNO_FLIP_PITCH    ? -pitch :  pitch;
          break;
        }
      case SH2_ROTATION_VECTOR:
        {
          // Fused heading = yaw of the BNO's orientation quaternion about the
          // vertical (this board mounts the BNO Z-axis up, per the gravity map
          // above). The chip's internal Kalman makes this far smoother than the
          // raw-magnetometer compass; tune BNO_HEADING_SIGN/OFFSET to the mounting.
          float qr = sensorValue.un.rotationVector.real;
          float qi = sensorValue.un.rotationVector.i;
          float qj = sensorValue.un.rotationVector.j;
          float qk = sensorValue.un.rotationVector.k;
          float yaw = atan2(2.0f * (qr * qk + qi * qj),
                            1.0f - 2.0f * (qj * qj + qk * qk)) * 180.0f / PI;
          float h = BNO_HEADING_SIGN * yaw + BNO_HEADING_OFFSET;
          while (h < 0)        h += 360.0f;
          while (h >= 360.0f)  h -= 360.0f;
          s->heading = h;
          break;
        }
      case SH2_GEOMAGNETIC_ROTATION_VECTOR:
        {
          float mr = sensorValue.un.geoMagRotationVector.real;
          float mi = sensorValue.un.geoMagRotationVector.i;
          float mj = sensorValue.un.geoMagRotationVector.j;
          float mk = sensorValue.un.geoMagRotationVector.k;
          break;
        }
      default:
        break;
      }
    }
  }

  // --- failover -------------------------------------------------------------
  bool bno_ok = imu_began && (millis() - imu_last_data < IMU_TIMEOUT_MS);

  // Keep the QMI attitude filter warm at ~50 Hz (cheap; safe on the bus), and
  // while the BNO is healthy run the once-per-boot orientation calibration.
  if (qmi_present && (millis() - qmi_last_read >= QMI_READ_MS)) {
    qmi_last_read = millis();
    bool calibrating = bno_ok && !cal_done;
    if (calibrating && !cal_running) {
      cal_running = true; cal_start = millis();
      cal_count = 0; cal_gsum[0] = cal_gsum[1] = cal_gsum[2] = 0;
    }
    qmiUpdate(s, calibrating);                  // advances q_qmi (+ q_align if calibrating)
    if (calibrating && millis() - cal_start > CAL_DURATION_MS) { qmiCalFinalize(); cal_done = true; }
  }

  if (bno_ok) {
    imu_source = IMU_SRC_BNO;
    s->IMU = true;
    // Heading = tilt-compensated CALIBRATED magnetometer (s->mx/my/mz) + gravity
    // (s->gx/gy/gz). Deterministic -> no fusion-yaw drift / 180-deg flips.
    float h = tiltCompassDeg(s->gx, s->gy, s->gz, s->mx, s->my, s->mz,
                             BNO_HEADING_SIGN, BNO_HEADING_OFFSET, s->heading);
    s->heading = h;
    return;                          // primary good — nothing else to do
  }

  // No BNO: try an ICM-20948 (DMP) primary — fully fused, gives heading too.
  if (icm_present) {
    icmUpdate(s);                    // writes state + icm_last_data when FIFO has data
    if (millis() - icm_last_data < IMU_TIMEOUT_MS) {
      imu_source = IMU_SRC_ICM;
      s->IMU = true;
      return;
    }
  }
  // Hot-plug recovery: if the ICM isn't streaming (unplugged, hung, or plugged in
  // after boot), retry the DMP bring-up at ~1 Hz. icmBegin() returns fast when the
  // chip doesn't ACK and reloads the DMP (then resumes) once it reappears — without
  // this, an unplugged ICM never comes back.
  {
    static unsigned long icm_retry = 0;
    if (millis() - icm_retry > 1000) {
      icm_retry = millis();
      if (icmBegin()) {              // chip present again -> DMP reloaded
        icmUpdate(s);
        if (millis() - icm_last_data < IMU_TIMEOUT_MS) {
          imu_source = IMU_SRC_ICM;
          s->IMU = true;
          return;
        }
      }
    }
  }

  // BNO down: attitude from the aligned QMI Mahony filter (no mag -> heading frozen).
  if (!qmi_present) qmi_present = qmiBegin();   // (re)detect if needed
  if (qmi_present) {
    imu_source = IMU_SRC_QMI;
    s->IMU = true;
    float up[3];   qmiUpBody(up);
    float disp[3]; qrot(q_align, up, disp);     // fused gravity vector, display frame
    s->gx = disp[0]; s->gy = disp[1]; s->gz = disp[2];
    // Aligned accel for the slip/turn indicator. q_align maps the QMI's "up" onto
    // the BNO's gravity vector (which points DOWN), so the aligned accel comes out
    // pointing down; the BNO's s->a points UP (accel reaction). Negate to match,
    // otherwise the turn coordinator reads inverted ("works upside down").
    float ad[3]; qrot(q_align, qmi_acc, ad);
    s->ax = s->ax * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * (-ad[0]);
    s->ay = s->ay * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * (-ad[1]);
    s->az = s->az * (1 - ALPHA_ATTITUDE) + ALPHA_ATTITUDE * (-ad[2]);
    float am = sqrt(qmi_acc[0]*qmi_acc[0] + qmi_acc[1]*qmi_acc[1] + qmi_acc[2]*qmi_acc[2]);
    s->g = s->g * (1 - ALPHA_GFORCE) + ALPHA_GFORCE * am / 9.8f;
    if (s->g > s->max_g) s->max_g = s->g;
  } else {
    imu_source = IMU_SRC_NONE;
    s->IMU = false;
  }

  // Reconnect handling. A BNO that has worked once is kept SERVICED every cycle
  // by the `if (imu_began)` block above (imu_began is never cleared once set), so
  // a wire glitch simply resumes: getSensorEvent() starts returning events again
  // when the bus is back, and wasReset() re-enables the reports if the device
  // power-cycled. Re-initializing a still-live BNO (begin_I2C -> a 2nd sh2_open)
  // actually BREAKS that recovery and never comes back — so we don't. We only
  // cold-start with begin_I2C() when the BNO was absent at boot and later appears.
  if (!imu_ever_began && millis() - imu_last_begin > 1000) {
    imu_last_begin = millis();
    Wire.beginTransmission(BNO08X_ADDR);
    if (Wire.endTransmission() == 0 && tryBeginIMU()) {   // present + brought up
      imu_last_data  = millis();
      imu_ever_began = true;
    }
  }
}

// Tilt-compensated magnetic heading.
//
// A raw atan2(mx, mz) compass is only correct when the unit is level: as soon
// as it pitches or rolls, the magnetometer picks up the vertical component of
// the field and the heading swings. We fix that by projecting the magnetometer
// onto the true horizontal plane, which is defined by the gravity vector.
//
//   d  = unit gravity vector              (level -> (0, -1, 0))
//   f  = device forward axis (z), projected onto the horizontal plane
//   r  = f x d  -> a horizontal "right" axis perpendicular to f
//   heading = atan2(mag . r, mag . f)
//
// Because f and r are built from the gravity vector, the result is independent
// of pitch/roll. When level this reduces exactly to the old atan2(mx, mz).
void updateHeading(state *s) {
  // Heading is now set by each source's own fusion: BNO -> ROTATION_VECTOR yaw,
  // ICM -> DMP. The QMI has no magnetometer, so its heading simply holds (no
  // stale-mag drift). This tilt-compensated mag compass is kept only as a
  // reference and no longer drives s->heading for any active source.
  if (imu_source != IMU_SRC_NONE) return;
  // Normalize the gravity ("down") vector.
  float dx = s->gx, dy = s->gy, dz = s->gz;
  float dn = sqrt(dx * dx + dy * dy + dz * dz);
  if (dn > 0) { dx /= dn; dy /= dn; dz /= dn; }

  // Forward axis = device z-hat with its vertical component removed.
  float zd = dz;                       // z-hat . d
  float fx = -zd * dx;
  float fy = -zd * dy;
  float fz = 1.0f - zd * dz;
  float fn = sqrt(fx * fx + fy * fy + fz * fz);

  float a;
  if (fn < 1e-3f) {
    // Nose pointing near-vertical: horizontal heading is undefined here, so
    // fall back to the uncompensated reading rather than dividing by ~0.
    a = 180.0f - atan2(s->mx, s->mz) * 180.0f / PI;
  } else {
    fx /= fn; fy /= fn; fz /= fn;

    // Right axis = forward x down (already unit length, since f is perpendicular
    // to the unit vector d).
    float rx = fy * dz - fz * dy;
    float ry = fz * dx - fx * dz;
    float rz = fx * dy - fy * dx;

    float north = s->mx * fx + s->my * fy + s->mz * fz;
    float east  = s->mx * rx + s->my * ry + s->mz * rz;
    a = 180.0f - atan2(east, north) * 180.0f / PI;
  }

  if (a < 0)      a += 360.0f;
  if (a >= 360.0f) a -= 360.0f;
  s->heading = a;
}