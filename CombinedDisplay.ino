// ============================================================================
//  CombinedDisplay.ino — BOARD_C (Waveshare ESP32-S3-Touch-LCD-2.8B)
//
//  A single 480x640 RGB-parallel IPS panel (ST7701S) showing BOTH instruments:
//  the PFD (compass removed) full-width on top, the ND/compass filling the rest
//  below. Reuses drawHorizonDisplay()/drawNavigationDisplay() and the color_index
//  palette unchanged — only the *output* differs: instead of an SPI blit, each
//  8-bit indexed canvas is converted straight into a region of the panel's PSRAM
//  framebuffer via Arduino_GFX's drawIndexedBitmap().
//
//  Bring-up (mirrors the Waveshare 2.8B demo, Display_ST7701.cpp):
//    1. Backlight PWM on GPIO6.
//    2. TCA9554 IO-expander (I2C @ 0x20): all pins outputs; EXIO8 low.
//    3. ST7701 hardware reset via EXIO1.
//    4. ST7701 register init over a 3-wire 9-bit SPI bit-banged on CLK=2/MOSI=1,
//       with CS held low via EXIO3 for the whole sequence.
//    5. Start the RGB scan-out (Arduino_ESP32RGBPanel + Arduino_RGB_Display).
//
//  UNTESTED on hardware — RGB timing + the IO-expander bit mapping + the data-lane
//  (R/B) order are the things most likely to need a tweak on the actual board.
//  The whole file compiles to nothing unless BOARD_C selects COMBINED_DISPLAY.
// ============================================================================
#include "config.h"
#if BOARD_C            // RGB-parallel ST7701 panel; BOARD_D (AMOLED) has its own file

#include <Wire.h>
#include "driver/spi_master.h"
#include "Arduino_GFX_Library.h"
#include "MyCanvas8.h"
#include "State.h"
#include "layout.h"

// Cross-file symbols (defined in the main sketch / drawers).
extern uint16_t color_index[];
extern HWCDC USBSerial;
void getState(state *out);
GFXcanvas8 generate_inc_map(int d, int sc);

// ---- TCA9554 IO-expander ---------------------------------------------------
#define TCA9554_OUTPUT_REG 0x01
#define TCA9554_CONFIG_REG 0x03

static uint8_t gExio = 0xFF;   // mirror of the output register (1 = high)

static uint8_t exioWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IO_EXPANDER_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission();             // 0 = ACK/success
}

// pin is 1-based (EXIO_PINx); the chip drives bit (pin-1).
// Non-static so Touch.cpp can drive the GT911 reset line (TCA9554 P1 = EXIO2).
void exioSet(uint8_t pin, bool high) {
  uint8_t mask = (uint8_t)(1u << (pin - 1));
  if (high) gExio |= mask; else gExio &= ~mask;
  exioWriteReg(TCA9554_OUTPUT_REG, gExio);
}

static void exioInit() {
  exioSet(8, false);                         // EXIO8 low (mirrors the demo); rest high
  exioWriteReg(TCA9554_OUTPUT_REG, gExio);
  uint8_t ack = exioWriteReg(TCA9554_CONFIG_REG, 0x00);   // 0 = output, all pins
  USBSerial.printf("COMBINED: TCA9554@0x%02X config-write ACK=%u (0=ok, nonzero=absent/NACK)\n",
                   IO_EXPANDER_ADDR, ack);
}

// ---- ST7701 3-wire (9-bit) SPI on hardware SPI2_HOST (CLK=2 / MOSI=1) -------
// Mirrors the Waveshare demo exactly: a 1-bit command phase carries D/C and an
// 8-bit address phase carries the byte, so each transfer is the ST7701's 9-bit
// frame. CS (EXIO3) is held low across the whole init by the caller (the device
// is spics_io_num=-1). HW SPI replaced an earlier bit-bang.
static spi_device_handle_t gSt7701Spi = NULL;

static void st7701SpiInit() {
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = ST7701_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = ST7701_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 64;
  esp_err_t e1 = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 1;       // D/C
  devcfg.address_bits = 8;       // data byte
  devcfg.mode = 0;               // SPI mode 0
  devcfg.clock_speed_hz = 10 * 1000 * 1000;
  devcfg.spics_io_num = -1;      // CS driven manually via the IO-expander
  devcfg.queue_size = 1;
  esp_err_t e2 = spi_bus_add_device(SPI2_HOST, &devcfg, &gSt7701Spi);
  USBSerial.printf("COMBINED: ST7701 SPI bus_init=%d add_device=%d\n", (int)e1, (int)e2);
}

