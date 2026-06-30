// ============================================================================
//  RemoteID.cpp — passive FAA Remote ID (ASTM F3411 / OpenDroneID) receiver.
//  (A .cpp, not .ino, so it is its own translation unit — its NimBLE + esp_wifi
//   includes and static helpers stay out of the sketch's .ino concatenation,
//   which otherwise hoists auto-prototypes above the types they reference.)
//
//  Drones/UAS that comply with the FAA Remote ID rule broadcast their ID,
//  position and altitude over either Bluetooth or WiFi using the OpenDroneID
//  message format. This module listens on BOTH of the ESP32-S3's radios and
//  keeps a small table of nearby aircraft; the ND plots them (RemoteID drawing
//  in instrument_drawer.ino) as orange dots with their altitude in feet.
//
//  Transports decoded here (see https://github.com/opendroneid/specs):
//    * Bluetooth LE legacy advertisement
//        AD structure: [len][0x16 service-data][0xFA 0xFF = UUID 0xFFFA]
//        [0x0D app code = Open Drone ID][msg counter][25-byte ODID message]
//    * WiFi beacon vendor element
//        [0xDD vendor IE][len][OUI FA 0B BC][0x0D][msg counter][message pack]
//  (WiFi NaN and BT5 Long Range / coded-PHY are not decoded yet — most
//   consumer drones also broadcast one of the two transports above.)
//
//  Design goals: FAST (BLE scans continuously; WiFi channel-hops) and
//  UNOBTRUSIVE (parsing happens in the radio callbacks; only a tiny, low-
//  priority task hops the WiFi channel; the table update is a short critical
//  section). The WiFi/BT radio is otherwise unused by the EFIS.
//
//  NOTE: this code targets the ESP32-S3 Arduino core but has not been bench-
//  tested against live drone traffic here — verify on hardware.
// ============================================================================
#include <Arduino.h>
#include "config.h"
#include "State.h"
#include "RemoteID.h"

#if RID_ENABLE

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
// BLE + WiFi RID are now RUNTIME toggles (gRidBle / gRidWifi, NVS-backed via the config
// portal), so both code paths compile unconditionally; the headers below are always pulled in.
// The ESP32-S3 Arduino core ships the NimBLE host (Bluedroid's esp_gap_ble_api is NOT built
// for this chip), so the BLE scanner is implemented on NimBLE. NOTE: this NimBLE build has
// extended advertising disabled (MYNEWT_VAL_BLE_EXT_ADV = 0), so we can only receive *legacy*
// (BT4) advertisements — BT5 Long-Range-only Remote ID is caught over WiFi instead.
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "esp_bt.h"            // esp_bt_controller_* + BT_CONTROLLER_INIT_CONFIG_DEFAULT (lean cfg)

// Runtime RID enables (defaults from config.h RID_USE_*). Live, set via the config portal.
volatile bool gRidBle  = RID_USE_BLE;
volatile bool gRidWifi = RID_USE_WIFI;

// ---- diagnostics -----------------------------------------------------------
// The firmware logs over its own USB-CDC instance (HWCDC USBSerial in
// InstrumentPanel.ino), NOT the default Serial (which is never begin()'d here),
// so RID_LOG must use USBSerial or the lines vanish. flush() forces the line out
// before a blocking/maybe-crashing call (the USB-CDC buffer is otherwise lazy).
#include "HWCDC.h"
extern HWCDC USBSerial;
#if RID_DEBUG
  #define RID_LOG(...)  do { USBSerial.printf(__VA_ARGS__); USBSerial.flush(); } while (0)
#else
  #define RID_LOG(...)  do {} while (0)
#endif
// Cheap free-running counters so a one-line stats print shows which radio is
// actually hearing traffic (set RID_DEBUG 0 to compile them out of the prints).
static volatile uint32_t g_ble_adv = 0;    // BLE advertisements seen (any)
static volatile uint32_t g_ble_hit = 0;    // BLE adverts carrying RID service data
static volatile uint32_t g_wifi_mgmt = 0;  // WiFi mgmt frames seen (any)
static volatile uint32_t g_wifi_hit = 0;   // WiFi frames carrying the RID vendor IE

