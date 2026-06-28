#pragma once
// ============================================================================
//  config.h — project-wide configuration for the InstrumentPanel EFIS
//
//  Hardware: ESP32-S3 + AXP2101 PMU driving two ST7789 SPI TFTs on SEPARATE
//  SPI buses (the dual-display wiring proven in InstrumentPanel_mini):
//    Display 1 (PFD - Primary Flight Display) : always built, highest priority
//    Display 2 (ND  - Navigation / compass)   : optional, see ENABLE_NAV_DISPLAY
//
//  Sensors (all I2C @ 400 kHz):
//    BNO08x   IMU    0x4A       - attitude (gravity), accel (g/slip), magnetometer
//    ICM-20948 IMU  0x68/0x69   - alt. IMU, e.g. the GY-912 module (auto-detected)
//    BMP390/388 baro 0x77/0x76  - pressure altitude + vertical speed (GY-912 = BMP388)
//    MS4525DO pitot  0x28       - differential pressure -> indicated airspeed
//    PA1010D  GPS    0x10       - lat/lon, GPS altitude, ground speed
//
//  Concurrency: a sensor task (core 0) writes a shared `state` snapshot under a
//  mutex; the PFD task (core 1, high prio) and optional ND task (core 0) read it.
// ============================================================================

// ============================================================================
//  Board selection
//
//  BOARD_A — Waveshare ESP32-S3-LCD-1.69 (non-touch): a SINGLE onboard ST7789
//  (240x280), QMI8658 IMU on I2C, battery sense on GPIO1. The pins below are
//  this board's documented wiring — identical to the original working firmware.
//
//  WARNING: this is a single-display board. Do NOT enable a second display or
//  reuse GPIO1-5 as SPI — GPIO1 is the battery-voltage ADC line and driving it
//  as an output fights the charge/power circuitry (overheating). The earlier
//  "InstrumentPanel_mini" pins were for a different, dual-display board.
//
//  BOARD_C — Waveshare ESP32-S3-Touch-LCD-2.8B (touch): a SINGLE 480x640 RGB-IPS
//  panel (ST7701S). Selects the COMBINED build — PFD (no compass) on top + ND /
//  compass below, on one screen. To use it, set BOARD_C 1 and BOARD_A 0. The
//  dual-display BOARD_A build is left completely unchanged.
//
//  BOARD_D — LilyGO T4-S3 (touch): a SINGLE 2.41" RM690B0 AMOLED (600x450 native,
//  used PORTRAIT 450x600) over QSPI, driven by the LilyGo-AMOLED-Series library.
//  Same COMBINED build as BOARD_C (PFD over ND). GPS UART + sensor I2C are remapped
//  to the board's broken-out header pins (the onboard 6/7 I2C is the touch/PMU bus).
//
//  Enable exactly ONE board.
// ============================================================================
#define BOARD_A 0
#define BOARD_C 0
#define BOARD_D 1

#if (BOARD_A + BOARD_C + BOARD_D) != 1
#error "Enable exactly one board (BOARD_A, BOARD_C, or BOARD_D) in config.h"
#endif

#if BOARD_A

// ---- Display driver selection (define exactly one) -------------------------
#define ST7789                 // onboard panel controller (SPI)

// ---- Display 1 : Primary Flight Display (PFD) — onboard ST7789 -------------
#define LCD_DC     4
#define LCD_CS     5
#define LCD_SCK    6
#define LCD_MOSI   7
#define LCD_RST    8
#define LCD_BL     15
#define LCD_WIDTH    240       // ST7789 panel native size (portrait)
#define LCD_HEIGHT   280
#define SCREEN1_WIDTH   280    // PFD canvas (landscape; tft uses setRotation(1))
#define SCREEN1_HEIGHT  240
#define PFD_CANVAS_ROTATION 2  // matches the original working firmware
#define PFD_TFT_ROTATION    1
// SPI clock. GFX default is conservative; 40 MHz is safe on this board's
// integrated panel (short on-board traces). Lower it if you see display noise.
#define LCD_SPI_SPEED 80000000

