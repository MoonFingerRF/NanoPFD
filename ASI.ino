// ============================================================================
//  ASI.ino — air-data drivers
//
//  Two sensors live here because the airspeed calculation needs both:
//    MS4525DO  (pitot)  - differential pressure -> indicated airspeed
//    BMP390    (baro)   - static pressure -> altitude + vertical speed, and the
//                         air density (P, T) used to convert pitot dP to speed
//
//  SEALEVELPRESSURE_HPA is defined in config.h.
// ============================================================================
#include <Adafruit_I2CDevice.h>

#include "Adafruit_BMP3XX.h"

Adafruit_BMP3XX bmp;

#define ASI_ADDR 0x28

class MS4525DO {
public:
  float pressure, tempurature;
  int status;
  uint8_t addr;
  uint8_t buf[4];
  static constexpr float P_CNT_ = 16383;
  static constexpr float T_CNT_ = 2047;
  static constexpr float T_MAX_ = 150;
  static constexpr float T_MIN_ = -50;
  static constexpr float c_ = 0.1f;
  static constexpr float d_ = 0.8f;
  Adafruit_I2CDevice *i2c_dev = NULL;
  MS4525DO(uint8_t addr_ = 0x28) {
    pressure = 0;
    tempurature = 0;
    status = 0;
    addr = addr_;
    i2c_dev = new Adafruit_I2CDevice(addr);
    for (int n = 0; n < 4; n++)
      buf[n] = 0;
  }
  bool begin() {
    return i2c_dev->begin();
  }
  bool read() {
    if (!i2c_dev->read(buf, 4))
      return false;
    status = buf[0] >> 6;
    if (status != 0)
      return false;
    int p_raw = (unsigned int)(buf[0] & 0x3F) << 8 | (unsigned int)buf[1];
    int t_raw = (unsigned int)buf[2] << 8 | (unsigned int)(buf[3] >> 5);

    float p_min_ = -1.0;
    float p_max_ = 1.0;
    pressure = (((float)(p_raw)-c_ * P_CNT_) * ((p_max_ - p_min_) / (d_ * P_CNT_)) + p_min_) * 0.45359237f * 9.80665f / 0.0254f / 0.0254f;
    tempurature = ((float)(t_raw) * (T_MAX_ - T_MIN_) / T_CNT_) + T_MIN_;

    return true;
  }
};

MS4525DO pitot(ASI_ADDR);

float pitot_offset = 0;
float pitot_pressure = 0;

void initASI(state *s) {
  if (pitot.begin()) {
    int N = 0;
    for (int n = 0; n < 10; n++) {
      if (pitot.read()) {
        N++;
        pitot_offset += pitot.pressure;
      }
    }
    if (N > 0) {
      pitot_offset /= N;
      s->ASI = true;
    } else {
      s->ASI = false;
    }
  } else {
    s->ASI = false;
  }
}

void updateASI(state *s) {
  if (pitot.read()) {
    s->ASI = true;
    pitot_pressure = pitot_pressure * 0.9 + 0.1 * (pitot_offset - pitot.pressure);
    // Airspeed from dynamic pressure needs valid static air density (P, T) from
    // the baro. If the baro isn't present/ready, hold the previous estimate
    // rather than dividing by a stale or zero pressure.
    if (s->BPS && bmp.pressure > 0) {
      float airTemp = bmp.temperature + 273.15f;   // K
      float v = 2.23694f * sqrt(fabsf(pitot_pressure) * 2 * 287.05f * airTemp / bmp.pressure);
      s->air_speed = s->air_speed * (1 - ALPHA_ASPEED) + ALPHA_ASPEED * v;
    }
  } else {
    s->ASI = false;
  }
}

bool BPS_First_Alt;

// State for the vertical-speed derivative
unsigned long vs_last_time = 0;     // millis() of the previous VS sample
float         vs_last_alt  = 0;     // altitude (ft) at the previous VS sample

void initBPS(state *s) {
  s->BPS = bmp.begin_I2C();

  if (s->BPS) {
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    bmp.setOutputDataRate(BMP3_ODR_50_HZ);
  }

  BPS_First_Alt = false;
  vs_last_time = 0;

  bmp.performReading();
}

void updateBPS(state *s) {
  if (bmp.performReading()) {
    if (!BPS_First_Alt) {
      // Capture the ground (home) altitude by averaging the first samples.
      float avg = 0;
      for (int n = 0; n < 10; n++) {
        avg += bmp.readAltitude(gBaroInHg * 33.8639f);
      }
      s->home_alt = avg / 10.0;
      BPS_First_Alt = true;
    }

    float new_alt = s->alt * (1 - ALPHA_ALT) + ALPHA_ALT * 3.28084 * bmp.readAltitude(gBaroInHg * 33.8639f);

    // Vertical speed = d(altitude)/dt, expressed in ft/min and smoothed.
    unsigned long now = millis();
    if (vs_last_time != 0) {
      float dt = (now - vs_last_time) / 1000.0f;   // seconds
      if (dt > 0) {
        float vs = (new_alt - vs_last_alt) / dt * 60.0f;   // ft/min (instantaneous)
        s->vertical_speed = s->vertical_speed * (1 - ALPHA_VSPEED) + ALPHA_VSPEED * vs;
      }
    }
    vs_last_time = now;
    vs_last_alt  = new_alt;

    s->alt = new_alt;
    s->BPS = true;
  } else {
    s->BPS = false;
  }
}