// ---- target table ----------------------------------------------------------
struct RidEntry {
  bool     used;
  uint8_t  key[6];        // BLE address / WiFi transmitter MAC (per-drone key)
  bool     have_loc;      // has reported a position at least once
  float    lat, lon;
  int      alt_ft;        // height AGL in feet
  uint32_t last_ms;
};
static RidEntry      g_tab[RID_MAX];
static portMUX_TYPE  g_mux = portMUX_INITIALIZER_UNLOCKED;

// Find the entry for this source, or claim a free/oldest slot for a new one.
// Caller holds g_mux.
static RidEntry *rid_lookup(const uint8_t *key) {
  int free_i = -1, old_i = -1; uint32_t old_ms = 0xFFFFFFFFu;
  for (int i = 0; i < RID_MAX; i++) {
    if (g_tab[i].used && memcmp(g_tab[i].key, key, 6) == 0) return &g_tab[i];
    if (!g_tab[i].used) { if (free_i < 0) free_i = i; }
    else if (g_tab[i].last_ms < old_ms) { old_ms = g_tab[i].last_ms; old_i = i; }
  }
  int idx = (free_i >= 0) ? free_i : old_i;
  if (idx < 0) return nullptr;
  RidEntry *e = &g_tab[idx];
  e->used = true; e->have_loc = false; e->alt_ft = 0;
  memcpy(e->key, key, 6);
  return e;
}

// ---- OpenDroneID message parsing -------------------------------------------
// We only need the Location/Vector message (type 1) for the dot. Other message
// types (Basic ID, System, ...) are ignored.
static void odid_parse(RidEntry *e, const uint8_t *m, int len) {
  if (len < 19) return;
  int type = (m[0] >> 4) & 0x0F;
  if (type != 0x01) return;                                  // not Location/Vector
  int32_t lat = (int32_t)((uint32_t)m[5]  | ((uint32_t)m[6]  << 8) | ((uint32_t)m[7]  << 16) | ((uint32_t)m[8]  << 24));
  int32_t lon = (int32_t)((uint32_t)m[9]  | ((uint32_t)m[10] << 8) | ((uint32_t)m[11] << 16) | ((uint32_t)m[12] << 24));
  if (lat == 0 && lon == 0) return;
  uint16_t geo = (uint16_t)(m[15] | (m[16] << 8));
  uint16_t hgt = (uint16_t)(m[17] | (m[18] << 8));           // height above takeoff (AGL)
  float h_m = (hgt != 0) ? (hgt * 0.5f - 1000.0f)            // ODID: alt_m = raw/2 - 1000
                         : (geo * 0.5f - 1000.0f);           // fall back to geodetic if unknown
  e->lat    = lat / 1e7f;
  e->lon    = lon / 1e7f;
  e->alt_ft = (int)lroundf(h_m * 3.28084f);
  e->have_loc = true;
}

// ---- Bluetooth LE ----------------------------------------------------------
// Walk the AD structures of one advertisement (legacy or extended) and pull out
// any OpenDroneID service-data element.
static void ble_parse_adv(const uint8_t *bda, const uint8_t *adv, int len) {
  g_ble_adv++;
  int i = 0;
  while (i + 1 < len) {                       // walk the AD structures
    int adlen = adv[i];
    if (adlen == 0 || i + 1 + adlen > len) break;
    int adtype = adv[i + 1];
    const uint8_t *d = &adv[i + 2];
    int dlen = adlen - 1;
    // Service Data - 16-bit UUID 0xFFFA, app code 0x0D, counter, then message
    if (adtype == 0x16 && dlen >= 4 && d[0] == 0xFA && d[1] == 0xFF && d[2] == 0x0D) {
      g_ble_hit++;
      portENTER_CRITICAL(&g_mux);
      RidEntry *e = rid_lookup(bda);
      if (e) { e->last_ms = millis(); odid_parse(e, d + 4, dlen - 4); }
      portEXIT_CRITICAL(&g_mux);
    }
    i += adlen + 1;
  }
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);  // fwd decl

// True when the WiFi AP is also up (config mode): the radio is SHARED, so the BLE scan
// must NOT run at 100% duty or it starves the AP — phones can't associate / get a DHCP
// lease. Flight mode (no AP) keeps the full-duty scan for the fastest RID detection.
static bool g_ble_share_radio = false;