// ---- I2C (onboard sensors) -------------------------------------------------
#define IIC_SDA 11
#define IIC_SCL 10

// ---- GPS (u-blox M10 over UART) --------------------------------------------
// The M10 streams standard NMEA at GPS_BAUD by default (no MTK/PMTK config).
// MOVED off 18/17 — those are now the ND display (DIN/CLK). Wire the M10 to the
// pins below (M10 TX -> GPS_RX is the one that matters; GPS_TX is optional).
#define GPS_BAUD 230400       // this M10 streams UBX binary (NMEA off) at 230400 (confirmed via raw scan)
#define GPS_RX   21           // ESP RX  <- M10 TX
#define GPS_TX   14           // ESP TX  -> M10 RX (optional, for config)

// ---- Display 2 : Navigation Display (ND) — external ST7789V2 ---------------
// Wiring: DIN(MOSI)=18, CLK(SCK)=17, CS=16, DC=3, RST=2, BL->VCC (always on).
// On its OWN SPI bus (SPI3_HOST) so its blit transfers in parallel with the PFD.
#define ENABLE_NAV_DISPLAY 1
#define ND_HAS_PANEL       1   // real panel connected
#define SHARED_SPI_BUS     0
#define LCD2_DC   3
#define LCD2_CS   16
#define LCD2_SCK  17
#define LCD2_MOSI 18
#define LCD2_RST  2
#define LCD2_BL   -1           // tied to VCC, not driven
// External panel on jumper wires. 40 MHz is the max clean rate: 80 MHz (the only
// higher value the ESP32 SPI can produce — the clock is quantized to 80/N MHz, so
// 50/60/70 silently run at 40) blacked the panel out over the jumpers. Rewire the
// ND panel with short traces to unlock a clean 80 MHz (would roughly halve the blit).
#define LCD2_SPI_SPEED 40000000   // 80 MHz retried with rewiring — still wouldn't hold over the jumpers; 40 is the clean rate
// ---- ST7789V2 panel geometry — SET THESE TO THE ACTUAL DISPLAY ----
// LCD2_WIDTH/HEIGHT = native portrait size; SCREEN2_* = landscape (rotation 1).
// Offsets place the visible area in the controller's 240x320 GRAM:
//   240x320 (2.0"):  COL 0, ROW 0
//   240x280 (1.69"): COL 0, ROW 20
//   240x240 (1.3"):  COL 0, ROW 0  (or 80 depending on the module)
#define LCD2_WIDTH    240
#define LCD2_HEIGHT   280
#define LCD2_COL_OFFSET 0
#define LCD2_ROW_OFFSET 20    // 240x280 panel offset in the 240x320 GRAM (matches the PFD)
#define SCREEN2_WIDTH   280
#define SCREEN2_HEIGHT  240
#define ND_CANVAS_ROTATION 1   // 90deg CCW from the PFD's landscape (was 2); flip 1<->3 if backwards
#define ND_TFT_ROTATION    1

// ---- Power management (soft power button) ----------------------------------
// Left OFF for now so the firmware drives the minimum set of pins (the board
// runs fine on USB). The original firmware used SYS_EN=41 / SYS_OUT=40; set
// ENABLE_POWER_MGMT to 1 once you've reconfirmed those for battery use.
#define ENABLE_POWER_MGMT 0
#define SYS_EN  41
#define SYS_OUT 40
#define PWR_OFF_HOLD_MS 1000

// ---- Battery (ADC) ---------------------------------------------------------
// GPIO1 is the battery-voltage divider — INPUT ONLY. Only read when power mgmt
// is enabled (never driven as an output).
#define BATT_ADC_PIN   1
#define BATT_ADC_SCALE (9.9f / 4095.0f)   // resistor divider + 12-bit ADC -> volts

#endif // BOARD_A

