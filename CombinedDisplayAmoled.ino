// ============================================================================
//  CombinedDisplayAmoled.ino — BOARD_D (LilyGO T4-S3, 2.41" RM690B0 AMOLED)
//
//  Single 450x600 (portrait) AMOLED over QSPI. COMBINED layout (PFD top, ND below)
//  reusing drawHorizonDisplay/drawNavigationDisplay + the color_index palette.
//
//  SELF-CONTAINED RM690B0 QSPI driver (no LilyGo/SensorLib/XPowersLib). Working
//  settings: PORTRAIT MADCTL 0x00 + 16px col offset; RGB332 8bpp (0x3A=0x02);
//  PMIC-EN(GPIO9) high + SY6970 watchdog off; 80 MHz request (~40 MHz effective).
//
//  PERF / OVERLAP: true async (queued) QSPI DMA does NOT work on this panel —
//  sustained queued transfers never signal completion (post_cb never fires;
//  get_trans_result deadlocks when queued-ahead). So the push stays a reliable
//  POLLING transfer, and the overlap is done with the SECOND CORE instead: core 1
//  finishes the PFD early, then pushes the PREVIOUS (complete, double-buffered)
//  frame WHILE core 0 is still drawing this frame's ND. The 13 ms push hides under
//  the longer ND draw -> ~20 fps, fully reliable. (RGB332 already took ~12 -> 16.)
// ============================================================================
#include "config.h"
#if BOARD_D

#include <Wire.h>
#include "driver/spi_master.h"
#include "Arduino_GFX_Library.h"
#include "MyCanvas8.h"
#include "State.h"
#include "layout.h"
#include "RLE332.h"

extern uint16_t color_index[];
extern HWCDC USBSerial;
extern float gPfdFps;
extern volatile unsigned long gPfdDrawUs;
extern volatile unsigned long gPfdBlitUs;   // reused for the push time (telemetry "blit=")
extern volatile unsigned long gNdDrawUs;    // ND-side draw+encode time (telemetry "nd_draw=")
extern volatile unsigned long gNdBlitUs;    // reused: ND draw-ONLY time (telemetry "nd_blit=")
void getState(state *out);
GFXcanvas8 generate_inc_map(int d, int sc);

// ---- self-contained RM690B0 QSPI driver ------------------------------------
#define AMO_SPI_HOST  SPI3_HOST
#define AMO_QSPI_HZ   80000000               // request 80; GPIO matrix caps effective ~40 MHz (clean)
#define AMO_D0  14
#define AMO_D1  10
#define AMO_D2  16
#define AMO_D3  12
#define AMO_SCK 15
#define AMO_CS  11
#define AMO_RST 13
#define AMO_PMIC_EN 9
#define AMO_SEND_BUF 16384                   // pixels per QSPI chunk
#define AMO_MADCTL  (0x00)                   // portrait (vendor rotation 1)
#define AMO_OFF_X   16
#define AMO_OFF_Y   0
#define AMO_LINE_STRIDE  RLE332_MAX_BYTES(RGB_WIDTH)   // fixed per-line RLE slot = the size cap
#define AMO_PUSH_LPC     36                            // lines/push chunk (36*450=16200 <= AMO_SEND_BUF)

static spi_device_handle_t gAmoSpi = nullptr;
static inline void amoCS(bool low) { digitalWrite(AMO_CS, low ? LOW : HIGH); }

static void amoWrite(uint8_t reg, const uint8_t *params, uint32_t len) {
  spi_transaction_t t; memset(&t, 0, sizeof(t));
  t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.cmd  = 0x02;
  t.addr = (uint32_t)reg << 8;
  if (len) { t.tx_buffer = params; t.length = 8 * len; }
  amoCS(true); spi_device_polling_transmit(gAmoSpi, &t); amoCS(false);
}

