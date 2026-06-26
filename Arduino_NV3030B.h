#pragma once
// ============================================================================
//  Arduino_NV3030B — Arduino_GFX driver for the NV3030B TFT controller
//  (e.g. the 1.83" 240x280 IPS module used as the ND).
//
//  The NV3030B is ST7789-command-compatible enough that the ST7789 driver shows
//  *an* image, but its gamma/power/VCOM and pixel-order (BGR) + inversion are
//  different — using the ST7789 init left the black level / contrast wrong.
//  Init sequence ported from nstepanets/NV3030B (Adafruit_GFX) into Arduino_GFX's
//  batch DSL.
// ============================================================================
#include "Arduino_GFX_Library.h"

// Exact init from the Waveshare/SpotPear 1.83" 240x280 NV3030B demo
// (LCD_1.83_Code/ESP32/.../LCD_Driver.cpp) — its gamma/VCOM/COLMOD give the
// correct black level + colors (the generic nstepanets values washed blacks out).
static const uint8_t nv3030b_init_operations[] = {
    BEGIN_WRITE,
    WRITE_C8_BYTES, 0xFD, 2, 0x06, 0x08,                       // unlock private regs
    WRITE_C8_BYTES, 0x61, 2, 0x07, 0x04,
    WRITE_C8_BYTES, 0x62, 3, 0x00, 0x44, 0x45,                 // bias
    WRITE_C8_BYTES, 0x63, 4, 0x41, 0x07, 0x12, 0x12,
    WRITE_C8_D8, 0x64, 0x37,
    WRITE_C8_BYTES, 0x65, 3, 0x09, 0x10, 0x21,                 // pump1 / VSP
    WRITE_C8_BYTES, 0x66, 3, 0x09, 0x10, 0x21,                 // pump2 / AVCL
    WRITE_C8_BYTES, 0x67, 2, 0x21, 0x40,                       // pump_sel
    WRITE_C8_BYTES, 0x68, 4, 0x90, 0x4C, 0x50, 0x70,           // gamma vap/van; 0x50 = VCOM
    WRITE_C8_BYTES, 0xB1, 3, 0x0F, 0x02, 0x01,                 // frame rate
    WRITE_C8_D8, 0xB4, 0x01,
    WRITE_C8_BYTES, 0xB5, 4, 0x02, 0x02, 0x0A, 0x14,           // porch
    WRITE_C8_BYTES, 0xB6, 5, 0x04, 0x01, 0x9F, 0x00, 0x02,
    WRITE_C8_D8, 0xDF, 0x11,                                   // gofc_gamma_en_sel
    WRITE_C8_BYTES, 0xE2, 6, 0x03, 0x00, 0x00, 0x30, 0x33, 0x3F,   // gamma +3
    WRITE_C8_BYTES, 0xE5, 6, 0x3F, 0x33, 0x30, 0x00, 0x00, 0x03,   // gamma -3
    WRITE_C8_BYTES, 0xE1, 2, 0x05, 0x67,                           // gamma +2
    WRITE_C8_BYTES, 0xE4, 2, 0x67, 0x06,                           // gamma -2
    WRITE_C8_BYTES, 0xE0, 8, 0x05, 0x06, 0x0A, 0x0C, 0x0B, 0x0B, 0x13, 0x19,  // gamma +1
    WRITE_C8_BYTES, 0xE3, 8, 0x18, 0x13, 0x0D, 0x09, 0x0B, 0x0B, 0x05, 0x06,  // gamma -1
    WRITE_C8_BYTES, 0xE6, 2, 0x00, 0xFF,
    WRITE_C8_BYTES, 0xE7, 6, 0x01, 0x04, 0x03, 0x03, 0x00, 0x12,
    WRITE_C8_BYTES, 0xE8, 3, 0x00, 0x70, 0x00,                 // source
    WRITE_C8_D8, 0xEC, 0x52,                                   // gate timing
    WRITE_C8_BYTES, 0xF1, 3, 0x01, 0x01, 0x02,                 // tearing effect
    WRITE_C8_BYTES, 0xF6, 4, 0x01, 0x30, 0x00, 0x00,           // interface ctrl
    WRITE_C8_BYTES, 0xFD, 2, 0xFA, 0xFC,                       // lock private regs
    WRITE_C8_D8, 0x3A, 0x55,                                   // COLMOD: 16-bit RGB565
    WRITE_C8_D8, 0x35, 0x00,                                   // TEON
    WRITE_COMMAND_8, 0x21,                                     // INVON (inversion)
    WRITE_COMMAND_8, 0x11,                                     // SLPOUT
    END_WRITE,
    DELAY, 120,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,                                     // DISPON
    END_WRITE,
    DELAY, 120,
};