// ============================================================================
//  BOARD_C — Waveshare ESP32-S3-Touch-LCD-2.8B (COMBINED single-display build)
//
//  A 480x640 RGB-parallel IPS panel (ST7701S). This is NOT an SPI display: the
//  ESP32-S3 LCD_CAM peripheral scans a ~600 KB RGB565 framebuffer out of PSRAM
//  over 16 data lines + DE/HSYNC/VSYNC/PCLK. One combined screen — PFD (compass
//  removed) full-width on top, ND/compass filling the rest below.
//
//  Quirks vs the SPI boards (handled in the combined-display bring-up):
//   * ST7701 init goes over a 3-wire SPI (CLK=2, MOSI=1) BEFORE RGB scan-out.
//   * ST7701 CS + RESET are NOT GPIOs — they hang off an I2C IO-expander
//     (TCA9554-class on the touch I2C): CS = EXIO3, RESET = EXIO1.
//   * Sensors use the broken-out I2C (SDA=15/SCL=7); GPS uses the broken-out
//     UART (TXD=43/RXD=44) — the board's "built-in ports".
//   * Backlight is PWM on GPIO6 (dimmable, unlike the ND's full-VCC panel).
//
//  All values extracted from the Waveshare 2.8B Arduino demo
//  (ESP32-S3-Touch-LCD-2.8B-Demo.zip : LVGL_Arduino/Display_ST7701.cpp/.h).
//  UNTESTED in this firmware — RGB + IO-expander bring-up needs the board. The
//  RGB pins overlap BOARD_A's SPI pins; that's fine (different board/build).
// ============================================================================
#if BOARD_C
#define COMBINED_DISPLAY 1     // single panel: PFD (no compass) stacked over ND

// ---- RGB panel geometry + timing (portrait) --------------------------------
#define RGB_WIDTH       480
#define RGB_HEIGHT      640
#define RGB_PCLK_HZ      9000000   // 9 MHz (~21 Hz refresh) = the measured render-fps PEAK (18.4 fps).
                                   // Lowering the clock frees PSRAM bandwidth for the parallel
                                   // composite (12MHz->16.7, 9MHz->18.4) UP TO A POINT: below ~9 the
                                   // panel frame gets slow enough to gate the render (8MHz dropped to
                                   // ~15.5). So the peak sits just above the render=refresh crossover.
#define RGB_HSYNC_PULSE 10
#define RGB_HSYNC_BACK  70
#define RGB_HSYNC_FRONT 60
#define RGB_VSYNC_PULSE 10
#define RGB_VSYNC_BACK  20
#define RGB_VSYNC_FRONT 20
#define RGB_PCLK_NEG    0          // 0 = latch data on the rising edge

// ---- RGB sync + 16-bit data pins -------------------------------------------
#define RGB_DE    40
#define RGB_VSYNC 39
#define RGB_HSYNC 38
#define RGB_PCLK  41
// RGB565 lane order: B0-4 = demo DATA0-4, G0-5 = DATA5-10, R0-4 = DATA11-15.
// (If red/blue look swapped on hardware, exchange the R* and B* sets.)
#define RGB_B0 5
#define RGB_B1 45
#define RGB_B2 48
#define RGB_B3 47
#define RGB_B4 21
#define RGB_G0 14
#define RGB_G1 13
#define RGB_G2 12
#define RGB_G3 11
#define RGB_G4 10
#define RGB_G5 9
#define RGB_R0 46
#define RGB_R1 3
#define RGB_R2 8
#define RGB_R3 18
#define RGB_R4 17

// ---- ST7701 init bus (3-wire SPI) + IO-expander reset/CS -------------------
#define ST7701_SCLK   2
#define ST7701_MOSI   1
#define LCD_BL        6            // backlight PWM (active high)
#define EXIO_LCD_CS   3            // ST7701 chip-select  (TCA9554 P3)
#define EXIO_LCD_RST  1            // ST7701 reset        (TCA9554 P1)
#define IO_EXPANDER_ADDR 0x20      // TCA9554 7-bit addr (verify on board)