static void st7701_write9(bool dc, uint8_t v) {
  spi_transaction_t t = {};
  t.cmd = dc ? 1 : 0;            // 1-bit D/C phase
  t.addr = v;                    // 8-bit data phase
  t.length = 0;                  // no trailing data bytes
  spi_device_transmit(gSt7701Spi, &t);
}

// Init stream encoded as 16-bit tokens (verbatim from the 2.8" demo):
//   0x0xx = command byte, 0x1xx = data byte, 0x2xx = delay xx ms.
#define STCMD(x)   (uint16_t)(x)
#define STDAT(x)   (uint16_t)(0x100 | (x))
#define STDLY(x) (uint16_t)(0x200 | (x))
static const uint16_t st7701_init[] = {
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x13),
  STCMD(0xEF), STDAT(0x08),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x10),
  STCMD(0xC0), STDAT(0x4F),STDAT(0x00),
  STCMD(0xC1), STDAT(0x10),STDAT(0x02),
  STCMD(0xC2), STDAT(0x07),STDAT(0x02),
  STCMD(0xCC), STDAT(0x10),
  STCMD(0xB0), STDAT(0x00),STDAT(0x10),STDAT(0x17),STDAT(0x0D),STDAT(0x11),STDAT(0x06),STDAT(0x05),STDAT(0x08),STDAT(0x07),STDAT(0x1F),STDAT(0x04),STDAT(0x11),STDAT(0x0E),STDAT(0x29),STDAT(0x30),STDAT(0x1F),
  STCMD(0xB1), STDAT(0x00),STDAT(0x0D),STDAT(0x14),STDAT(0x0E),STDAT(0x11),STDAT(0x06),STDAT(0x04),STDAT(0x08),STDAT(0x08),STDAT(0x20),STDAT(0x05),STDAT(0x13),STDAT(0x13),STDAT(0x26),STDAT(0x30),STDAT(0x1F),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x11),
  STCMD(0xB0), STDAT(0x65),
  STCMD(0xB1), STDAT(0x71),
  STCMD(0xB2), STDAT(0x82),
  STCMD(0xB3), STDAT(0x80),
  STCMD(0xB5), STDAT(0x42),
  STCMD(0xB7), STDAT(0x85),
  STCMD(0xB8), STDAT(0x20),
  STCMD(0xC0), STDAT(0x09),
  STCMD(0xC1), STDAT(0x78),
  STCMD(0xC2), STDAT(0x78),
  STCMD(0xD0), STDAT(0x88),
  STCMD(0xEE), STDAT(0x42),
  STCMD(0xE0), STDAT(0x00),STDAT(0x00),STDAT(0x02),
  STCMD(0xE1), STDAT(0x04),STDAT(0xA0),STDAT(0x06),STDAT(0xA0),STDAT(0x05),STDAT(0xA0),STDAT(0x07),STDAT(0xA0),STDAT(0x00),STDAT(0x44),STDAT(0x44),
  STCMD(0xE2), STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),STDAT(0x00),
  STCMD(0xE3), STDAT(0x00),STDAT(0x00),STDAT(0x22),STDAT(0x22),
  STCMD(0xE4), STDAT(0x44),STDAT(0x44),
  STCMD(0xE5), STDAT(0x0C),STDAT(0x90),STDAT(0xA0),STDAT(0xA0),STDAT(0x0E),STDAT(0x92),STDAT(0xA0),STDAT(0xA0),STDAT(0x08),STDAT(0x8C),STDAT(0xA0),STDAT(0xA0),STDAT(0x0A),STDAT(0x8E),STDAT(0xA0),STDAT(0xA0),
  STCMD(0xE6), STDAT(0x00),STDAT(0x00),STDAT(0x22),STDAT(0x22),
  STCMD(0xE7), STDAT(0x44),STDAT(0x44),
  STCMD(0xE8), STDAT(0x0D),STDAT(0x91),STDAT(0xA0),STDAT(0xA0),STDAT(0x0F),STDAT(0x93),STDAT(0xA0),STDAT(0xA0),STDAT(0x09),STDAT(0x8D),STDAT(0xA0),STDAT(0xA0),STDAT(0x0B),STDAT(0x8F),STDAT(0xA0),STDAT(0xA0),
  STCMD(0xEB), STDAT(0x00),STDAT(0x00),STDAT(0xE4),STDAT(0xE4),STDAT(0x44),STDAT(0x00),STDAT(0x40),
  STCMD(0xED), STDAT(0xFF),STDAT(0xF5),STDAT(0x47),STDAT(0x6F),STDAT(0x0B),STDAT(0xA1),STDAT(0xAB),STDAT(0xFF),STDAT(0xFF),STDAT(0xBA),STDAT(0x1A),STDAT(0xB0),STDAT(0xF6),STDAT(0x74),STDAT(0x5F),STDAT(0xFF),
  STCMD(0xEF), STDAT(0x08),STDAT(0x08),STDAT(0x08),STDAT(0x40),STDAT(0x3F),STDAT(0x64),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x00),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x13),
  STCMD(0xE6), STDAT(0x16),STDAT(0x7C),
  STCMD(0xE8), STDAT(0x00),STDAT(0x0E),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x00),
  STCMD(0x11), STDLY(200),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x13),
  STCMD(0xE8), STDAT(0x00),STDAT(0x0C), STDLY(150),
  STCMD(0xE8), STDAT(0x00),STDAT(0x00),
  STCMD(0xFF), STDAT(0x77),STDAT(0x01),STDAT(0x00),STDAT(0x00),STDAT(0x00),
  STCMD(0x29),
  STCMD(0x35), STDAT(0x00),
  STCMD(0x11), STDLY(200),
  STCMD(0x29), STDLY(100),
};
#undef STCMD
#undef STDAT
#undef STDLY