// Kick off (or restart) a passive legacy scan.
static void ble_start_disc() {
  uint8_t own_addr_type = 0;                     // BLE_OWN_ADDR_PUBLIC
  ble_hs_id_infer_auto(0, &own_addr_type);       // use the chip's public address
  struct ble_gap_disc_params dp = {0};
  dp.itvl             = 0x60;                     // 0.625ms units -> ~60 ms scan interval
  dp.window           = g_ble_share_radio ? 0x18 // ~15 ms listen (25% duty) -> AP gets the radio
                                          : 0x60; // ~60 ms listen (100%) in flight (no AP)
  dp.passive          = 1;                        // passive: just listen, never request
  dp.filter_duplicates = 0;                       // report every advert (position updates)
  dp.filter_policy    = 0;                        // accept all advertisers
  dp.limited          = 0;
  int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, ble_gap_event_cb, nullptr);
  RID_LOG("[RID] BLE: ble_gap_disc rc=%d (share=%d)\n", rc, (int)g_ble_share_radio);
}

// NimBLE GAP event: an advertisement arrived (or the scan ended -> restart).
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      ble_parse_adv(event->disc.addr.val, event->disc.data, event->disc.length_data);
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      ble_start_disc();                            // never give up listening
      break;
    default: break;
  }
  return 0;
}

static void ble_on_sync(void)        { ble_start_disc(); }
static void ble_on_reset(int reason) { RID_LOG("[RID] BLE host reset (reason %d)\n", reason); }

// The NimBLE host runs in its own task; nimble_port_run() returns only on stop.
static void ble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void ble_begin() {
  // Bring the BLE CONTROLLER up ourselves with a LEAN config BEFORE nimble_port_init()
  // (which otherwise inits the controller with fat defaults: ~10 BLE "activities" +
  // big scan-dedup caches ≈ 71 KB internal RAM — too much to also fit the WiFi config
  // AP alongside the internal PFD canvas). We only ever run ONE passive scan, so cap
  // the activities low and shrink the duplicate-filter list. nimble_port_init() then
  // sees the controller already enabled and just brings up the host on top.
  // Lean CONTROLLER first — its event/buffer counts (driven by ble_max_act) are what the
  // NimBLE host's porting-layer mempools size themselves from.
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  bt_cfg.ble_max_act = 2;            // default 10 — each activity reserves per-act buffers
  esp_err_t ce = esp_bt_controller_init(&bt_cfg);
  if (ce != ESP_OK && ce != ESP_ERR_INVALID_STATE) {
    RID_LOG("[RID] bt_controller_init err %d — skipping BLE (low RAM?)\n", ce);
    return;                          // bail cleanly instead of crashing the host downstream
  }
  esp_err_t ee = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ee != ESP_OK && ee != ESP_ERR_INVALID_STATE) {
    RID_LOG("[RID] bt_controller_enable err %d — skipping BLE\n", ee);
    return;
  }

  // Then the NimBLE Porting Layer (funcs table + mempools); nimble_port_init() does this
  // internally, so driving the steps by hand we must call both or esp_nimble_init() null-
  // derefs allocating the default event queue (npl_freertos_eventq_init).
  npl_freertos_funcs_init();
  int mp = npl_freertos_mempool_init();
  ble_npl_eventq_init(nimble_port_get_dflt_eventq());   // nimble_port_init() inits the default
                                                        // event queue before the host; we must too
  RID_LOG("[RID] npl_mempool_init=%d ce=%d ee=%d\n", mp, (int)ce, (int)ee);

  esp_err_t e = esp_nimble_init();               // HOST ONLY (nimble_port_init would re-init the
  if (e != ESP_OK) { RID_LOG("[RID] esp_nimble_init err %d\n", e); return; }  // controller -> INVALID_STATE)
  ble_hs_cfg.sync_cb  = ble_on_sync;             // start scanning once the host is ready
  ble_hs_cfg.reset_cb = ble_on_reset;
  nimble_port_freertos_init(ble_host_task);
  RID_LOG("[RID] BLE: NimBLE legacy (BT4) passive scan (lean controller)\n");
}