// ---- Sensors on the board's broken-out I2C / UART ("built-in ports") -------
#define IIC_SDA 15
#define IIC_SCL 7
#define GPS_BAUD 230400            // this M10 streams UBX binary (NMEA off) at 230400 (confirmed via raw scan)
#define GPS_RX   44                // ESP RXD <- GPS TX
#define GPS_TX   43                // ESP TXD -> GPS RX

// ---- Battery (ADC) ---------------------------------------------------------
// GPIO4 is the 2.8B's battery-voltage divider (per Waveshare docs: "GPIO4 - used
// for battery voltage reading"). Same ~200K/100K (/3) divider as the other
// Waveshare ESP32-S3 boards, so the BOARD_A scale applies.
#define BATT_ADC_PIN   4
#define BATT_ADC_SCALE (9.9f / 4095.0f)   // resistor divider + 12-bit ADC -> volts

// ---- Combined layout (px from the top; tune on hardware) -------------------
// PFD spans the full 480 width with the bottom heading-arc removed, so it needs
// only its attitude+tapes band; everything below PFD_REGION_H is the ND/compass.
#define PFD_REGION_H 360
#define ND_REGION_H  (RGB_HEIGHT - PFD_REGION_H)   // 280 px for the ND/compass
// The PFD content is raised PFD_SHIFT px (so its top element sits ~20px from the screen
// top), and the ND canvas extends ND_OVERLAP px UP into the freed PFD bottom margin so the
// compass can be larger. The PFD only composites its top (PFD_REGION_H - ND_OVERLAP) rows;
// the ND owns the rest, so the two never overlap in the framebuffer (no 2-core race).
// ND_OVERLAP tracks PFD_SHIFT to keep the heading box ~15px under the (raised) turn
// coordinator. Tune PFD_SHIFT for the top margin; the gap follows automatically.
#define PFD_SHIFT    16   // green FMA text starts at y=0.1*360=36; -16 lands it ~20px from the top
#define ND_OVERLAP   (23 + PFD_SHIFT)               // -> heading box ~15px below the turn coordinator
#define ND_TOP       (PFD_REGION_H - ND_OVERLAP)    // framebuffer row where the ND starts
#define ND_CANVAS_H  (RGB_HEIGHT - ND_TOP)          // ND canvas height (ND_REGION_H + ND_OVERLAP)

// Text auto-scales with the panel: the drawers derive their text size + element
// offsets from the canvas width (layout.h / lyt::txtScale), so there's no build-time
// font knob — the 480-wide combined panel renders text 2x, the 280-wide boards 1x.

// Diagnostic: 1 = draw R/G/B/white bands instead of the instruments, to verify the
// RGB panel comes up (orientation + color-lane order). Set back to 0 for normal use.
#define COMBINED_TEST_PATTERN 0
#endif // BOARD_C

// ============================================================================
//  BOARD_D — LilyGO T4-S3 (2.41" RM690B0 AMOLED, COMBINED single-display build)
//
//  600x450 native AMOLED over QSPI, driven by the LilyGo-AMOLED-Series library
//  (beginAMOLED_241 handles the QSPI bus, RM690B0 init, and the SY6970 PMIC that
//  powers the panel). We render the COMBINED PFD-over-ND frame into 8-bit canvases,
//  convert to RGB565, and pushColors() it each frame (push-based, unlike BOARD_C's
//  continuously-scanned RGB panel). Used PORTRAIT: 450 wide x 600 tall.
//
//  GPS UART + sensor I2C are remapped to the broken-out HEADER pins, because the
//  onboard I2C (GPIO 6/7) is the touch + PMIC bus owned by the display library.
//  *** VERIFY the header pins below against your T4-S3 silkscreen. ***
//
//  Library-owned pins (do NOT reuse): QSPI D0-3 = 14/10/16/12, SCK=15, CS=11,
//  RST=13, TE=18, PMIC-EN=9, touch/PMU I2C = 6/7, SD = 1-4.
//
//  UNTESTED in this firmware — orientation (AMOLED_ROTATION), color/byte order, and
//  the I2C-bus handoff (display init uses 6/7, then we restore Wire to 43/44 for the
//  sensors) are the things most likely to need a tweak on the actual board.
// ============================================================================
#if BOARD_D
#define COMBINED_DISPLAY 1

