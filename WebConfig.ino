// ============================================================================
//  WebConfig.ino — config-mode WiFi access point + captive-portal settings page.
//
//  CONFIG MODE: NanoPFD brings up a WiFi AP ("NanoPFD"). Join it from a phone and a
//  captive portal pops the settings page automatically; changes (IMU orientation,
//  map zoom, local pressure, AP password) save to flash and apply live on the
//  display.
//
//  Why it's a MODE, not always-on: WiFi and the Remote ID BLE scanner can't run at
//  once on this board. Measured on hardware: after the PFD canvas the chip has ~84 KB
//  of internal SRAM, and the BLE controller+host eat ~71 KB of it, leaving ~13 KB —
//  far too little for the WiFi driver (even trimmed). So the two are mutually
//  exclusive, chosen at boot by an NVS flag ("cfg"/"apmode", default false = flight):
//    flight mode  -> Remote ID (BLE), no AP
//    config mode  -> WiFi AP (no BLE), so WiFi has the full ~84 KB to work with
//  Toggle the flag + reboot by HOLDING the BOOT button ~3 s (InstrumentPanel.ino).
//
//  The page is one self-contained HTML string in flash (no filesystem); the JSON
//  API is hand-built (no JSON library) to keep flash + RAM tiny.
// ============================================================================
#include <WiFi.h>          // IPAddress + the WebServer/DNSServer Arduino glue
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "esp_wifi.h"      // raw WiFi driver — lean buffer config (Arduino softAP is too heavy)
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi_default.h"   // esp_netif_create_default_wifi_ap()
#include "nvs_flash.h"
#include "config.h"       // ORI_*_DEF, BARO_*, CORE_SENSORS, gBaroInHg, gOri* externs
#include "map_zoom.h"     // mapZoomSet / mapZoomIdx / mapZoomCount / mapZoomRangeM

// Runtime IMU orientation (shared by both IMUs; declared extern in config.h). These
// power-on defaults are overwritten by webConfigLoadSettings() from NVS.
volatile uint8_t gOriFlipV = ORI_FLIP_VERTICAL_DEF;
volatile uint8_t gOriFlipR = ORI_FLIP_ROLL_DEF;
volatile uint8_t gOriFlipP = ORI_FLIP_PITCH_DEF;
volatile uint8_t gOriSwap  = ORI_SWAP_ROLL_PITCH_DEF;

#define AP_SSID         "NanoPFD"
#define AP_PASS_DEFAULT "NanoPFD"     // < 8 chars -> WPA2 minimum not met -> open AP (see below)

static WebServer  cfgServer(80);
static DNSServer  cfgDns;

static String cfgGetPass() {
  Preferences p; p.begin("cfg", true);
  String s = p.getString("appass", AP_PASS_DEFAULT);
  p.end();
  return s;
}

// Load all persisted settings into the live globals. Called at EVERY boot (flight or
// config), so the saved orientation / zoom apply in normal use too.
void webConfigLoadSettings() {
  Preferences p; p.begin("cfg", true);
  gOriFlipV = p.getUChar("oriV", ORI_FLIP_VERTICAL_DEF) ? 1 : 0;
  gOriFlipR = p.getUChar("oriR", ORI_FLIP_ROLL_DEF) ? 1 : 0;
  gOriFlipP = p.getUChar("oriP", ORI_FLIP_PITCH_DEF) ? 1 : 0;
  gOriSwap  = p.getUChar("oriS", ORI_SWAP_ROLL_PITCH_DEF) ? 1 : 0;
  int zoom  = p.getInt("zoom", -1);
  p.end();
  if (zoom >= 0) mapZoomSet(zoom);     // gBaroInHg is restored separately (baroPrefs)
}