// ---- WiFi ------------------------------------------------------------------
static void wifi_parse_beacon(const uint8_t *f, int len) {
  if (len < 38 || f[0] != 0x80) return;       // 0x80 = management / beacon
  const uint8_t *sa = f + 10;                  // Addr2 = transmitter MAC
  int i = 36;                                  // skip 24B header + 12B fixed params
  while (i + 2 <= len) {
    int tag = f[i], taglen = f[i + 1];
    const uint8_t *d = &f[i + 2];
    if (i + 2 + taglen > len) break;
    // vendor IE 0xDD, OUI FA 0B BC (ASD-STAN), type 0x0D, counter, message pack
    if (tag == 0xDD && taglen >= 6 && d[0] == 0xFA && d[1] == 0x0B && d[2] == 0xBC && d[3] == 0x0D) {
      g_wifi_hit++;
      const uint8_t *pk = d + 5; int pklen = taglen - 5;   // d[4]=counter
      if (pklen >= 3 && ((pk[0] >> 4) & 0x0F) == 0x0F) {   // message pack (type 0xF)
        int msz = pk[1], n = pk[2];
        if (msz >= 19) {
          portENTER_CRITICAL(&g_mux);
          RidEntry *e = rid_lookup(sa);
          if (e) {
            e->last_ms = millis();
            for (int k = 0; k < n && 3 + (k + 1) * msz <= pklen; k++)
              odid_parse(e, pk + 3 + k * msz, msz);
          }
          portEXIT_CRITICAL(&g_mux);
        }
      }
    }
    i += 2 + taglen;
  }
}

static void wifi_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  g_wifi_mgmt++;
  auto *pkt = (wifi_promiscuous_pkt_t *)buf;
  int len = (int)pkt->rx_ctrl.sig_len - 4;     // strip 4-byte FCS
  if (len > 0) wifi_parse_beacon(pkt->payload, len);
}

// Turn on promiscuous MGMT capture on whatever WiFi driver is already running.
static void wifi_promisc_on() {
  wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(wifi_promisc_cb);
  esp_wifi_set_promiscuous(true);
}

// FLIGHT mode: stand up the WiFi driver in monitor-only (NULL) mode just for RID, then
// the hop task (rid_task) sweeps the 2.4 GHz channels. No AP here.
static void wifi_begin() {
  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase(); nvs_flash_init();
  }
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  // Monitor-only (promiscuous): we never associate or transmit, so cut the WiFi
  // driver's RX/TX/aggregation buffers hard. (Beacons repeat, so a small RX pool catches RID.)
  wcfg.static_rx_buf_num  = 4;     // default 10  (~1.6 KB each)
  wcfg.dynamic_rx_buf_num = 8;     // default 32
  wcfg.dynamic_tx_buf_num = 4;     // default 32  (we don't transmit)
  wcfg.cache_tx_buf_num   = 0;
  wcfg.ampdu_rx_enable    = 0;     // no aggregation/reorder buffers
  wcfg.ampdu_tx_enable    = 0;
  wcfg.amsdu_tx_enable    = 0;
  wcfg.nvs_enable         = 0;     // no WiFi NVS
  wcfg.rx_ba_win          = 0;     // no block-ack window (ampdu_rx off)
  esp_err_t we = esp_wifi_init(&wcfg);
  if (we != ESP_OK) { RID_LOG("[RID] esp_wifi_init err %d\n", we); return; }
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);           // not a station/AP — monitor only
  esp_wifi_start();
  wifi_promisc_on();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

// CONFIG mode: the WiFi driver is ALREADY up as the softAP (WebConfig.ino). Just add
// promiscuous capture on top — it delivers frames on the AP's channel (no hopping, since
// hopping would drop AP clients), so RID drones on the AP channel still plot while the
// config page is live. This is "WiFi RID + the AP at the same time".
void wifiRidAttach() { wifi_promisc_on(); }

