// ============================================================================
//  InstrumentPanel — a DIY glass-cockpit EFIS for ESP32-S3
//
//  Top-level sketch: wires up the hardware, builds the display(s), and starts
//  the FreeRTOS tasks. The actual rendering lives in instrument_drawer.ino and
//  the sensor drivers in IMU.ino / ASI.ino / GPS.ino.
//
//  Task layout (see config.h to tune cores/priorities):
//    sensorTask  core 0           - polls all I2C sensors into gLocal
//    pfdTask     core 1, high pri  - renders the Primary Flight Display
//    ndTask      core 0, low pri   - renders the Navigation Display (optional)
//    loop()      core 1, low pri   - soft power button + debug telemetry
//
//  Cross-task data flows through the mutex-guarded global `State`.
// ============================================================================

#include <SPI.h>
#include <Wire.h>

#include <Arduino.h>
#include "HWCDC.h"
#include "esp_task_wdt.h"
#include <Preferences.h>   // NVS storage for the altimeter setting (recalled after power-off)
#include "Arduino_GFX_Library.h"

#include "config.h"      // pins, screen sizes, tunables, color indices
#include "State.h"       // shared flight-state struct + init/copy helpers
#include "RemoteID.h"    // FAA Remote ID (OpenDroneID) BLE + WiFi traffic receiver
#include "map_zoom.h"    // runtime touch-zoom range/tier + field-mode state
#include "Touch.h"       // GT911/CST226 tap reader that drives the zoom
#include "MyCanvas8.h"   // GFXcanvas8 subclass with a rotation matrix (ND uses it)
#include "layout.h"      // resolution-independent layout math (txtScale, etc.)
#include "chart_data.h"  // ND moving-map data + MapProj (types needed before auto-prototypes)

HWCDC USBSerial;


// RGB565 values for the 8-bit indexed canvas palette (indices defined in config.h)
uint16_t color_index[NUM_COLORS] = {
  0x0000,  // IBLACK
  0x001F,  // IBLUE
  0xF800,  // IRED
  0x07E0,  // IGREEN
  0x07FF,  // ICYAN
  0xF81F,  // IMAGENTA
  0xFFE0,  // IYELLOW
  0xFFFF,  // IWHITE
  0x055F,  // ISKY
  0xD421,  // IGND
  0xAD55,  // IGREY
  0x4208,  // IDGREY (dark grey for roads)
  0xFD20,  // IORANGE (Remote ID traffic)
  0x000C,  // IDBLUE (dark blue water: (0,0,98) on RGB565, -> RGB332 0x01 on the LilyGO)
  0x2104   // IMROAD (dim dark grey for medium roads — darker than IDGREY interstates)
};

// Local altimeter setting (inHg, the "Kollsman" value). The IO0 button adjusts it
// (see loop()); the altitude calc (ASI.ino) uses gBaroInHg * 33.8639 -> hPa. Persisted
// to NVS (debounced) so it survives power-off.
volatile float gBaroInHg = 29.92f;
Preferences    baroPrefs;

// ----------------------------------------------------------------------------
//  Shared state + locking
// ----------------------------------------------------------------------------
state State;        // the single shared snapshot (guarded by bigLock)
state gLocal;       // the sensor task's private working copy

SemaphoreHandle_t bigLock = NULL;

void bigLock_lock()   { xSemaphoreTake(bigLock, portMAX_DELAY); }
void bigLock_unlock() { xSemaphoreGive(bigLock); }

void getState(state *out) {       // display tasks: read a consistent snapshot
  bigLock_lock();
  copy_state(out, &State);
  bigLock_unlock();
}

void setState(state *s) {         // sensor task: publish the latest readings
  bigLock_lock();
  copy_state(&State, s);
  bigLock_unlock();
}