// ---- the settings page (dark EFIS theme, matches the display palette) -------
static const char CFG_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1"><title>NanoPFD</title><style>
:root{--bg:#000;--fg:#e8e8e8;--cy:#22d3ee;--gn:#37d067;--sky:#5aa9e6;--gy:#6b7280;--pn:#0c0f12;--ln:#1b2228}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.45 ui-monospace,Menlo,Consolas,monospace}.w{max-width:460px;margin:0 auto;padding:18px 14px}
h1{font-size:20px;letter-spacing:3px;color:var(--cy);margin:6px 0 0;text-align:center;text-shadow:0 0 12px #0891b2}
.s{text-align:center;color:var(--gy);font-size:11px;letter-spacing:2px;margin:2px 0 18px;text-transform:uppercase}
.c{background:var(--pn);border:1px solid var(--ln);border-radius:9px;padding:13px 14px;margin-bottom:13px}
.c h2{font-size:11px;letter-spacing:2px;color:var(--gn);text-transform:uppercase;margin:0 0 6px;
border-bottom:1px solid var(--ln);padding-bottom:7px}.r{display:flex;align-items:center;justify-content:space-between;padding:8px 0}
input,select{background:#11161b;color:var(--fg);border:1px solid #2a333c;border-radius:6px;padding:8px;font:inherit}
input[type=number]{width:104px;text-align:right}select{min-width:140px}input[type=text]{width:150px}
.tg{position:relative;width:48px;height:26px;flex:0 0 auto}.tg input{opacity:0;width:0;height:0}
.sl{position:absolute;inset:0;background:#2a333c;border-radius:26px;transition:.15s;cursor:pointer}
.sl:after{content:"";position:absolute;height:20px;width:20px;left:3px;top:3px;background:#9aa6b2;border-radius:50%;transition:.15s}
.tg input:checked+.sl{background:var(--sky)}.tg input:checked+.sl:after{transform:translateX(22px);background:#fff}
button{width:100%;background:var(--cy);color:#001014;border:0;border-radius:9px;padding:13px;font:inherit;
font-weight:700;letter-spacing:2px;cursor:pointer;margin-top:2px}button:active{filter:brightness(.85)}
.st{text-align:center;color:var(--gn);min-height:20px;margin-top:10px;letter-spacing:1px}.u{color:var(--gy);font-size:11px;margin-left:6px}
</style></head><body><div class=w>
<h1>NANO&middot;PFD</h1><div class=s>configuration</div>
<div class=c><h2>IMU orientation</h2>
<div class=r><label>Upside down</label><span class=tg><input type=checkbox id=v><span class=sl></span></span></div>
<div class=r><label>Reverse roll</label><span class=tg><input type=checkbox id=r><span class=sl></span></span></div>
<div class=r><label>Reverse pitch</label><span class=tg><input type=checkbox id=p><span class=sl></span></span></div>
<div class=r><label>Swap roll/pitch</label><span class=tg><input type=checkbox id=sw><span class=sl></span></span></div></div>
<div class=c><h2>Navigation</h2>
<div class=r><label>Map zoom</label><select id=z></select></div></div>
<div class=c><h2>Air data</h2>
<div class=r><label>Local pressure</label><span><input type=number id=b step=0.01 min=28 max=31><span class=u>inHg</span></span></div></div>
<div class=c><h2>WiFi</h2>
<div class=r><label>AP password</label><input type=text id=pw maxlength=63></div>
<div class=u>8&ndash;63 chars for WPA2; shorter = open network</div></div>
<button onclick=save()>SAVE TO FLASH</button><div class=st id=st></div>
</div><script>
var $=function(i){return document.getElementById(i)};
function load(){fetch('/api').then(function(r){return r.json()}).then(function(d){
$('v').checked=!!d.oriV;$('r').checked=!!d.oriR;$('p').checked=!!d.oriP;$('sw').checked=!!d.oriS;
$('b').value=d.baro.toFixed(2);$('pw').value=d.pass;var z=$('z');z.innerHTML='';
d.zooms.forEach(function(m,i){var o=document.createElement('option');o.value=i;o.text=m;
if(i==d.zoom)o.selected=true;z.add(o)});})}
function save(){var q='oriV='+($('v').checked?1:0)+'&oriR='+($('r').checked?1:0)+'&oriP='+($('p').checked?1:0)
+'&oriS='+($('sw').checked?1:0)+'&zoom='+$('z').value+'&baro='+$('b').value+'&pass='+encodeURIComponent($('pw').value);
fetch('/api?'+q,{method:'POST'}).then(function(r){$('st').textContent=r.ok?'✓ saved':'error';
setTimeout(function(){$('st').textContent=''},2500)})}
load();
</script></body></html>)HTML";

static void cfgHandleRoot() { cfgServer.send_P(200, "text/html", CFG_PAGE); }

static void cfgHandleApiGet() {
  String j = "{";
  j += "\"oriV\":" + String(gOriFlipV) + ",\"oriR\":" + String(gOriFlipR) +
       ",\"oriP\":" + String(gOriFlipP) + ",\"oriS\":" + String(gOriSwap) + ",";
  j += "\"zoom\":" + String(mapZoomIdx()) + ",";
  j += "\"baro\":" + String(gBaroInHg, 2) + ",";
  j += "\"pass\":\"" + cfgGetPass() + "\",\"zooms\":[";
  for (int i = 0; i < mapZoomCount(); i++) {
    int r = mapZoomRangeM(i);
    char b[20];
    if (r >= 1852) snprintf(b, sizeof b, "\"%d NM\"", (int)lroundf(r / 1852.0f));
    else           snprintf(b, sizeof b, "\"%d m\"", r);
    j += b; if (i + 1 < mapZoomCount()) j += ",";
  }
  j += "]}";
  cfgServer.send(200, "application/json", j);
}

static void cfgHandleApiSet() {
  if (cfgServer.hasArg("oriV")) gOriFlipV = cfgServer.arg("oriV").toInt() ? 1 : 0;
  if (cfgServer.hasArg("oriR")) gOriFlipR = cfgServer.arg("oriR").toInt() ? 1 : 0;
  if (cfgServer.hasArg("oriP")) gOriFlipP = cfgServer.arg("oriP").toInt() ? 1 : 0;
  if (cfgServer.hasArg("oriS")) gOriSwap  = cfgServer.arg("oriS").toInt() ? 1 : 0;
  if (cfgServer.hasArg("zoom")) mapZoomSet(cfgServer.arg("zoom").toInt());
  if (cfgServer.hasArg("baro")) {
    float b = cfgServer.arg("baro").toFloat();
    if (b >= BARO_MIN_INHG && b <= BARO_MAX_INHG) {
      gBaroInHg = b;
      Preferences bp; bp.begin("baro", false); bp.putFloat("inHg", b); bp.end();
    }
  }
  Preferences p; p.begin("cfg", false);
  p.putUChar("oriV", gOriFlipV); p.putUChar("oriR", gOriFlipR);
  p.putUChar("oriP", gOriFlipP); p.putUChar("oriS", gOriSwap);
  p.putInt("zoom", mapZoomIdx());
  if (cfgServer.hasArg("pass")) p.putString("appass", cfgServer.arg("pass"));   // applies next config boot
  p.end();
  cfgServer.send(200, "text/plain", "ok");
}

// esp_netif's default AP gateway (we bring the AP up via raw esp_wifi, below).
#define AP_IP_STR "192.168.4.1"

// Captive portal: any other URL (the OS connectivity probes) -> redirect to the page
// so the "sign in to network" sheet pops automatically.
static void cfgHandleNotFound() {
  cfgServer.sendHeader("Location", "http://" AP_IP_STR "/", true);
  cfgServer.send(302, "text/plain", "");
}

// Run the portal off the render core, at low priority (below the sensors).
static void cfgTask(void *) {
  for (;;) {
    cfgDns.processNextRequest();
    cfgServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Bring up the AP + captive portal + web server. Called every boot, after the BLE
// controller (so BLE claims internal SRAM first).
//
// We do NOT use Arduino's WiFi.softAP(): it always allocates the core's DEFAULT WiFi
// buffer pool (10 static + 32 dynamic RX, AMPDU reorder buffers, ...), which can't fit
// in the ~80 KB of internal SRAM left after the PFD canvas + BLE controller — esp_wifi_init
// fails with "alloc pm_beacon_offset fail". So bring the AP up by hand with the WiFi
// driver buffers trimmed hard, the same lean approach the Remote ID monitor uses
// (RemoteID.cpp). WiFi's DMA-capable buffers must live in internal SRAM, hence the diet.
// ---- config-vs-flight mode -------------------------------------------------
// The WiFi AP and the Remote ID BLE scanner can't share this board's internal SRAM:
// the PFD canvas (kept in SRAM for fps) leaves only ~85 KB of internal heap, and WiFi
// (~44 KB) + BLE (~68 KB) together overflow it (measured on hardware — WiFi's PHY/DMA
// buffers can't move to PSRAM at runtime). So the two are MUTUALLY EXCLUSIVE, chosen at
// boot from an NVS flag. Default = AP on (so the config page is reachable out of the box);
// hold the BOOT button ~3 s in flight to toggle + reboot (InstrumentPanel.ino).
bool webConfigApMode() {
  Preferences p; p.begin("cfg", true);
  bool m = p.getBool("apmode", true);     // default: AP/config mode ON
  p.end();
  return m;
}
void webConfigToggleApMode() {
  Preferences p; p.begin("cfg", false);
  bool m = p.getBool("apmode", true);
  p.putBool("apmode", !m);
  p.end();
}

void webConfigBegin() {
  String pass = cfgGetPass();
  bool   wpa2 = pass.length() >= 8;

  // netif + event-loop + AP netif/DHCP. In AP mode no BLE is up, so ~85 KB of internal SRAM
  // is free here and these allocate without OOM (this is what aborted when attempted after BLE).
  nvs_flash_init();                     // no-ops (ESP_ERR_INVALID_STATE) if already done — ignore
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_ap();   // AP netif + built-in DHCP server (gateway 192.168.4.1)

  // Still push WiFi's heap buffers to PSRAM where the driver allows it (small extra headroom).
  size_t saved_thresh = 256 * 1024;
  heap_caps_malloc_extmem_enable(4096);

  wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
  wc.static_rx_buf_num  = 4;            // default 10  (~1.6 KB each, DMA -> internal)
  wc.dynamic_rx_buf_num = 8;            // default 32
  wc.dynamic_tx_buf_num = 8;            // the AP transmits (beacons / DHCP / HTTP) — keep a few
  wc.cache_tx_buf_num   = 0;
  wc.ampdu_rx_enable    = 0;            // no aggregation / reorder buffers
  wc.ampdu_tx_enable    = 0;
  wc.amsdu_tx_enable    = 0;
  wc.nvs_enable         = 0;
  wc.rx_ba_win          = 0;
  esp_err_t ie = esp_wifi_init(&wc);
  heap_caps_malloc_extmem_enable(saved_thresh);   // restore: big allocs -> PSRAM, small -> internal

  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_AP);

  wifi_config_t ap = {};
  strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len        = strlen(AP_SSID);
  ap.ap.channel         = 1;            // fixed channel: no scan, smaller footprint
  ap.ap.max_connection  = 1;           // one phone at a time keeps the AP buffers small
  ap.ap.beacon_interval = 300;         // longer beacon = less airtime, eases WiFi/BLE coex
  if (wpa2) {
    strncpy((char *)ap.ap.password, pass.c_str(), sizeof(ap.ap.password) - 1);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    ap.ap.authmode = WIFI_AUTH_OPEN;   // the default <8-char pass -> open network
  }
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_err_t se = esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);        // AP: never modem-sleep
  esp_wifi_set_max_tx_power(44);        // ~11 dBm (0.25 dBm units); plenty for cockpit range

  cfgDns.start(53, "*", IPAddress(192, 168, 4, 1));   // all DNS -> us (captive)
  cfgServer.on("/", cfgHandleRoot);
  cfgServer.on("/api", HTTP_GET,  cfgHandleApiGet);
  cfgServer.on("/api", HTTP_POST, cfgHandleApiSet);
  cfgServer.onNotFound(cfgHandleNotFound);
  cfgServer.begin();
  USBSerial.printf("WiFi AP '%s' (%s) init=%d start=%d internal_free=%u at http://%s/\n",
                   AP_SSID, wpa2 ? "WPA2" : "OPEN", (int)ie, (int)se,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), AP_IP_STR);
  xTaskCreatePinnedToCore(cfgTask, "cfgweb", 4096, NULL, 1, NULL, CORE_SENSORS);
}