// AMOLED geometry (portrait). RM690B0 is 600x450 native; we rotate to 450x600.
#define RGB_WIDTH        450       // portrait (driver sets MADCTL MX|MV|RGB; see AMO_* in the .ino)
#define RGB_HEIGHT       600
#define AMOLED_BRIGHTNESS 255      // 0..255 (RM690B0 0x51)

// Sensors live on the Qwiic = the ONBOARD I2C bus (GPIO 6/7), shared with the touch
// (CST226 @0x5A) + PMIC (SY6970 @0x6A). Confirmed by pin scan: BNO 0x4A, BMP 0x77,
// MS4525 0x28 all ACK on 6/7. (The Qwiic is wired to the main bus, not a 43/44 pair.)
#define IIC_SDA 6
#define IIC_SCL 7

// Battery: the T4-S3 has no divider-to-ADC pin — VBAT is read from the SY6970
// PMIC (0x6A on the 6/7 bus) over I2C in the sensor task. See sy6970ReadVbat().
#define BATT_VIA_PMIC 1

// GPS UART on GPIO44 (GPS TX -> ESP RX; confirmed by pin scan: ~1300 B/s of UBX).
// UBX is read-only for us, so no ESP TX.
#define GPS_BAUD 230400
#define GPS_RX   44                // ESP RXD <- GPS TX
#define GPS_TX   -1                // unused (UBX read-only)

// Combined layout (px from the top; tune on hardware) — scaled from BOARD_C 480x640.
#define PFD_REGION_H 338
#define ND_REGION_H  (RGB_HEIGHT - PFD_REGION_H)
#define PFD_SHIFT    15
#define ND_OVERLAP   (23 + PFD_SHIFT)
#define ND_TOP       (PFD_REGION_H - ND_OVERLAP)
#define ND_CANVAS_H  (RGB_HEIGHT - ND_TOP)

#define COMBINED_TEST_PATTERN 0
#endif // BOARD_D

// ============================================================================
//  Board-independent configuration
// ============================================================================

// Only BOARD_C defines COMBINED_DISPLAY; default it off so `#if COMBINED_DISPLAY`
// reads cleanly on the dual-display boards.
#ifndef COMBINED_DISPLAY
#define COMBINED_DISPLAY 0
#endif

#ifndef COMBINED_TEST_PATTERN
#define COMBINED_TEST_PATTERN 0
#endif

// Battery / soft-power are BOARD_A features. The helpers in instrument_drawer.ino
// reference these pins unconditionally but are dead code when ENABLE_POWER_MGMT==0
// (the default on every board), so give boards that don't wire them harmless
// fallbacks just to satisfy the compiler.
#ifndef SYS_EN
#define SYS_EN  -1
#endif
#ifndef SYS_OUT
#define SYS_OUT -1
#endif
#ifndef BATT_ADC_PIN
#define BATT_ADC_PIN -1
#endif
#ifndef BATT_ADC_SCALE
#define BATT_ADC_SCALE 0.0f
#endif
#ifndef BATT_VIA_PMIC                 // BOARD_D reads VBAT from the SY6970 PMIC, not an ADC pin
#define BATT_VIA_PMIC 0
#endif
// True on any board that can report battery voltage (divider+ADC pin, or the PMIC).
#define HAVE_BATTERY (BATT_ADC_PIN >= 0 || BATT_VIA_PMIC)

// ---- IMU options -----------------------------------------------------------
// Supported IMUs are auto-detected at runtime and share the I2C bus (IMU.ino
// failover): BNO08x (0x4A) first, else an ICM-20948 (0x68/0x69, e.g. the GY-912
// module), else the onboard QMI8658 (0x6B/0x6A). ENABLE_ICM20948 compiles the
// ICM-20948 driver (ICM.ino) which runs the chip's on-board DMP: all fusion
// (accel+gyro+AK09916 magnetometer) happens on the IMU and the host just reads the
// fused 9-axis quaternion. Needs the SparkFun ICM-20948 library with DMP support
// enabled (#define ICM_20948_USE_DMP in its src/util/ICM_20948_C.h). 0 = off.
#define ENABLE_ICM20948 1

