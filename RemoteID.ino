// ============================================================================
//  RemoteID.ino — passive FAA Remote ID (ASTM F3411 / OpenDroneID) receiver.
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
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

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
#if RID_USE_BLE
static void ble_parse_adv(const uint8_t *bda, const uint8_t *adv, int len) {
  int i = 0;
  while (i + 1 < len) {                       // walk the AD structures
    int adlen = adv[i];
    if (adlen == 0 || i + 1 + adlen > len) break;
    int adtype = adv[i + 1];
    const uint8_t *d = &adv[i + 2];
    int dlen = adlen - 1;
    // Service Data - 16-bit UUID 0xFFFA, app code 0x0D, counter, then message
    if (adtype == 0x16 && dlen >= 4 && d[0] == 0xFA && d[1] == 0xFF && d[2] == 0x0D) {
      portENTER_CRITICAL(&g_mux);
      RidEntry *e = rid_lookup(bda);
      if (e) { e->last_ms = millis(); odid_parse(e, d + 4, dlen - 4); }
      portEXIT_CRITICAL(&g_mux);
    }
    i += adlen + 1;
  }
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      esp_ble_gap_start_scanning(0);          // 0 = scan continuously
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto *r = &param->scan_rst;
      if (r->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
        ble_parse_adv(r->bda, r->ble_adv, r->adv_data_len + r->scan_rsp_len);
      else if (r->search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
        esp_ble_gap_start_scanning(0);        // restart if the scan ever ends
      break;
    }
    default: break;
  }
}

static void ble_begin() {
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bt_controller_init(&cfg);
  esp_bt_controller_enable(ESP_BT_MODE_BLE);
  esp_bluedroid_init();
  esp_bluedroid_enable();
  esp_ble_gap_register_callback(ble_gap_cb);
  esp_ble_scan_params_t sp;
  sp.scan_type          = BLE_SCAN_TYPE_PASSIVE;       // passive: just listen
  sp.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
  sp.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
  sp.scan_interval      = 0x50;                         // 0x50 * 0.625ms = 50 ms
  sp.scan_window        = 0x50;                         // 100% duty cycle
  sp.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE;   // report every advert (position updates)
  esp_ble_gap_set_scan_params(&sp);                     // -> PARAM_SET_COMPLETE -> start scanning
}
#endif // RID_USE_BLE

// ---- WiFi ------------------------------------------------------------------
#if RID_USE_WIFI
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
  auto *pkt = (wifi_promiscuous_pkt_t *)buf;
  int len = (int)pkt->rx_ctrl.sig_len - 4;     // strip 4-byte FCS
  if (len > 0) wifi_parse_beacon(pkt->payload, len);
}

static void wifi_begin() {
  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase(); nvs_flash_init();
  }
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&wcfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);           // not a station/AP — monitor only
  esp_wifi_start();
  wifi_promiscuous_filter_t filt; filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(wifi_promisc_cb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

// Cheap task: hop the WiFi channel so we sweep all of 2.4 GHz for drone beacons
// (BLE needs no hopping — its controller scans the advertising channels itself).
static void rid_task(void *) {
  static const uint8_t chans[] = { 1, 6, 11, 3, 9, 2, 7, 12, 4, 8, 13, 5, 10 };
  int ci = 0;
  for (;;) {
    esp_wifi_set_channel(chans[ci], WIFI_SECOND_CHAN_NONE);
    ci = (ci + 1) % (int)(sizeof(chans) / sizeof(chans[0]));
    vTaskDelay(pdMS_TO_TICKS(RID_HOP_MS));
  }
}
#endif // RID_USE_WIFI

// ---- public API ------------------------------------------------------------
void remoteid_begin() {
#if RID_USE_WIFI
  wifi_begin();                                // bring up WiFi first, then BLE (coexistence)
#endif
#if RID_USE_BLE
  ble_begin();
#endif
#if RID_USE_WIFI
  xTaskCreatePinnedToCore(rid_task, "ridhop", 2048, nullptr, RID_TASK_PRIO, nullptr, RID_TASK_CORE);
#endif
}

void remoteid_fill(state *s) {
  uint32_t now = millis();
  int n = 0;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < RID_MAX; i++) {
    if (!g_tab[i].used) continue;
    if (now - g_tab[i].last_ms > RID_AGE_MS) { g_tab[i].used = false; continue; }
    if (!g_tab[i].have_loc) continue;
    if (n < RID_MAX) {
      s->rid[n].lat    = g_tab[i].lat;
      s->rid[n].lon    = g_tab[i].lon;
      s->rid[n].alt_ft = g_tab[i].alt_ft;
      n++;
    }
  }
  portEXIT_CRITICAL(&g_mux);
  s->n_rid = n;
}

#else  // !RID_ENABLE
void remoteid_begin() {}
void remoteid_fill(state *s) { s->n_rid = 0; }
#endif