// ----------------------------------------------------------------------------
//  Display bus serialization
//
//  When both panels share one SPI bus, concurrent transactions from the two
//  display tasks (running on different cores) would corrupt each other, so
//  every blit is wrapped in busLock()/busUnlock(). With separate buses these
//  are no-ops and the panels blit in parallel.
// ----------------------------------------------------------------------------
#if SHARED_SPI_BUS
SemaphoreHandle_t gDisplayBusLock = NULL;
static inline void busLock()   { if (gDisplayBusLock) xSemaphoreTake(gDisplayBusLock, portMAX_DELAY); }
static inline void busUnlock() { if (gDisplayBusLock) xSemaphoreGive(gDisplayBusLock); }
#else
static inline void busLock()   {}
static inline void busUnlock() {}
#endif

// Frame-rate estimates (single writer each, read for telemetry — races benign)
float gPfdFps = 0;
float gNdFps  = 0;
volatile uint32_t gSensCount = 0;   // sensorTask loop count (-> sensor publish rate in telemetry)
// Diagnostic timing (microseconds) for the PFD render vs blit
volatile unsigned long gPfdDrawUs = 0;
volatile unsigned long gPfdBlitUs = 0;
volatile unsigned long gNdDrawUs  = 0;
volatile unsigned long gNdBlitUs  = 0;

// ----------------------------------------------------------------------------
//  Display construction
// ----------------------------------------------------------------------------
#if !COMBINED_DISPLAY   // BOARD_A/dual-display SPI path; the combined RGB build
                        // (CombinedDisplay.ino) replaces all of this.