// BNO heading is taken from its fused ROTATION_VECTOR (the chip's internal
// Kalman), not the noisy raw magnetometer. Tune these to the sensor mounting so
// the compass reads true (check against a known direction): SIGN flips CW/CCW,
// OFFSET rotates magnetic north into place (degrees).
#define HEADING_SIGN   -1.0f
#define HEADING_OFFSET  180.0f

// ICM-20948 (GY-912) heading from the yaw of its DMP 9-axis (magnetometer-
// referenced) quaternion. Same idea as the BNO constants — TUNE to the mounting:
// SIGN flips CW/CCW, OFFSET rotates magnetic north into place (degrees).
#define ICM_HEADING_SIGN    1.0f
#define ICM_HEADING_OFFSET  0.0f

// ---- Navigation display (map) ----------------------------------------------
// ND moving-map chart (airports, runways, navaids, airspace, glide paths, river).
#define MAP_ENABLE      1
#define MAP_RANGE_M     30000     // DISTANCE PARAMETER: metres from the aircraft (center) out to
                                  // the edge of the radar circle. ~16 NM. Integer (the LOD #if needs
                                  // it). Lower -> zoom in (more detail); raise -> see farther + the
                                  // level-of-detail drops (chart_data.h MAP_MAX_TIER). 1000000 = US view.
#define MAP_DEFAULT_LAT  39.1031f // fallback center (downtown Cincinnati) when GPS is lost
#define MAP_DEFAULT_LON -84.5120f
#define MAP_LABELS       1        // draw airport / navaid identifiers next to their symbols
// Outer ring used by the home-plotting overlay (kept in sync with the chart range).
#define ND_MAP_RANGE_M  MAP_RANGE_M

// ---- Rendering tunables ----------------------------------------------------
// The panel has physically rounded corners that clip a ~quarter-circle of this
// radius (px) at each corner. Keep text/readouts inset past it. Tune to taste.
#define SCREEN_CORNER        20
#define FOV                  45        // attitude-indicator vertical FOV (deg)
#define SEALEVELPRESSURE_HPA 1013.25f  // standard QNH (= the gBaroInHg 29.92 default)
// Adjustable local altimeter setting in inHg (the "Kollsman" setting), changed by the
// IO0 button (tap = +0.01, hold = continuous, wraps 31.00 -> 28.00). The altitude calc
// (ASI.ino) uses gBaroInHg * 33.8639 (inHg -> hPa). Defined in InstrumentPanel.ino.
extern volatile float gBaroInHg;
#define BARO_MIN_INHG 28.00f
#define BARO_MAX_INHG 31.00f
#define VSI_FULL_SCALE       20.0f     // ft/SEC at the ends of the VSI bar (+/- climb/descent)
#define VSI_TICK             10.0f     // ft/sec between VSI indentation ticks (0, +/-10, +/-20)
#define GMETER_FS            4.0f      // g-meter full scale (top of bar); over = pulsing red

// ---- Sensor smoothing (exponential moving-average alphas) ------------------
// filtered = (1 - alpha)*previous + alpha*sample.  Smaller alpha = smoother.
#define ALPHA_ATTITUDE 0.2f
#define ALPHA_TURNCOORD 0.15f   // dedicated slip/turn-coordinator ball damping (smaller = smoother)
#define ALPHA_GFORCE   0.1f
#define ALPHA_ALT      0.1f
#define ALPHA_VSPEED   0.1f
#define ALPHA_ASPEED   0.2f
#define ALPHA_BATT     0.02f