static void st7701SendInit() {
  for (size_t i = 0; i < sizeof(st7701_init) / sizeof(st7701_init[0]); i++) {
    uint16_t t = st7701_init[i];
    if (t & 0x200)      delay(t & 0xFF);
    else                st7701_write9((t & 0x100) != 0, (uint8_t)(t & 0xFF));
  }
}

// ---- RGB panel + combined render state -------------------------------------
static Arduino_RGB_Display *gRgb       = nullptr;
static MyCanvas8           *gCmbPfd    = nullptr;   // 480 x PFD_REGION_H (top)
static MyCanvas8           *gCmbNd     = nullptr;   // 480 x ND_REGION_H  (bottom)
static GFXcanvas8          *gCmbIncMap = nullptr;   // pitch-ladder texture

static void i2cScan() {
  USBSerial.print("COMBINED: I2C scan:");
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { USBSerial.printf(" 0x%02X", a); n++; }
  }
  USBSerial.printf("  (%u found; expect 0x20 = TCA9554)\n", n);
}

// Bring the panel up and allocate the framebuffer + canvases. Called once from
// setup() (BEFORE the tasks start) so the I2C expander traffic during the ST7701
// init can't race the sensor task on the shared I2C bus.
void combinedDisplayInit() {
  // 1. Backlight PWM (active high) — dimmable, unlike the SPI-board panels.
  ledcAttach(LCD_BL, 20000, 10);
  ledcWrite(LCD_BL, 1023);

  // 2. Diagnostics, the IO-expander, and the ST7701's 3-wire HW SPI bus.
  delay(2500);                               // DIAG: let the native-USB CDC settle so the scan prints survive
  i2cScan();                                 // confirm the TCA9554 (and sensors) ACK
  exioInit();
  st7701SpiInit();                           // claims GPIO 1/2 via SPI2_HOST

  // 3. Reset (EXIO1), then 4. the register init with CS (EXIO3) held low.
  exioSet(EXIO_LCD_RST, false); delay(20);
  exioSet(EXIO_LCD_RST, true);  delay(50);
  exioSet(EXIO_LCD_CS, false);
  st7701SendInit();
  exioSet(EXIO_LCD_CS, true);
  USBSerial.println("COMBINED: ST7701 reset + HW-SPI init sent");

  // 5. RGB scan-out. A bounce buffer (= demo's 10*height) avoids PSRAM-bandwidth
  //    "line drift" on this panel. bus=NULL/rst=NONE -> begin() skips any panel-IC
  //    SPI init (we already did it above) and just starts the RGB DMA.
  Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
      RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK,
      RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4,
      RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
      RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4,
      // NOTE: Arduino_GFX inverts this internally — it sets esp_lcd
      // hsync_idle_low = (polarity==0)?1:0. The Waveshare demo runs with
      // hsync/vsync_idle_low = 0, so we must pass polarity = 1 (passing 0
      // mis-synced the panel -> garbage vertical lines).
      /*hsync_polarity*/ 1, RGB_HSYNC_FRONT, RGB_HSYNC_PULSE, RGB_HSYNC_BACK,
      /*vsync_polarity*/ 1, RGB_VSYNC_FRONT, RGB_VSYNC_PULSE, RGB_VSYNC_BACK,
      RGB_PCLK_NEG, RGB_PCLK_HZ, /*useBigEndian*/ false,
      /*de_idle_high*/ 0, /*pclk_idle_high*/ 0, /*bounce_px*/ (size_t)(10 * RGB_HEIGHT));

  gRgb = new Arduino_RGB_Display(RGB_WIDTH, RGB_HEIGHT, panel, 0, /*auto_flush*/ false,
                                 /*bus*/ nullptr, /*rst*/ GFX_NOT_DEFINED,
                                 /*init_operations*/ nullptr, /*len*/ 0);
  if (!gRgb->begin()) {
    USBSerial.println("COMBINED: RGB panel begin() FAILED (framebuffer alloc?)");
  } else {
    USBSerial.printf("COMBINED: RGB panel up, fb=%p\n", (void *)gRgb->getFramebuffer());
  }
  gRgb->fillScreen(0x0000);   // RGB565 black
  gRgb->flush();

  // Full-resolution (crisp). PFD canvas -> fast INTERNAL SRAM (heavy render target);
  // the big pitch-ladder texture -> PSRAM to make room; ND canvas -> PSRAM. Text is
  // magnified in the drawers via FONT_SCALE.
  heap_caps_malloc_extmem_enable(100 * 1024);     // route the ~180KB inc_map to PSRAM
  static GFXcanvas8 incmap = generate_inc_map(0.6 * PFD_REGION_H, lyt::txtScale(RGB_WIDTH));
  heap_caps_malloc_extmem_enable(256 * 1024);     // restore default threshold
  gCmbIncMap = &incmap;

  static MyCanvas8 pfd(RGB_WIDTH, PFD_REGION_H, false);
  static MyCanvas8 nd(RGB_WIDTH, ND_CANVAS_H, false);   // taller: extends ND_OVERLAP px up
  // The PFD draw is MEMORY-bound, so the PFD canvas MUST be in fast internal SRAM
  // for full fps (in PSRAM the draw ~doubles and fps halves). It's allocated here,
  // first, while a 170 KB contiguous internal block is still free; the BLE
  // controller (~71 KB internal) and the display task stacks then fit in what's
  // left ONLY because the tasks are created before remoteid_begin() (see setup())
  // and WiFi RID is off (RID_USE_WIFI) — together that keeps this internal.
  // Put the canvas in PSRAM when the WiFi stack needs the internal SRAM: CONFIG (AP) mode,
  // or flight mode with WiFi RID on (gRidWifi -> a standalone WiFi monitor needs ~40 KB
  // internal). Both run at lower fps, acceptable for those modes. Otherwise (plain flight)
  // keep the canvas internal for full fps.
  bool apMode = webConfigApMode() || gRidWifi;
  const char *pfdWhere = apMode ? "PSRAM (config mode)" : "INTERNAL";
  uint8_t *pfdBuf = apMode ? nullptr
    : (uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * PFD_REGION_H, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!pfdBuf) { if (!apMode) pfdWhere = "PSRAM (fallback)";
    pfdBuf = (uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * PFD_REGION_H,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  USBSerial.printf("COMBINED: PFD canvas in %s; internal free=%u\n", pfdWhere,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  pfd.useBuffer(pfdBuf);
  nd.useBuffer((uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * ND_CANVAS_H,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  gCmbPfd = &pfd;   // the drawers set canvas->textScale from width (lyt::txtScale)
  gCmbNd  = &nd;
  USBSerial.printf("COMBINED: full-res, text x%d; iram free=%u\n", lyt::txtScale(RGB_WIDTH),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// Parallel render: the ND draws + composites its region on core 0 while the PFD does
// the same on core 1. Viable only with enough PSRAM-bandwidth headroom (low pixel
// clock) — at 16/24 MHz the two cores' framebuffer traffic starved the LCD DMA.
static SemaphoreHandle_t gNdStart = nullptr;
static SemaphoreHandle_t gNdDone  = nullptr;
static state gCmbSnap;   // shared frame snapshot (written before gNdStart, then read-only)

// Composite an index region into the RGB565 framebuffer, TWO pixels per 32-bit store.
// The framebuffer region starts on a 4-byte boundary (RGB_WIDTH and ND_TOP are even), so
// the uint32 writes are aligned. Halves the store count -> bursts PSRAM better than
// one uint16 at a time (the composite was store-bound, ~13 MB/s, well under PSRAM BW).
static inline void compositeRegion(uint16_t *fb, const uint8_t *idx, int npix) {
  uint32_t *fb32 = (uint32_t *)fb;
  int i = 0, half = npix >> 1;
  for (int j = 0; j < half; j++, i += 2)
    fb32[j] = (uint32_t)color_index[idx[i]] | ((uint32_t)color_index[idx[i + 1]] << 16);
  if (npix & 1) fb[npix - 1] = color_index[idx[npix - 1]];   // odd tail
}

void ndDrawTask(void *params) {
  for (;;) {
    xSemaphoreTake(gNdStart, portMAX_DELAY);
    unsigned long t0 = micros();
    drawNavigationDisplay(gCmbNd, &gCmbSnap);
    unsigned long tDraw = micros();
    compositeRegion(gRgb->getFramebuffer() + RGB_WIDTH * ND_TOP,   // ND region (overlaps up)
                    gCmbNd->getBuffer(), RGB_WIDTH * ND_CANVAS_H);
    gNdDrawUs = tDraw - t0;            // ND draw (map+compass)
    gNdBlitUs = micros() - tDraw;      // ND composite (LUT -> framebuffer)
    xSemaphoreGive(gNdDone);
  }
}

// Render loop: PFD (no compass) into the top band, ND/compass into the bottom,
// each converted indexed->RGB565 straight into its framebuffer region.
void combinedTask(void *params) {
  init_state(&gCmbSnap);
  gNdStart = xSemaphoreCreateBinary();
  gNdDone  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(ndDrawTask, "ndDraw", STACK_ND, NULL, PRIO_ND, NULL, CORE_SENSORS);
  unsigned long prevTime = millis();
  for (;;) {
#if COMBINED_TEST_PATTERN
    // Diagnostic bands top->bottom: red, green, blue, white. Correct output = four
    // horizontal stripes in that order (panel up + portrait orientation + R/B lane
    // order correct). Vertical stripes = transposed; garbage = init/timing wrong.
    gRgb->fillRect(0, 0,                  RGB_WIDTH, RGB_HEIGHT / 4, 0xF800); // red
    gRgb->fillRect(0, RGB_HEIGHT / 4,     RGB_WIDTH, RGB_HEIGHT / 4, 0x07E0); // green
    gRgb->fillRect(0, RGB_HEIGHT / 2,     RGB_WIDTH, RGB_HEIGHT / 4, 0x001F); // blue
    gRgb->fillRect(0, 3 * RGB_HEIGHT / 4, RGB_WIDTH, RGB_HEIGHT / 4, 0xFFFF); // white
    gRgb->flush();
    vTaskDelay(200);
    continue;
#endif
    getState(&gCmbSnap);

    unsigned long t0 = micros();
    xSemaphoreGive(gNdStart);   // core 0: draw ND + composite its region, in parallel
    drawHorizonDisplay(gCmbPfd, gCmbIncMap, &gCmbSnap, /*showCompass*/ false);
    unsigned long tPfd = micros();

    // Composite the PFD region on this core (core 0 handles the ND region). Only the top
    // ND_TOP rows are written; the ND owns the rest (its overlap covers the PFD's black bottom).
    compositeRegion(gRgb->getFramebuffer(), gCmbPfd->getBuffer(), RGB_WIDTH * ND_TOP);
    gPfdBlitUs = micros() - tPfd;             // PFD composite (LUT -> framebuffer)

    xSemaphoreTake(gNdDone, portMAX_DELAY);   // ND draw + composite finished on core 0
    gRgb->flush();
    gPfdDrawUs = micros() - t0;

    unsigned long now = millis();
    unsigned long dt  = now - prevTime;
    prevTime = now;
    if (dt > 0) gPfdFps = 0.9f * gPfdFps + 0.1f * (1000.0f / dt);

    vTaskDelay(1);
  }
}

#endif // BOARD_C