Arduino_GFX *makeDisplay1() {
#ifdef ST7789
  // DMA bus: writeIndexedPixels() converts indexed->RGB565 in 1 KB chunks and
  // DMAs each chunk while the CPU converts the next, so the per-pixel conversion
  // overlaps the SPI transfer (blit ~= the raw transfer time, not conv+transfer).
  Arduino_DataBus *bus = new Arduino_ESP32SPIDMA(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
  // Waveshare 1.69 (240x280) panel offsets — matches the original firmware.
  return new Arduino_ST7789(bus, LCD_RST, 0, true, LCD_WIDTH, LCD_HEIGHT, 0, 20, 0, 0);
#endif
#ifdef ST7701
  Arduino_DataBus *bus = new Arduino_ESP32RGBPanel();
  return new Arduino_S7701S(bus, 1, true, 240, 320, 0, 20, 0, 0);
#endif
}

#if ENABLE_NAV_DISPLAY
Arduino_GFX *makeDisplay2() {
  // The ND runs on its OWN SPI bus so its blit transfers in parallel with the
  // PFD's. The PFD uses the default host (HSPI -> SPI2_HOST); pass spi_num=2 so
  // the ND lands on SPI3_HOST. Same yielding DMA bus as the PFD.
  Arduino_DataBus *bus = new Arduino_ESP32SPIDMA(LCD2_DC, LCD2_CS, LCD2_SCK, LCD2_MOSI,
                                                 GFX_NOT_DEFINED, 2 /* SPI3_HOST */);
  // ST7789V2 panel (same proven driver as the PFD). Offsets depend on the panel
  // size — see LCD2_* in config.h (set to match the actual display).
  return new Arduino_ST7789(bus, LCD2_RST, 0, true, LCD2_WIDTH, LCD2_HEIGHT,
                            LCD2_COL_OFFSET, LCD2_ROW_OFFSET, 0, 0);
}
#endif

// Fast full-frame blit of an 8-bit indexed canvas.
//
// Arduino_GFX::drawIndexedBitmap() draws pixel-by-pixel via writePixel(), which
// re-sends a 1x1 address window (CASET/RASET/RAMWR) for EVERY pixel — ~400 ms
// for a 240x240 frame. Instead we set the address window ONCE for the whole
// frame and stream the pixels with writeIndexedPixels() in a single
// transaction. Our displays are Arduino_ST7789 (an Arduino_TFT), so the cast is
// valid. The DMA bus (see makeDisplay1) overlaps the indexed->RGB565 conversion
// with the SPI transfer chunk-by-chunk inside this single call.
static inline void blitIndexed(Arduino_GFX *gfx, uint8_t *buf, int16_t w, int16_t h) {
  Arduino_TFT *tft = (Arduino_TFT *)gfx;
  tft->startWrite();
  tft->writeAddrWindow(0, 0, w, h);
  tft->writeIndexedPixels(buf, color_index, (uint32_t)w * h);
  tft->endWrite();
}

// ---- PFD double buffering ---------------------------------------------------
// Two canvases: the PFD task draws into one while this dedicated blit task pushes
// the OTHER to the panel, so the ~11 ms draw overlaps the ~18 ms blit (frame ->
// max(draw, blit) instead of draw+blit). The blit task sits on the sensor core
// but at lower priority, so the sensor I2C/UART reads always preempt it — the
// blit only uses SPI, so there's no bus contention, and the SPI DMA keeps
// running across a preemption.
static Arduino_GFX      *gPfdTft       = nullptr;
static MyCanvas8        *gPfdCanvas[2] = {nullptr, nullptr};
static volatile int      gBlitIdx      = 0;
static SemaphoreHandle_t gBlitReady    = nullptr;   // PFD -> blit: "buffer gBlitIdx is ready"
static SemaphoreHandle_t gBlitDone     = nullptr;   // blit -> PFD: "blit finished, buffer free"

void blitTask(void *params) {
  for (;;) {
    xSemaphoreTake(gBlitReady, portMAX_DELAY);
    unsigned long t = micros();
    busLock();
    blitIndexed(gPfdTft, gPfdCanvas[gBlitIdx]->getBuffer(), SCREEN1_WIDTH, SCREEN1_HEIGHT);
    busUnlock();
    gPfdBlitUs = micros() - t;
    xSemaphoreGive(gBlitDone);
  }
}
#endif // !COMBINED_DISPLAY (display construction + blit task)

// ----------------------------------------------------------------------------
//  Tasks
// ----------------------------------------------------------------------------

// Polls every sensor and publishes the result. Fast sensors (IMU) are read
// every loop; the slower I2C devices are throttled to ~25 Hz.
void sensorTask(void *params) {
  unsigned long lastSlow = 0;
  for (;;) {
    unsigned long now = millis();
    // GPS / baro / airspeed at 25 Hz: these are EXPENSIVE reads (GPS UART parse, baro/airspeed
    // I2C) and slow-changing values, so 25 Hz keeps the alt/airspeed tapes visually smooth without
    // stealing core-0 time from the displays (bumping to 50 Hz cost the PFD ~5 fps). The fast-moving
    // sensor — the IMU attitude — streams far above the fps and is drained every loop below.
    if (now - lastSlow > 40) {
      updateGPS(&gLocal);
      updateBPS(&gLocal);
      updateASI(&gLocal);
      remoteid_fill(&gLocal);     // pull in Remote ID traffic (cheap; ages out stale targets)
      touchPoll(&gLocal);         // capacitive tap -> map zoom in/out (top/bottom half)
#if BATT_VIA_PMIC
      updateBatteryPMIC();        // BOARD_D: sample VBAT from the SY6970 PMIC (owns the I2C bus here)
#endif
      lastSlow = now;
    }
    updateIMU(&gLocal);            // IMU as fast as it streams (FIFO drained each loop)
    updateHeading(&gLocal);        // tilt-compensated compass heading
    setState(&gLocal);
    gSensCount++;                  // publish-rate counter (telemetry)
    vTaskDelay(1);
  }
}

#if !COMBINED_DISPLAY   // dual-display PFD + ND tasks (combined build: combinedTask)
// Primary Flight Display: artificial horizon, speed/alt tapes, VSI, G, etc.
void pfdTask(void *params) {
  state snap;
  init_state(&snap);

  // Double-buffered canvases (MyCanvas8 -> rotated text for the heading arc).
  static MyCanvas8 canvas0(SCREEN1_WIDTH, SCREEN1_HEIGHT);
  static MyCanvas8 canvas1(SCREEN1_WIDTH, SCREEN1_HEIGHT);
  canvas0.setRotation(PFD_CANVAS_ROTATION);          // horizon fast-path honors this
  canvas1.setRotation(PFD_CANVAS_ROTATION);
  gPfdCanvas[0] = &canvas0;
  gPfdCanvas[1] = &canvas1;

  // The pitch-ladder texture is expensive to build, so generate it once and
  // sample it every frame inside drawHorizonDisplay().
  GFXcanvas8 inc_map = generate_inc_map(0.6 * canvas0.height(), lyt::txtScale(canvas0.width()));

  gPfdTft = makeDisplay1();
  busLock();
  gPfdTft->begin(LCD_SPI_SPEED);
  gPfdTft->setRotation(PFD_TFT_ROTATION);
  gPfdTft->fillScreen(IBLACK);
  busUnlock();

  // Spin up the blit task now that the panel + canvases are ready.
  gBlitReady = xSemaphoreCreateBinary();
  gBlitDone  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(blitTask, "blit", STACK_BLIT, NULL, PRIO_BLIT, NULL, CORE_BLIT);

  int  cur         = 0;
  bool blitPending = false;
  unsigned long prevTime = millis();
  for (;;) {
    getState(&snap);
    unsigned long _t0 = micros();
    drawHorizonDisplay(gPfdCanvas[cur], &inc_map, &snap, true);   // draw into the current buffer
    gPfdDrawUs = micros() - _t0;

    // Wait for the previous blit to finish before reusing/handing off buffers,
    // then hand this freshly-drawn buffer to the blit task (it blits while we
    // draw the next frame into the other buffer).
    if (blitPending) xSemaphoreTake(gBlitDone, portMAX_DELAY);
    gBlitIdx = cur;
    xSemaphoreGive(gBlitReady);
    blitPending = true;
    cur ^= 1;

    unsigned long now = millis();
    unsigned long dt  = now - prevTime;
    prevTime = now;
    if (dt > 0) gPfdFps = 0.9f * gPfdFps + 0.1f * (1000.0f / dt);

    vTaskDelay(1);   // brief yield (loopTask telemetry / idle watchdog)
  }
}

#if ENABLE_NAV_DISPLAY
static MyCanvas8 *gNdCanvas[2] = {nullptr, nullptr};

// ---- ND double buffering (mirrors the PFD) ---------------------------------
// A dedicated blit task pushes one ND buffer to the panel while ndTask draws the next,
// so the draw overlaps the blit (frame -> max(draw, blit)). This is the ONE ND
// optimization that pays off: ~16 -> ~18 fps with NO PFD cost (the blit task is pri 2,
// below the PFD blit's pri 3, so it only takes leftover core-0 time). Beyond this the
// ND is BLIT-BOUND and stuck near ~18fps, all verified on-device:
//   * The blit floor is the 40MHz SPI transfer. The ESP32 SPI clock is quantized to
//     80/N MHz, so the only higher value is 80MHz, which blacks out the ND panel over
//     its jumper wiring (50/60/70 silently run at 40). A clean 80MHz (which would
//     roughly halve the blit) needs the ND panel rewired with short traces.
//   * Moving the ND canvas to internal SRAM did NOT help (the blit is transfer+
//     contention-bound, not memory-bound) and forced the PFD pitch-ladder into PSRAM.
//     Raising the ND priority just stole core-0 CPU from the PFD blit (bad trade).
//     The two displays share core 0's blit budget; this layout favors the PFD.
static Arduino_GFX      *gNdTft       = nullptr;
static volatile int      gNdBlitIdx   = 0;
static SemaphoreHandle_t gNdBlitReady = nullptr;
static SemaphoreHandle_t gNdBlitDone  = nullptr;

#if ND_HAS_PANEL
void ndBlitTask(void *params) {
  for (;;) {
    xSemaphoreTake(gNdBlitReady, portMAX_DELAY);
    unsigned long t = micros();
    busLock();
    blitIndexed(gNdTft, gNdCanvas[gNdBlitIdx]->getBuffer(), SCREEN2_WIDTH, SCREEN2_HEIGHT);
    busUnlock();
    gNdBlitUs = micros() - t;
    xSemaphoreGive(gNdBlitDone);
  }
}
#endif

// Navigation Display: rotating compass card / HSI. Double-buffered in PSRAM (internal
// SRAM is reserved for the PFD's two canvases + pitch-ladder texture).
void ndTask(void *params) {
  state snap;
  init_state(&snap);

  const size_t ndsz = (size_t)SCREEN2_WIDTH * SCREEN2_HEIGHT;
  static MyCanvas8 nd0(SCREEN2_WIDTH, SCREEN2_HEIGHT, false);
  static MyCanvas8 nd1(SCREEN2_WIDTH, SCREEN2_HEIGHT, false);
  nd0.useBuffer((uint8_t *)heap_caps_malloc(ndsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  nd1.useBuffer((uint8_t *)heap_caps_malloc(ndsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  nd0.setRotation(ND_CANVAS_ROTATION);
  nd1.setRotation(ND_CANVAS_ROTATION);
  gNdCanvas[0] = &nd0;
  gNdCanvas[1] = &nd1;

#if ND_HAS_PANEL
  gNdTft = makeDisplay2();
  busLock();
  gNdTft->begin(LCD2_SPI_SPEED);
  gNdTft->setRotation(ND_TFT_ROTATION);
  gNdTft->fillScreen(0x0000);
  busUnlock();
  gNdBlitReady = xSemaphoreCreateBinary();
  gNdBlitDone  = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(ndBlitTask, "ndblit", STACK_BLIT, NULL, PRIO_NDBLIT, NULL, CORE_BLIT);
#endif

  int  cur         = 0;
  bool blitPending = false;
  unsigned long prevTime = millis();
  for (;;) {
    getState(&snap);
    unsigned long t0 = micros();
    drawNavigationDisplay(gNdCanvas[cur], &snap);
    gNdDrawUs = micros() - t0;

#if ND_HAS_PANEL
    if (blitPending) xSemaphoreTake(gNdBlitDone, portMAX_DELAY);
    gNdBlitIdx = cur;
    xSemaphoreGive(gNdBlitReady);
    blitPending = true;
#endif
    cur ^= 1;

    unsigned long now = millis();
    unsigned long dt  = now - prevTime;
    prevTime = now;
    if (dt > 0) gNdFps = 0.9f * gNdFps + 0.1f * (1000.0f / dt);

    vTaskDelay(1);
  }
}
#endif
#endif // !COMBINED_DISPLAY (pfd + nd tasks)

// ----------------------------------------------------------------------------
//  Setup / loop
// ----------------------------------------------------------------------------
#if COMBINED_DISPLAY
void combinedDisplayInit();
void combinedTask(void *params);
#endif
void setup(void) {
#if ENABLE_POWER_MGMT
  pinMode(SYS_EN, OUTPUT);
  digitalWrite(SYS_EN, HIGH);     // latch system power on
  pinMode(SYS_OUT, INPUT);
#endif

#if !COMBINED_DISPLAY
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#if ENABLE_NAV_DISPLAY && (LCD2_BL >= 0)
  pinMode(LCD2_BL, OUTPUT);
  digitalWrite(LCD2_BL, HIGH);
#endif
#endif // !COMBINED_DISPLAY (the combined build PWMs the backlight in combinedDisplayInit)

  USBSerial.begin(115200);
  delay(500);

  // The blit task saturates core 0 (the higher-priority sensor task still
  // preempts it), so the core-0 idle task barely runs and would trip the Task
  // Watchdog. Tear the TWDT down entirely rather than leave it half-disabled
  // (which spams "task_wdt: task not found" from the idle hook). The interrupt
  // watchdog still catches genuine ISR hangs.
  disableLoopWDT();        // unsubscribe the Arduino loopTask first
  esp_task_wdt_deinit();   // then tear down the TWDT (it unsubscribes the idles)

  // 8 MB octal PSRAM is enabled, but PSRAM is slower than internal SRAM. By
  // default Arduino sends allocations >16 KB to PSRAM; raise that limit so the
  // speed-critical PFD buffers (two 67 KB canvases + the 83 KB pitch-ladder
  // texture, which is sampled per-pixel) stay in fast internal SRAM. Second-
  // display (ND) buffers are placed in PSRAM explicitly where they're created.
  heap_caps_malloc_extmem_enable(256 * 1024);

  Wire.begin(IIC_SDA, IIC_SCL);   // explicit I2C pins for this board
  Wire.setClock(400000);

  pinMode(0, INPUT_PULLUP);        // IO0 (BOOT button) — adjusts the altimeter setting
  baroPrefs.begin("baro", false);  // recall the saved altimeter setting (default 29.92)
  gBaroInHg = baroPrefs.getFloat("inHg", 29.92f);
  if (gBaroInHg < BARO_MIN_INHG || gBaroInHg > BARO_MAX_INHG) gBaroInHg = 29.92f;

  mapZoomInit();                  // set the boot map zoom level (range/tier)

  init_state(&State);
  init_state(&gLocal);

  initIMU(&gLocal);
  initASI(&gLocal);
  initGPS(&gLocal);
  initBPS(&gLocal);
  copy_state(&State, &gLocal);

  bigLock = xSemaphoreCreateMutex();
#if SHARED_SPI_BUS
  gDisplayBusLock = xSemaphoreCreateMutex();
#endif

#if ENABLE_NAV_DISPLAY
  // One-shot, contention-free benchmark: how long does an ND frame take to draw
  // in PSRAM vs internal SRAM? (Runs before the tasks start, so it's clean.)
  {
    const size_t ndsz = (size_t)SCREEN2_WIDTH * SCREEN2_HEIGHT;
    for (int pass = 0; pass < 2; pass++) {
      uint32_t cap = (pass == 0) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                 : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      uint8_t *b = (uint8_t *)heap_caps_malloc(ndsz, cap);
      if (!b) { USBSerial.printf("ND bench %s: alloc failed\n", pass ? "internal" : "PSRAM"); continue; }
      MyCanvas8 nd(SCREEN2_WIDTH, SCREEN2_HEIGHT, false);
      nd.useBuffer(b);
      nd.setRotation(ND_CANVAS_ROTATION);
      unsigned long t0 = micros();
      for (int i = 0; i < 8; i++) drawNavigationDisplay(&nd, &gLocal);
      unsigned long us = (micros() - t0) / 8;
      USBSerial.printf("ND draw bench (%-8s): %lu us/frame (%.0f fps draw-only)\n",
                       pass ? "internal" : "PSRAM", us, 1e6 / (double)us);
      heap_caps_free(b);
    }
  }
#endif

#if COMBINED_DISPLAY
  combinedDisplayInit();   // bring up the RGB panel before the tasks start (so the
                           // ST7701/expander I2C traffic can't race the sensor task)
#endif

  touchInit();             // GT911/CST226 reset + start (AFTER the expander is up; no-op on BOARD_A)

  xTaskCreatePinnedToCore(sensorTask, "sensors", STACK_SENSORS, NULL, PRIO_SENSORS, NULL, CORE_SENSORS);
#if COMBINED_DISPLAY
  xTaskCreatePinnedToCore(combinedTask, "combined", STACK_PFD, NULL, PRIO_PFD, NULL, CORE_PFD);
#else
  xTaskCreatePinnedToCore(pfdTask,    "pfd",     STACK_PFD,     NULL, PRIO_PFD,     NULL, CORE_PFD);
#if ENABLE_NAV_DISPLAY
  xTaskCreatePinnedToCore(ndTask,     "nd",      STACK_ND,      NULL, PRIO_ND,      NULL, CORE_ND);
#endif
#endif

  // Start Remote ID LAST, AFTER the render/sensor tasks have grabbed their stacks
  // from contiguous internal SRAM. The BLE controller's ~70 KB of internal allocs
  // otherwise fragments the heap so the (later) task stacks can't fit — even with
  // the PFD canvas internal. The delay lets combinedTask spin up its ndDrawTask
  // (created on first run) before the radios carve up what's left.
  delay(150);
  remoteid_begin();        // FAA Remote ID (BLE; WiFi optional) traffic receiver
}

// loop() runs in the Arduino loopTask (core 1, low priority). It only handles
// the soft power button and optional debug telemetry — the displays and sensors
// are driven by their own tasks above.
void loop() {
#if ENABLE_POWER_MGMT
  // Soft power button: long-press latches SYS_EN low to power down.
  // TODO(board A): enable once SYS_EN/SYS_OUT pins are wired and set in config.h.
  static bool          pressed   = false;
  static unsigned long pressTime = 0;

  if (!digitalRead(SYS_OUT)) {                 // button held (active low)
    if (!pressed) { pressed = true; pressTime = millis(); }
    else if (millis() - pressTime > PWR_OFF_HOLD_MS) digitalWrite(SYS_EN, LOW);
  } else {
    pressed = false;
  }
#endif

  // IO0 (BOOT button): tap = +0.01 inHg on the altimeter setting; hold (>450 ms) =
  // continuous. One button, so it only counts up and wraps 31.00 -> 28.00.
  {
    static bool bprev = false, bdirty = false;
    static uint32_t bpress = 0, brepeat = 0, bsaveT = 0;
    bool bdown = (digitalRead(0) == LOW);
    uint32_t bnow = millis();
    bool tap = bdown && !bprev;
    bool rep = bdown && bprev && (bnow - bpress > 450) && (bnow - brepeat > 90);
    if (tap) bpress = bnow;
    if (tap || rep) {
      brepeat = bnow;
      gBaroInHg = roundf((gBaroInHg + 0.01f) * 100.0f) / 100.0f;
      if (gBaroInHg > BARO_MAX_INHG + 0.005f) gBaroInHg = BARO_MIN_INHG;
      bdirty = true; bsaveT = bnow;
    }
    if (bdirty && bnow - bsaveT > 1500) {        // persist once the user settles (coalesces a scroll into one write)
      baroPrefs.putFloat("inHg", gBaroInHg);
      bdirty = false;
    }
    bprev = bdown;
  }

#if DEBUG_SERIAL
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > DEBUG_PRINT_MS) {
    lastPrint = millis();
#if ENABLE_POWER_MGMT
    USBSerial.printf("pfd_fps=%.1f nd_fps=%.1f vbat=%.2f\n",
                     gPfdFps, gNdFps, readBatteryVoltage());
#else
    int isrc = imuSource();
    const char *src = isrc == 1 ? "BNO" : isrc == 2 ? "QMI" : isrc == 3 ? "ICM" : "--";
    static uint32_t lastSensCount = 0; static unsigned long lastSensT = 0;
    unsigned long sdt = millis() - lastSensT;
    int sensHz = sdt ? (int)((gSensCount - lastSensCount) * 1000UL / sdt) : 0;
    lastSensCount = gSensCount; lastSensT = millis();
    USBSerial.printf("pfd_fps=%.1f nd_fps=%.1f sens=%dHz draw=%luus blit=%luus nd_draw=%luus nd_blit=%luus iram=%u psram=%u/%u IMU=%d(%s) BPS=%d ASI=%d GPS=%d\n",
                     gPfdFps, gNdFps, sensHz, gPfdDrawUs, gPfdBlitUs, gNdDrawUs, gNdBlitUs,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize(),
                     gLocal.IMU, src, gLocal.BPS, gLocal.ASI, gLocal.GPS);
#endif
  }
#endif

  vTaskDelay(20);
}