static void amoSetAddr(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye) {
  xs += AMO_OFF_X; xe += AMO_OFF_X; ys += AMO_OFF_Y; ye += AMO_OFF_Y;
  uint8_t c[4] = { (uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe };
  uint8_t r[4] = { (uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye };
  amoWrite(0x2A, c, 4);          // CASET
  amoWrite(0x2B, r, 4);          // RASET
  amoWrite(0x2C, NULL, 0);       // RAMWR
}

// Polling quad-write of the RGB332 frame (1 byte/pixel) in SEND_BUF-pixel chunks.
static void amoPush(const uint8_t *data, uint32_t len) {
  const uint8_t *p = data; bool first = true;
  amoCS(true);
  while (len) {
    uint32_t chunk = len > AMO_SEND_BUF ? AMO_SEND_BUF : len;
    spi_transaction_ext_t t; memset(&t, 0, sizeof(t));
    if (first) {
      t.base.flags = SPI_TRANS_MODE_QIO;
      t.base.cmd = 0x32; t.base.addr = 0x002C00; first = false;
    } else {
      t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      t.command_bits = 0; t.address_bits = 0; t.dummy_bits = 0;
    }
    t.base.tx_buffer = p; t.base.length = chunk * 8;   // 8 bits/pixel (RGB332)
    spi_device_polling_transmit(gAmoSpi, (spi_transaction_t *)&t);
    len -= chunk; p += chunk;
  }
  amoCS(false);
}

static bool amoBegin() {
  pinMode(AMO_CS, OUTPUT);      digitalWrite(AMO_CS, HIGH);
  pinMode(AMO_PMIC_EN, OUTPUT); digitalWrite(AMO_PMIC_EN, HIGH);   // enable the panel power rail
  pinMode(AMO_RST, OUTPUT);
  digitalWrite(AMO_RST, HIGH);  delay(20);
  digitalWrite(AMO_RST, LOW);   delay(20);
  digitalWrite(AMO_RST, HIGH);  delay(120);

  spi_bus_config_t bus; memset(&bus, 0, sizeof(bus));
  bus.data0_io_num = AMO_D0; bus.data1_io_num = AMO_D1;
  bus.data2_io_num = AMO_D2; bus.data3_io_num = AMO_D3;
  bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
  bus.sclk_io_num = AMO_SCK;
  bus.max_transfer_sz = (AMO_SEND_BUF * 16) + 8;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
  if (spi_bus_initialize(AMO_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dev; memset(&dev, 0, sizeof(dev));
  dev.command_bits = 8; dev.address_bits = 24; dev.mode = 0;
  dev.clock_speed_hz = AMO_QSPI_HZ; dev.spics_io_num = -1;
  dev.flags = SPI_DEVICE_HALFDUPLEX; dev.queue_size = 1;
  if (spi_bus_add_device(AMO_SPI_HOST, &dev, &gAmoSpi) != ESP_OK) return false;

  static const struct { uint8_t reg, param, len; } seq[] = {
    {0xFE,0x20,0x01},{0x26,0x0A,0x01},{0x24,0x80,0x01},{0x5A,0x51,0x01},{0x5B,0x2E,0x01},
    {0xFE,0x00,0x01},{0x3A,0x02,0x01},{0xC2,0x00,0x21},{0x35,0x00,0x01},{0x51,0x00,0x01},  // 3A=0x02: RGB332
    {0x11,0x00,0x80},{0x29,0x00,0x20},{0x51,0xFF,0x01},
  };
  for (uint32_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
    amoWrite(seq[i].reg, &seq[i].param, seq[i].len & 0x1F);
    if (seq[i].len & 0x80) delay(120);
    if (seq[i].len & 0x20) delay(10);
  }
  uint8_t madctl = AMO_MADCTL;          amoWrite(0x36, &madctl, 1);
  uint8_t br = (uint8_t)AMOLED_BRIGHTNESS; amoWrite(0x51, &br, 1);
  return true;
}

static void sy6970DisableWatchdog() {   // @0x6A on the onboard 6/7 I2C; disable the ~40s watchdog
  const uint8_t ADDR = 0x6A, REG07 = 0x07;
  Wire.beginTransmission(ADDR); Wire.write(REG07);
  if (Wire.endTransmission(false) != 0) { USBSerial.println("AMOLED: SY6970 not found @0x6A"); return; }
  if (Wire.requestFrom(ADDR, (uint8_t)1) != 1) return;
  uint8_t v = Wire.read() & 0xCF;
  Wire.beginTransmission(ADDR); Wire.write(REG07); Wire.write(v); Wire.endTransmission();
  USBSerial.printf("AMOLED: SY6970 watchdog disabled (REG07=0x%02X)\n", v);
}

// ---- combined render state -------------------------------------------------
static MyCanvas8  *gCmbPfd    = nullptr;   // RGB_WIDTH x PFD_REGION_H (top)
static MyCanvas8  *gCmbNd     = nullptr;   // RGB_WIDTH x ND_CANVAS_H  (bottom, overlaps up)
static GFXcanvas8 *gCmbIncMap = nullptr;   // pitch-ladder texture
// Double-buffered per-line RLE frame (PSRAM): line y occupies gComp[b] + y*AMO_LINE_STRIDE,
// holding gLineLen[b][y] encoded bytes. gBounce is the internal-SRAM decode target for the push.
static uint8_t    *gComp[2]   = {nullptr, nullptr};
static uint16_t   *gLineLen[2] = {nullptr, nullptr};
static uint8_t    *gBounce     = nullptr;            // AMO_PUSH_LPC lines, internal SRAM
static volatile int gDrawBuf  = 0;                   // buffer the cores draw into this frame
static uint8_t     gIdx332[NUM_COLORS];    // palette index -> nearest RGB332

// Encode nLines rows of an 8-bit index canvas into the per-line RLE frame at rows
// [frameY0 .. frameY0+nLines). Fused: runs are found on the index bytes and gIdx332 is
// applied only on emit, so a flat line costs ~nothing (skips its run) -> cheaper than a
// straight index->RGB332 convert on our mostly-flat content.
static void amoEncodeRegion(const uint8_t *idx, int nLines, int frameY0,
                            uint8_t *comp, uint16_t *lineLen) {
  for (int r = 0; r < nLines; r++) {
    int y = frameY0 + r;
    lineLen[y] = (uint16_t)rle332_encode_indexed(idx + (size_t)r * RGB_WIDTH, RGB_WIDTH,
                                                 comp + (size_t)y * AMO_LINE_STRIDE, gIdx332);
  }
}

// Decode the RLE frame chunk-by-chunk into the internal bounce buffer and quad-DMA each chunk.
// The DMA reads internal SRAM (not PSRAM); only the small compressed data is read from PSRAM,
// so the push no longer fights the concurrent ND draw for PSRAM bandwidth.
static void amoPushCompressed(const uint8_t *comp, const uint16_t *lineLen) {
  bool first = true;
  amoCS(true);
  for (int y0 = 0; y0 < RGB_HEIGHT; y0 += AMO_PUSH_LPC) {
    int lines = RGB_HEIGHT - y0; if (lines > AMO_PUSH_LPC) lines = AMO_PUSH_LPC;
    for (int j = 0; j < lines; j++) {
      int y = y0 + j;
      rle332_decode(comp + (size_t)y * AMO_LINE_STRIDE, lineLen[y],
                    gBounce + (size_t)j * RGB_WIDTH, RGB_WIDTH);
    }
    spi_transaction_ext_t t; memset(&t, 0, sizeof(t));
    if (first) {
      t.base.flags = SPI_TRANS_MODE_QIO; t.base.cmd = 0x32; t.base.addr = 0x002C00; first = false;
    } else {
      t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      t.command_bits = 0; t.address_bits = 0; t.dummy_bits = 0;
    }
    t.base.tx_buffer = gBounce; t.base.length = (uint32_t)lines * RGB_WIDTH * 8;   // 8 bits/px
    spi_device_polling_transmit(gAmoSpi, (spi_transaction_t *)&t);
  }
  amoCS(false);
}

void combinedDisplayInit() {
  delay(1500);
  bool ok = amoBegin();
  USBSerial.printf("AMOLED(T4-S3): RM690B0 QSPI init -> %s, %dx%d portrait RGB332, CPU %d MHz\n",
                   ok ? "OK" : "FAILED", RGB_WIDTH, RGB_HEIGHT, getCpuFrequencyMhz());
  sy6970DisableWatchdog();

  for (int i = 0; i < NUM_COLORS; i++) {     // index -> RGB332 (top 3R,3G,2B of the RGB565 palette)
    uint16_t c = color_index[i];
    uint8_t R3 = (c >> 13) & 0x07, G3 = (c >> 8) & 0x07, B2 = (c >> 3) & 0x03;
    gIdx332[i] = (R3 << 5) | (G3 << 2) | B2;
  }

  heap_caps_malloc_extmem_enable(100 * 1024);
  static GFXcanvas8 incmap = generate_inc_map(0.6 * PFD_REGION_H, lyt::txtScale(RGB_WIDTH));
  heap_caps_malloc_extmem_enable(256 * 1024);
  gCmbIncMap = &incmap;

  static MyCanvas8 pfd(RGB_WIDTH, PFD_REGION_H, false);
  static MyCanvas8 nd(RGB_WIDTH, ND_CANVAS_H, false);
  // The PFD draw is MEMORY-bound (measured: 35ms internal vs 67ms PSRAM), so it gets
  // the fast internal SRAM. The ND draw is COMPUTE-bound (~same speed either way) -> PSRAM.
  // PFD canvas -> internal SRAM: the PFD draw is MEMORY-bound (measured 39ms internal vs 59ms
  // PSRAM) so the fast RAM matters. ND canvas -> PSRAM: the ND draw is COMPUTE-bound (measured
  // 37ms IDENTICAL in internal vs PSRAM — it's map math/Bresenham, not pixel writes), so SRAM
  // would give it no speedup, and only one large contiguous internal block exists anyway.
  uint8_t *pfdBuf = (uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * PFD_REGION_H,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!pfdBuf)
    pfdBuf = (uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * PFD_REGION_H,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  pfd.useBuffer(pfdBuf);
  nd.useBuffer((uint8_t *)heap_caps_malloc((size_t)RGB_WIDTH * ND_CANVAS_H,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  gCmbPfd = &pfd;
  gCmbNd  = &nd;
  // Per-line RLE frame buffers (PSRAM) + line-length tables (internal) + the internal-SRAM
  // bounce buffer the push decodes into.
  gComp[0] = (uint8_t *)heap_caps_malloc((size_t)RGB_HEIGHT * AMO_LINE_STRIDE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gComp[1] = (uint8_t *)heap_caps_malloc((size_t)RGB_HEIGHT * AMO_LINE_STRIDE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  gLineLen[0] = (uint16_t *)heap_caps_malloc(RGB_HEIGHT * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  gLineLen[1] = (uint16_t *)heap_caps_malloc(RGB_HEIGHT * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  gBounce  = (uint8_t *)heap_caps_malloc((size_t)AMO_PUSH_LPC * RGB_WIDTH, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  amoSetAddr(0, 0, RGB_WIDTH - 1, RGB_HEIGHT - 1);   // address window is constant -> set ONCE
  USBSerial.printf("AMOLED: combined %dx%d RGB332+RLE (stride=%d), iram free=%u\n",
                   RGB_WIDTH, RGB_HEIGHT, (int)AMO_LINE_STRIDE,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// Each core encodes its own region (cores are balanced ~53ms; scheduling rebalances don't
// help). core1 draws+encodes PFD and pushes the previous frame; core0 draws+encodes ND.
static SemaphoreHandle_t gNdStart = nullptr;
static SemaphoreHandle_t gNdDone  = nullptr;
static state gCmbSnap;

void ndDrawTask(void *params) {
  for (;;) {
    xSemaphoreTake(gNdStart, portMAX_DELAY);
    unsigned long t0 = micros();
    drawNavigationDisplay(gCmbNd, &gCmbSnap);
    amoEncodeRegion(gCmbNd->getBuffer(), ND_CANVAS_H, ND_TOP, gComp[gDrawBuf], gLineLen[gDrawBuf]);
    gNdDrawUs = micros() - t0;
    xSemaphoreGive(gNdDone);
  }
}

void combinedTask(void *params) {
  init_state(&gCmbSnap);
  gNdStart = xSemaphoreCreateBinary();
  gNdDone  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(ndDrawTask, "ndDraw", STACK_ND, NULL, PRIO_ND, NULL, CORE_SENSORS);
  int  cur = 0; bool pending = false;
  unsigned long prevTime = millis();
  for (;;) {
#if COMBINED_TEST_PATTERN
    static uint8_t tmp[RGB_WIDTH];
    for (int y = 0; y < RGB_HEIGHT; y++) {
      uint8_t v = y < RGB_HEIGHT / 4 ? 0xE0 : y < RGB_HEIGHT / 2 ? 0x1C
                : y < 3 * RGB_HEIGHT / 4 ? 0x03 : 0xFF;
      memset(tmp, v, RGB_WIDTH);
      gLineLen[0][y] = (uint16_t)rle332_encode(tmp, RGB_WIDTH, gComp[0] + (size_t)y * AMO_LINE_STRIDE);
    }
    amoPushCompressed(gComp[0], gLineLen[0]);
    vTaskDelay(200);
    continue;
#endif
    getState(&gCmbSnap);
    unsigned long t0 = micros();

    gDrawBuf = cur;
    xSemaphoreGive(gNdStart);   // core 0: draw + encode ND of `cur` (in parallel)

    if (pending) amoPushCompressed(gComp[1 - cur], gLineLen[1 - cur]);   // push prev (overlaps ND draw)
    unsigned long tPush = micros();

    drawHorizonDisplay(gCmbPfd, gCmbIncMap, &gCmbSnap, /*showCompass*/ false);
    amoEncodeRegion(gCmbPfd->getBuffer(), ND_TOP, 0, gComp[cur], gLineLen[cur]);
    unsigned long tDraw = micros();

    xSemaphoreTake(gNdDone, portMAX_DELAY);
    pending = true;
    cur = 1 - cur;

    gPfdDrawUs = tDraw - tPush;         // PFD draw + encode (core 1)
    gPfdBlitUs = tPush - t0;            // push of the previous frame
    unsigned long now = millis(), dt = now - prevTime; prevTime = now;
    if (dt > 0) gPfdFps = 0.9f * gPfdFps + 0.1f * (1000.0f / dt);
    vTaskDelay(1);
  }
}

#endif // BOARD_D