#define NV3030B_MADCTL     0x36
#define NV3030B_MADCTL_MY  0x80
#define NV3030B_MADCTL_MX  0x40
#define NV3030B_MADCTL_MV  0x20
#define NV3030B_MADCTL_BGR 0x08

class Arduino_NV3030B : public Arduino_TFT {
public:
  Arduino_NV3030B(Arduino_DataBus *bus, int8_t rst = GFX_NOT_DEFINED, uint8_t r = 0,
                  bool ips = true, int16_t w = 240, int16_t h = 280,
                  uint8_t col_offset1 = 0, uint8_t row_offset1 = 0,
                  uint8_t col_offset2 = 0, uint8_t row_offset2 = 0)
      : Arduino_TFT(bus, rst, r, ips, w, h, col_offset1, row_offset1, col_offset2, row_offset2) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
#if defined(ESP32)
    _override_datamode = SPI_MODE3;   // same bus mode the ST7789 driver used here
#endif
    return Arduino_TFT::begin(speed);
  }

  void setRotation(uint8_t r) override {
    Arduino_TFT::setRotation(r);
    // RGB order (the Waveshare demo uses MADCTL base 0x00, not BGR).
    uint8_t mad;
    switch (_rotation) {
      case 1:  mad = NV3030B_MADCTL_MX | NV3030B_MADCTL_MV; break;
      case 2:  mad = NV3030B_MADCTL_MX | NV3030B_MADCTL_MY; break;
      case 3:  mad = NV3030B_MADCTL_MY | NV3030B_MADCTL_MV; break;
      default: mad = 0x00; break;   // case 0
    }
    _bus->beginWrite();
    _bus->writeC8D8(NV3030B_MADCTL, mad);
    _bus->endWrite();
  }

  void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override {
    if ((x != _currentX) || (w != _currentW)) {
      _currentX = x; _currentW = w;
      x += _xStart;
      _bus->writeC8D16D16(0x2A, x, x + w - 1);   // CASET
    }
    if ((y != _currentY) || (h != _currentH)) {
      _currentY = y; _currentH = h;
      y += _yStart;
      _bus->writeC8D16D16(0x2B, y, y + h - 1);   // RASET
    }
    _bus->writeCommand(0x2C);                     // RAMWR
  }

  void invertDisplay(bool i) override { _bus->sendCommand((_ips ^ i) ? 0x21 : 0x20); }
  void displayOn(void) override       { _bus->sendCommand(0x11); delay(120); }
  void displayOff(void) override      { _bus->sendCommand(0x10); delay(120); }

protected:
  void tftInit() override {
    if (_rst != GFX_NOT_DEFINED) {
      pinMode(_rst, OUTPUT);
      digitalWrite(_rst, HIGH); delay(100);
      digitalWrite(_rst, LOW);  delay(120);
      digitalWrite(_rst, HIGH); delay(120);
    } else {
      _bus->sendCommand(0x01);   // SWRESET
      delay(120);
    }
    _bus->batchOperation(nv3030b_init_operations, sizeof(nv3030b_init_operations));
  }
};