// Cheap task: hop the WiFi channel so we sweep all of 2.4 GHz for drone beacons
// (BLE needs no hopping — its controller scans the advertising channels itself).
static void rid_task(void *) {
  // Remote ID WiFi beacons live on the non-overlapping 2.4 GHz channels (almost
  // always 6, sometimes 1/11). Dwell on those primarily so a ~1 Hz beacon is
  // caught fast; sweep the rest occasionally in case a drone's AP is elsewhere.
  static const uint8_t chans[] = { 6, 1, 11, 6, 1, 11, 6, 3, 9, 6, 4, 8, 6, 2, 13, 6, 5, 10, 6, 7, 12 };
  int ci = 0;
  for (;;) {
    esp_wifi_set_channel(chans[ci], WIFI_SECOND_CHAN_NONE);
    ci = (ci + 1) % (int)(sizeof(chans) / sizeof(chans[0]));
    vTaskDelay(pdMS_TO_TICKS(RID_HOP_MS));
  }
}

// ---- public API ------------------------------------------------------------
// FLIGHT mode (no AP): BLE and/or a standalone channel-hopping WiFi monitor, per the
// runtime toggles. (When gRidWifi is on, the canvas is in PSRAM — see combinedDisplay* —
// so there's internal SRAM for the WiFi driver.)
void remoteid_begin() {
  RID_LOG("[RID] flight receiver (BLE=%d WiFi=%d) internal free=%u\n",
          (int)gRidBle, (int)gRidWifi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  g_ble_share_radio = false;                       // no AP in flight -> full-duty BLE scan
  if (gRidBle) ble_begin();
  if (gRidWifi) {
    wifi_begin();
    xTaskCreatePinnedToCore(rid_task, "ridhop", 2048, nullptr, RID_TASK_PRIO, nullptr, RID_TASK_CORE);
  }
  RID_LOG("[RID] flight receiver up; internal free=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// CONFIG mode (AP up): BLE (now fits — the config-mode canvas is in PSRAM) and/or WiFi RID
// attached to the AP's radio on its channel. Call AFTER webConfigBegin().
void remoteid_begin_ap() {
  RID_LOG("[RID] config receiver (BLE=%d WiFi=%d) internal free=%u\n",
          (int)gRidBle, (int)gRidWifi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  g_ble_share_radio = true;                        // AP is up -> BLE must share the radio (low duty)
  if (gRidWifi) wifiRidAttach();
  if (gRidBle)  ble_begin();
  RID_LOG("[RID] config receiver up; internal free=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void remoteid_fill(state *s) {
  uint32_t now = millis();
  int n = 0, seen = 0;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < RID_MAX; i++) {
    if (!g_tab[i].used) continue;
    if (now - g_tab[i].last_ms > RID_AGE_MS) { g_tab[i].used = false; continue; }
    seen++;                                       // any tracked target -> PROXIMITY
    if (!g_tab[i].have_loc) continue;             // only positioned ones get a dot
    if (n < RID_MAX) {
      s->rid[n].lat    = g_tab[i].lat;
      s->rid[n].lon    = g_tab[i].lon;
      s->rid[n].alt_ft = g_tab[i].alt_ft;
      n++;
    }
  }
  portEXIT_CRITICAL(&g_mux);
  s->n_rid = n;
  s->n_rid_seen = seen;

#if RID_DEBUG
  // One concise line per second so you can see which radio is hearing what:
  //   ble_adv  climbing, ble_hit 0   -> BLE works, but no RID over BT4/BT5 here
  //   ble_adv  stuck at 0            -> BLE controller/coexistence not scanning
  //   wifi_mgmt climbing, wifi_hit 0 -> WiFi monitor works, no RID beacon heard
  //   *_hit > 0                      -> RID frames decoded; tracked/pos should rise
  static uint32_t lastLog = 0;
  if (now - lastLog >= 1000) {
    lastLog = now;
    RID_LOG("[RID] ble_adv=%u ble_hit=%u | wifi_mgmt=%u wifi_hit=%u | tracked=%d pos=%d\n",
            g_ble_adv, g_ble_hit, g_wifi_mgmt, g_wifi_hit, seen, n);
  }
#endif
}

#else  // !RID_ENABLE
volatile bool gRidBle = false, gRidWifi = false;
void remoteid_begin() {}
void remoteid_begin_ap() {}
void remoteid_fill(state *s) { s->n_rid = 0; s->n_rid_seen = 0; }
#endif