// ---- FreeRTOS task layout --------------------------------------------------
// The PFD owns core 1 so its refresh rate stays as high as possible. Sensor
// polling and the lower-priority ND share core 0.
#define CORE_PFD       1
#define CORE_SENSORS   0
#define CORE_ND        0
#define CORE_BLIT      0       // blit shares the sensor core (SPI bus vs the sensors' I2C/UART)
#define PRIO_PFD       10
#define PRIO_SENSORS    5
#define PRIO_BLIT       3      // BELOW sensors so IMU/I2C reads always preempt the blit (smooth data)
#define PRIO_NDBLIT     PRIO_ND
#define PRIO_ND         2      // BELOW the PFD blit (raising it cost the PFD on the OLD layout)
// The draw tasks measured only ~2.3 KB of stack use (high-water mark), so these
// were trimmed from 20000/16000 to free internal SRAM for the Remote ID radios —
// letting the PFD canvas stay in internal SRAM (full fps) with RID on. 6 KB is
// still ~2.6x the measured peak. (sensorTask kept larger for the GPS/I2C parse.)
#define STACK_PFD       6000   // bytes (canvases are heap-allocated, not stack)
#define STACK_SENSORS   8000
#define STACK_ND        6000
#define STACK_BLIT      4096

// ---- Debug -----------------------------------------------------------------
#define DEBUG_SERIAL       1     // 1 = stream fps / battery telemetry over USB
#define DEBUG_PRINT_MS     500   // telemetry interval

// ---- Color palette indices (8-bit indexed canvas) --------------------------
// The RGB565 values for these indices live in color_index[] in the main sketch.
#define IBLACK    0
#define IBLUE     1
#define IRED      2
#define IGREEN    3
#define ICYAN     4
#define IMAGENTA  5
#define IYELLOW   6
#define IWHITE    7
#define ISKY      8
#define IGND      9
#define IGREY    10
#define IDGREY   11    // darker grey: roads (distinct from the IGREY range rings)
#define IORANGE  12    // Remote ID traffic dots (unique; not used by any other symbol)
#define IDBLUE   13    // dim blue: rivers + lakes (water bodies)
#define IMROAD   14    // dim dark grey: regional medium roads (darker than IDGREY interstates)
#define NUM_COLORS 15

// ---- Remote ID receiver (ASTM F3411 / OpenDroneID) ------------------------
// Listens for drone/UAS Remote ID broadcasts on the ESP32's BLE + WiFi radios
// and plots them on the ND (orange dot + altitude in ft). See RemoteID.cpp.
#define RID_ENABLE    1     // master enable for the receiver
#define RID_USE_BLE   1     // Bluetooth LE advertisements (most consumer drones +
                            //   standalone Remote ID modules). REQUIRES arduino-esp32
                            //   core <= 3.3.6 — 3.3.7..3.3.10 have a regression that
                            //   panics S3 BLE startup (issue #12357). See build.sh.
#define RID_USE_WIFI  0     // WiFi beacons (channel-hopping). OFF: the promiscuous
                            //   per-packet callback steals significant CPU (drops
                            //   render fps), and BLE covers most Remote ID. Enable
                            //   for WiFi-broadcasting drones at an fps cost.
#define RID_AGE_MS    12000 // drop a target not heard from within this long (ms)
#define RID_HOP_MS    700   // WiFi channel dwell (ms) — long enough to reliably
                            //   catch a ~1 Hz Remote ID beacon on each visit.
#define RID_TASK_CORE 0     // core for the (low-priority) receiver task
#define RID_TASK_PRIO 1     // low priority — yields to rendering + sensors
// NOTE: the ESP32-S3 Arduino core's NimBLE host is built WITHOUT extended
// advertising, so the BLE scan is BT4-legacy only — Remote ID modules that
// broadcast solely over Bluetooth 5 Long-Range are caught over WiFi instead.
#define RID_DEBUG     1     // print a per-second receiver stats line to Serial
                            //   ([RID] ble_adv/ble_hit/wifi_mgmt/wifi_hit/...) so
                            //   you can see which radio is hearing what. Set 0 later.
// RID_MAX (max simultaneous targets) is defined in State.h.
