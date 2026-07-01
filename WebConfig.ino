// ============================================================================
//  WebConfig.ino — config-mode WiFi access point + captive-portal settings page.
//
//  CONFIG MODE: NanoPFD brings up a WiFi AP ("NanoPFD"). Join it from a phone and a
//  captive portal pops a TABBED settings page (Attitude / Display / Nav / Air / WiFi):
//  IMU orientation + compass heading offset, the color palette (per-color pickers),
//  map zoom, local pressure, AP password. Changes apply LIVE on the panel and save to
//  flash.
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
#include "esp_coexist.h"        // esp_coex_preference_set — favour WiFi when BLE RID shares the radio
#include "nvs_flash.h"
#include "config.h"       // ORI_*_DEF, BARO_*, CORE_SENSORS, gBaroInHg, gOri* externs
#include "map_zoom.h"     // mapZoomSet / mapZoomIdx / mapZoomCount / mapZoomRangeM

// Runtime IMU orientation (shared by both IMUs; declared extern in config.h). These
// power-on defaults are overwritten by webConfigLoadSettings() from NVS.
volatile uint8_t gOriFlipV = ORI_FLIP_VERTICAL_DEF;
volatile uint8_t gOriFlipR = ORI_FLIP_ROLL_DEF;
volatile uint8_t gOriFlipP = ORI_FLIP_PITCH_DEF;
volatile uint8_t gOriSwap  = ORI_SWAP_ROLL_PITCH_DEF;

extern uint16_t color_index[];   // live RGB565 palette (defined in InstrumentPanel.ino)

// RGB565 <-> "#rrggbb" for the web color pickers (expand/quantize the 5/6/5 channels).
static void rgb565ToHex(uint16_t c, char out[8]) {
  uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
  snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}
static uint16_t hexToRgb565(const String &s) {
  int i = (s.length() && s[0] == '#') ? 1 : 0;
  long v = strtol(s.c_str() + i, nullptr, 16);
  uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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
  gRidBle   = p.getUChar("ridble",  RID_USE_BLE)  ? 1 : 0;   // RID enables (applied at boot)
  gRidWifi  = p.getUChar("ridwifi", RID_USE_WIFI) ? 1 : 0;
  gHeadingOffset = p.getFloat("hdg", HEADING_OFFSET_DEF);
  gPitchTrim = p.getFloat("ptrim", 0);  gRollTrim = p.getFloat("rtrim", 0);   // IMU mount trim
  gAlphaAtt  = p.getFloat("aatt", gAlphaAtt);  gAlphaG   = p.getFloat("ag",   gAlphaG);
  gAlphaAlt  = p.getFloat("aalt", gAlphaAlt);  gAlphaVs  = p.getFloat("avs",  gAlphaVs);
  gAlphaAsi  = p.getFloat("aasi", gAlphaAsi);
  gVsiFs     = p.getFloat("vsifs", gVsiFs);    gGmeterFs = p.getFloat("gmfs", gGmeterFs);
  gUnitAsi   = p.getUChar("uasi", gUnitAsi);   gUnitGs   = p.getUChar("ugs", gUnitGs);   gUnitAlt = p.getUChar("ualt", gUnitAlt);
  gV1        = p.getFloat("v1", gV1);   gVr = p.getFloat("vr", gVr);   gVStall = p.getFloat("vst", gVStall);   gVMax = p.getFloat("vmx", gVMax);
  int zoom  = p.getInt("zoom", -1);
  if (p.getBytesLength("pal") == NUM_COLORS * sizeof(uint16_t)) {   // restore a saved palette
    p.getBytes("pal", (void *)color_index, NUM_COLORS * sizeof(uint16_t));
    gPaletteDirty = true;                                          // AMOLED rebuilds its LUT next frame
  }
  p.end();
  if (zoom >= 0) mapZoomSet(zoom);     // gBaroInHg is restored separately (baroPrefs)
}

// ---- the settings page (dark EFIS theme, matches the display palette) -------
// Tabbed (Attitude / Display / Nav / Air / WiFi). Every control LIVE-applies on change
// for instant preview on the panel (the display runs in config mode); SAVE persists all,
// incl. the AP password (which is save-only and takes effect next config boot).
static const char CFG_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1"><title>NanoPFD</title><style>
:root{--bg:#000;--fg:#e8e8e8;--cy:#22d3ee;--gn:#37d067;--sky:#5aa9e6;--gy:#6b7280;--pn:#0c0f12;--ln:#1b2228}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.45 ui-monospace,Menlo,Consolas,monospace}.w{max-width:460px;margin:0 auto;padding:18px 14px}
h1{font-size:20px;letter-spacing:3px;color:var(--cy);margin:6px 0 0;text-align:center;text-shadow:0 0 12px #0891b2}
.s{text-align:center;color:var(--gy);font-size:11px;letter-spacing:2px;margin:2px 0 14px;text-transform:uppercase}
.tabs{display:flex;flex-wrap:wrap;gap:4px;margin-bottom:14px}
.tb{flex:1 1 56px;background:#11161b;color:var(--gy);border:1px solid var(--ln);border-radius:7px;
padding:9px 2px;font:inherit;font-size:10px;letter-spacing:1px;cursor:pointer;text-transform:uppercase}
.tb.on{background:var(--cy);color:#001014;border-color:var(--cy);font-weight:700}
.pane.hide{display:none}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.mc{background:#11161b;border:1px solid var(--ln);border-radius:7px;padding:8px 10px}
.ml{font-size:10px;color:var(--gy);letter-spacing:1px;text-transform:uppercase}
.mv{font-size:20px;color:var(--cy);margin-top:2px}
.mt{display:flex;gap:4px;margin-bottom:8px}
.mb{flex:1;background:#11161b;color:var(--gy);border:1px solid var(--ln);border-radius:6px;padding:7px 2px;font:inherit;font-size:10px;cursor:pointer}
.mb.on{background:var(--sky);color:#001014}
canvas#plot{width:100%;height:190px;background:#06090c;border:1px solid var(--ln);border-radius:7px;display:block;touch-action:none}
.pc{display:flex;align-items:center;gap:6px;margin-top:8px}
.pb{background:#11161b;color:var(--fg);border:1px solid var(--ln);border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}
a.save{display:block;text-decoration:none;text-align:center;margin-bottom:8px}.save.sec{background:transparent;color:var(--fg);border:1px solid var(--ln)}
.c{background:var(--pn);border:1px solid var(--ln);border-radius:9px;padding:13px 14px;margin-bottom:13px}
.c h2{font-size:11px;letter-spacing:2px;color:var(--gn);text-transform:uppercase;margin:0 0 6px;
border-bottom:1px solid var(--ln);padding-bottom:7px}
.r{display:flex;align-items:center;justify-content:space-between;padding:7px 0}
input,select{background:#11161b;color:var(--fg);border:1px solid #2a333c;border-radius:6px;padding:8px;font:inherit}
input[type=number]{width:104px;text-align:right}select{min-width:140px}input[type=text]{width:160px}
input[type=color]{width:48px;height:30px;padding:2px;cursor:pointer}
.tg{position:relative;width:48px;height:26px;flex:0 0 auto}.tg input{opacity:0;width:0;height:0}
.sl{position:absolute;inset:0;background:#2a333c;border-radius:26px;transition:.15s;cursor:pointer}
.sl:after{content:"";position:absolute;height:20px;width:20px;left:3px;top:3px;background:#9aa6b2;border-radius:50%;transition:.15s}
.tg input:checked+.sl{background:var(--sky)}.tg input:checked+.sl:after{transform:translateX(22px);background:#fff}
.save{width:100%;background:var(--cy);color:#001014;border:0;border-radius:9px;padding:13px;font:inherit;
font-weight:700;letter-spacing:2px;cursor:pointer;margin-top:2px}.save:active{filter:brightness(.85)}
.st{text-align:center;color:var(--gn);min-height:20px;margin-top:10px;letter-spacing:1px}
.u{color:var(--gy);font-size:11px;margin-left:6px}
.tb.pri{flex:1 1 0;font-size:13px;padding:12px 4px}
.dd{position:relative;flex:0 0 auto}
.ddm{position:absolute;right:0;top:112%;background:#11161b;border:1px solid var(--ln);border-radius:7px;padding:4px;z-index:9;display:none;min-width:132px}
.ddm.show{display:block}
.ddi{display:block;width:100%;text-align:left;background:none;border:0;color:var(--fg);padding:9px 10px;font:inherit;font-size:12px;cursor:pointer;border-radius:5px}
.ddi:active{background:#1b2228}
.plt{width:100%;height:96px;background:#06090c;border:1px solid var(--ln);border-radius:7px;display:block;margin-bottom:7px;touch-action:none}
.plabel{font-size:10px;color:var(--gy);letter-spacing:1px;text-transform:uppercase;margin:2px 0}
.wp{display:flex;gap:6px;align-items:center;padding:5px 0;border-bottom:1px solid var(--ln)}
.wp input{padding:6px}.wp .nm{flex:1 1 60px;min-width:0}.wp .ll{width:82px;text-align:right}
.wp .del{background:#2a1015;color:#fd6a6a;border:1px solid #52222a;border-radius:6px;padding:6px 9px;cursor:pointer;font:inherit}
canvas#pmap{width:100%;height:280px;background:#06090c;border:1px solid var(--ln);border-radius:7px;display:block;touch-action:none}
.hint{color:var(--gy);font-size:11px;margin:6px 0}
</style></head><body><div class=w>
<h1>NANO&middot;PFD</h1><div class=s>configuration</div>
<div class=tabs>
<button class="tb pri on" data-t=log>&#9635; Log</button>
<button class="tb pri" data-t=plan>&#9873; Flight Plan</button>
<div class=dd><button class=tb id=setBtn>&#9881;</button>
<div class=ddm id=setMenu>
<button class=ddi data-t=att>Attitude</button><button class=ddi data-t=disp>Display</button>
<button class=ddi data-t=nav>Nav</button><button class=ddi data-t=air>Air</button>
<button class=ddi data-t=tune>Tune</button><button class=ddi data-t=net>WiFi</button></div></div></div>
<div class="pane hide" id=att>
<div class=c><h2>IMU orientation</h2>
<div class=r><label>Upside down</label><label class=tg><input type=checkbox id=v><span class=sl></span></label></div>
<div class=r><label>Reverse roll</label><label class=tg><input type=checkbox id=r><span class=sl></span></label></div>
<div class=r><label>Reverse pitch</label><label class=tg><input type=checkbox id=p><span class=sl></span></label></div>
<div class=r><label>Swap roll/pitch</label><label class=tg><input type=checkbox id=sw><span class=sl></span></label></div></div>
<div class=c><h2>Compass</h2>
<div class=r><label>Heading offset</label><span><input type=number id=hdg min=0 max=359 step=1><span class=u>deg</span></span></div></div>
<div class=c><h2>Mount trim</h2>
<div class=r><label>Pitch trim</label><span><input type=number id=pt step=0.5><span class=u>deg</span></span></div>
<div class=r><label>Roll trim</label><span><input type=number id=rt step=0.5><span class=u>deg</span></span></div>
<button class="save sec" onclick=setLevel()>SET LEVEL (CAPTURE)</button>
<div class=u>Mount the IMU at any angle; with the aircraft level, tap Set level to zero the horizon. Combine with the orientation flips above for 90&deg; mountings.</div></div></div>
<div class="pane hide" id=disp>
<div class=c><h2>Color palette</h2><div id=pal></div></div></div>
<div class="pane hide" id=nav>
<div class=c><h2>Navigation</h2>
<div class=r><label>Map zoom</label><select id=z></select></div></div></div>
<div class="pane hide" id=air>
<div class=c><h2>Air data</h2>
<div class=r><label>Local pressure</label><span><input type=number id=b step=0.01 min=28 max=31><span class=u>inHg</span></span></div></div>
<div class=c><h2>Units</h2>
<div class=r><label>Airspeed</label><select id=uasi><option value=0>knots</option><option value=1>mph</option><option value=2>km/h</option></select></div>
<div class=r><label>Ground speed</label><select id=ugs><option value=0>knots</option><option value=1>mph</option><option value=2>km/h</option></select></div>
<div class=r><label>Altitude</label><select id=ualt><option value=0>feet</option><option value=1>meters</option></select></div></div>
<div class=c><h2>V-speeds <span class=u>in airspeed units &middot; 0 = off</span></h2>
<div class=r><label>V1</label><input type=number id=v1 min=0 max=2000 step=1></div>
<div class=r><label>Rotation (V<sub>R</sub>)</label><input type=number id=vr min=0 max=2000 step=1></div>
<div class=r><label>Stall &mdash; red below</label><input type=number id=vst min=0 max=2000 step=1></div>
<div class=r><label>Max &mdash; red above</label><input type=number id=vmx min=0 max=2000 step=1></div>
<div class=u>Red warning blocks + amber caution bands appear on the airspeed tape; V1/V<sub>R</sub> show as speed bugs.</div></div></div>
<div class="pane hide" id=tune>
<div class=c><h2>Smoothing</h2>
<div class=r><label>Attitude</label><input type=number id=aatt step=0.02 min=0.02 max=1></div>
<div class=r><label>G-force</label><input type=number id=ag step=0.02 min=0.02 max=1></div>
<div class=r><label>Altitude</label><input type=number id=aalt step=0.02 min=0.02 max=1></div>
<div class=r><label>Vertical speed</label><input type=number id=avs step=0.02 min=0.02 max=1></div>
<div class=r><label>Airspeed</label><input type=number id=aasi step=0.02 min=0.02 max=1></div>
<div class=u>0&ndash;1; smaller = smoother but slower to respond.</div></div>
<div class=c><h2>Instrument scales</h2>
<div class=r><label>VSI full-scale</label><span><input type=number id=vsifs step=1 min=1><span class=u>ft/s</span></span></div>
<div class=r><label>G-meter full-scale</label><span><input type=number id=gmfs step=0.5 min=1><span class=u>g</span></span></div></div></div>
<div class=pane id=log>
<div class=c><h2>Session peaks</h2>
<div class=grid>
<div class=mc><div class=ml>Top GPS</div><div class=mv id=mxgs>&ndash;</div></div>
<div class=mc><div class=ml>Top airspeed</div><div class=mv id=mxasi>&ndash;</div></div>
<div class=mc><div class=ml>Max altitude</div><div class=mv id=mxalt>&ndash;</div></div>
<div class=mc><div class=ml>Max g</div><div class=mv id=mxg>&ndash;</div></div></div></div>
<div class=c><h2>History <span class=u id=logdur></span></h2>
<div id=plots>
<div class=plabel>GPS speed &middot; kt</div><canvas class=plt data-m=0></canvas>
<div class=plabel>Airspeed &middot; mph</div><canvas class=plt data-m=1></canvas>
<div class=plabel>Altitude &middot; ft</div><canvas class=plt data-m=2></canvas>
<div class=plabel>Load factor &middot; g</div><canvas class=plt data-m=3></canvas></div>
<div class=pc><button class=pb id=zout>&minus;</button><button class=pb id=zin>+</button>
<button class=pb id=zall>fit</button><span class=u id=plbl></span></div>
<div class=hint>Drag to pan &middot; pinch or &minus;/+ to zoom the time axis</div></div>
<div class=c><h2>Data</h2>
<a class=save id=dl href="/flog.csv">DOWNLOAD CSV</a>
<button class="save sec" onclick=resetLog()>RESET LOG</button></div></div>
<div class="pane hide" id=plan>
<div class=c><h2>Flight plan <span class=u id=wpn></span></h2>
<canvas id=pmap></canvas>
<div class=hint>Tap the map to add a waypoint &middot; drag a marker to move it &middot; pinch/scroll to zoom</div>
<div id=wplist></div>
<div class=pc style="margin-top:8px"><button class=pb id=wpadd>+ Waypoint</button>
<button class=pb id=wpctr>Center on GPS</button><button class=pb id=wpclr>Clear</button></div></div>
<button class=save onclick=planSave()>SAVE FLIGHT PLAN</button>
<div class=st id=planst></div></div>
<div class="pane hide" id=net>
<div class=c><h2>Remote ID receiver</h2>
<div class=r><label>Bluetooth LE</label><label class=tg><input type=checkbox id=rb><span class=sl></span></label></div>
<div class=r><label>WiFi</label><label class=tg><input type=checkbox id=rw><span class=sl></span></label></div>
<div class=u>Plots nearby drones as orange dots on the compass. Both radios run continuously alongside the AP and the display &mdash; WiFi RID listens on the AP's channel (6, where Remote&nbsp;ID beacons live), BLE scans at low duty. Toggles apply live.</div></div>
<div class=c><h2>WiFi AP</h2>
<div class=r><label>AP password</label><input type=text id=pw maxlength=63></div>
<div class=u>8&ndash;63 chars for WPA2; shorter = open network</div></div>
<div class=c><h2>Reboot</h2>
<div class=u style="margin:0 0 10px">This unit runs <b>one always-on mode</b>: the glass display, the WiFi AP, and both Remote&nbsp;ID receivers, all the time. Reboot to re-read flash or recover.</div>
<button class=save onclick=exitCfg()>REBOOT</button></div></div>
<button class=save id=savebtn onclick=save() style=display:none>SAVE TO FLASH</button><div class=st id=st></div>
</div><script>
var $=function(i){return document.getElementById(i)};
var PAL=['Background','Blue','Red / warning','Green','Cyan','Magenta','Yellow','White / text','Sky','Ground','Grey','Roads','Traffic','Water','Roads (med)'];
function ap(q){return fetch('/api?'+q,{method:'POST'})}
function flash(t){$('st').textContent=t;setTimeout(function(){$('st').textContent=''},2000)}
function showPane(t){var ps=document.querySelectorAll('.pane');for(var j=0;j<ps.length;j++)ps[j].classList.add('hide');
$(t).classList.remove('hide');
var pr=document.querySelectorAll('.tb.pri');for(var j=0;j<pr.length;j++)pr[j].classList.toggle('on',pr[j].dataset.t==t);
var cfg=['att','disp','nav','air','tune','net'].indexOf(t)>=0;
$('setBtn').classList.toggle('on',cfg);$('savebtn').style.display=cfg?'block':'none';
$('setMenu').classList.remove('show');
if(t=='log')loadLog();if(t=='plan')planShow()}
var alltb=document.querySelectorAll('[data-t]');for(var k=0;k<alltb.length;k++)alltb[k].onclick=function(){showPane(this.dataset.t)};
$('setBtn').onclick=function(e){e.stopPropagation();$('setMenu').classList.toggle('show')};
document.addEventListener('click',function(){$('setMenu').classList.remove('show')});
function bind(){
['v','r','p','sw'].forEach(function(id,n){var key=['oriV','oriR','oriP','oriS'][n];
$(id).onchange=function(){ap(key+'='+(this.checked?1:0))}});
$('hdg').onchange=function(){ap('hdg='+(this.value||0))};
$('z').onchange=function(){ap('zoom='+this.value)};
$('b').onchange=function(){ap('baro='+this.value)};
$('pt').onchange=function(){ap('ptrim='+this.value)};$('rt').onchange=function(){ap('rtrim='+this.value)};
['aatt','ag','aalt','avs','aasi','vsifs','gmfs','uasi','ugs','ualt','v1','vr','vst','vmx'].forEach(function(id){$(id).onchange=function(){ap(id+'='+this.value)}})}
function setLevel(){ap('level=1').then(function(){flash('✓ captured');setTimeout(load,500)})}
function load(){fetch('/api').then(function(r){return r.json()}).then(function(d){
$('v').checked=!!d.oriV;$('r').checked=!!d.oriR;$('p').checked=!!d.oriP;$('sw').checked=!!d.oriS;
$('hdg').value=d.hdg;$('b').value=d.baro.toFixed(2);$('pw').value=d.pass;
$('rb').checked=!!d.ridble;$('rw').checked=!!d.ridwifi;
$('pt').value=d.ptrim;$('rt').value=d.rtrim;$('aatt').value=d.aatt;$('ag').value=d.ag;
$('aalt').value=d.aalt;$('avs').value=d.avs;$('aasi').value=d.aasi;$('vsifs').value=d.vsifs;$('gmfs').value=d.gmfs;
$('uasi').value=d.uasi;$('ugs').value=d.ugs;$('ualt').value=d.ualt;$('v1').value=d.v1;$('vr').value=d.vr;$('vst').value=d.vst;$('vmx').value=d.vmx;
var z=$('z');z.innerHTML='';d.zooms.forEach(function(m,i){var o=document.createElement('option');
o.value=i;o.text=m;if(i==d.zoom)o.selected=true;z.add(o)});
var pe=$('pal');pe.innerHTML='';d.pal.forEach(function(hex,i){
var row=document.createElement('div');row.className='r';
var lb=document.createElement('label');lb.textContent=PAL[i]||('Color '+i);
var ci=document.createElement('input');ci.type='color';ci.value=hex;ci.id='pal'+i;
ci.onchange=function(){ap('pal'+i+'='+encodeURIComponent(this.value))};
row.appendChild(lb);row.appendChild(ci);pe.appendChild(row)});
bind()})}
function save(){var q='oriV='+($('v').checked?1:0)+'&oriR='+($('r').checked?1:0)+'&oriP='+($('p').checked?1:0)
+'&oriS='+($('sw').checked?1:0)+'&zoom='+$('z').value+'&baro='+$('b').value+'&hdg='+($('hdg').value||0)
+'&ridble='+($('rb').checked?1:0)+'&ridwifi='+($('rw').checked?1:0)
+'&ptrim='+$('pt').value+'&rtrim='+$('rt').value+'&aatt='+$('aatt').value+'&ag='+$('ag').value+'&aalt='+$('aalt').value
+'&avs='+$('avs').value+'&aasi='+$('aasi').value+'&vsifs='+$('vsifs').value+'&gmfs='+$('gmfs').value
+'&uasi='+$('uasi').value+'&ugs='+$('ugs').value+'&ualt='+$('ualt').value+'&v1='+$('v1').value+'&vr='+$('vr').value+'&vst='+$('vst').value+'&vmx='+$('vmx').value
+'&pass='+encodeURIComponent($('pw').value);
for(var i=0;i<PAL.length;i++){var e=$('pal'+i);if(e)q+='&pal'+i+'='+encodeURIComponent(e.value)}
ap(q).then(function(r){flash(r.ok?'✓ saved':'error')})}
var LOG=null,met=0,v0=0,v1=0;
function fmtT(s){var m=Math.floor(s/60),x=Math.round(s%60);return m+':'+(x<10?'0':'')+x}
function z2(x){return(x<10?'0':'')+x}
function fmtClock(e){var d=new Date(e*1000);return z2(d.getUTCHours())+':'+z2(d.getUTCMinutes())+':'+z2(d.getUTCSeconds())}
function useUTC(){return LOG&&LOG.tsrc==2&&LOG.tend>0}            // GPS time acquired?
function epAt(k){return LOG.tend-LOG.secs*(LOG.n-1-k)/(LOG.n>1?LOG.n-1:1)}  // UTC sec at point k
function loadLog(){fetch('/flog').then(function(r){return r.json()}).then(function(d){LOG=d;
$('mxgs').textContent=d.maxgs.toFixed(0)+' kt';$('mxasi').textContent=d.maxasi.toFixed(0)+' mph';
$('mxalt').textContent=d.maxalt+' ft';$('mxg').textContent=d.maxg.toFixed(2)+' g';
$('logdur').textContent='last '+fmtT(d.secs)+(useUTC()?' · ended '+fmtClock(d.tend)+'Z':' · no GPS time');
v0=0;v1=d.n;drawPlot()})}
var COL=['#22d3ee','#37d067','#5aa9e6','#fd6800'];
function clampV(){if(v1<=v0)v1=v0+1;if(v0<0){v1-=v0;v0=0}if(v1>LOG.n){v0-=(v1-LOG.n);v1=LOG.n;if(v0<0)v0=0}}
function drawOne(cv,a,col,dc){var w=cv.clientWidth,h=cv.clientHeight;cv.width=w;cv.height=h;
var x=cv.getContext('2d');x.clearRect(0,0,w,h);
var i0=Math.max(0,Math.floor(v0)),i1=Math.min(LOG.n,Math.ceil(v1));if(i1<=i0)return;
var mn=1e9,mx=-1e9,i;for(i=i0;i<i1;i++){if(a[i]<mn)mn=a[i];if(a[i]>mx)mx=a[i]}
if(mx<=mn)mx=mn+1;var pd=(mx-mn)*0.12;mn-=pd;mx+=pd;
x.strokeStyle='#141a1f';for(var gy=0;gy<=3;gy++){var yy=h-1-gy/3*(h-2);x.beginPath();x.moveTo(0,yy);x.lineTo(w,yy);x.stroke()}
x.strokeStyle=col;x.lineWidth=1.5;x.beginPath();var n=i1-i0;
for(i=0;i<n;i++){var v=a[i0+i],px=n>1?i/(n-1)*w:0,py=h-1-(v-mn)/(mx-mn)*(h-2);i?x.lineTo(px,py):x.moveTo(px,py)}x.stroke();
x.fillStyle='#6b7280';x.font='10px monospace';x.textAlign='left';
x.fillText(mx.toFixed(dc),3,11);x.fillText(mn.toFixed(dc),3,h-4)}
function drawPlot(){if(!LOG||!LOG.n)return;clampV();
var a=[LOG.gs,LOG.asi,LOG.alt,LOG.g],dcs=[0,0,0,2],pl=document.querySelectorAll('.plt');
for(var m=0;m<pl.length;m++)drawOne(pl[m],a[m],COL[m],dcs[m]);
var i0=Math.max(0,Math.floor(v0)),i1=Math.min(LOG.n,Math.ceil(v1)),U=useUTC();
function tlab(k){return U?fmtClock(epAt(k)):fmtT(k/LOG.n*LOG.secs)}
$('plbl').textContent=(U?tlab(i0)+' – '+tlab(i1-1)+' UTC':fmtT(i0/LOG.n*LOG.secs)+'-'+fmtT(i1/LOG.n*LOG.secs))}
$('zin').onclick=function(){var c=(v0+v1)/2,r=(v1-v0)/4;v0=c-r;v1=c+r;drawPlot()};
$('zout').onclick=function(){var c=(v0+v1)/2,r=v1-v0;v0=c-r;v1=c+r;drawPlot()};
$('zall').onclick=function(){v0=0;v1=LOG.n;drawPlot()};
(function(){var P={},panX=0,lastD=0,panning=false;
function ax(cv){
cv.addEventListener('pointerdown',function(e){P[e.pointerId]=e.clientX;cv.setPointerCapture(e.pointerId);
var ids=Object.keys(P);if(ids.length==1){panning=true;panX=e.clientX}else if(ids.length==2){panning=false;lastD=Math.abs(P[ids[0]]-P[ids[1]])||1}});
cv.addEventListener('pointermove',function(e){if(P[e.pointerId]===undefined||!LOG)return;P[e.pointerId]=e.clientX;
var ids=Object.keys(P),rc=cv.getBoundingClientRect();
if(ids.length>=2){var d=Math.abs(P[ids[0]]-P[ids[1]])||1,mid=(P[ids[0]]+P[ids[1]])/2,span=v1-v0;
var idxc=v0+(mid-rc.left)/rc.width*span,ns=span*lastD/d,frac=(mid-rc.left)/rc.width;
v0=idxc-frac*ns;v1=v0+ns;lastD=d;drawPlot()}
else if(panning){var dd=(e.clientX-panX)/rc.width*(v1-v0);v0-=dd;v1-=dd;panX=e.clientX;drawPlot()}});
function up(e){delete P[e.pointerId];var ids=Object.keys(P);panning=ids.length==1;if(panning)panX=P[ids[0]]}
cv.addEventListener('pointerup',up);cv.addEventListener('pointercancel',up);
cv.addEventListener('wheel',function(e){e.preventDefault();if(!LOG)return;var rc=cv.getBoundingClientRect(),span=v1-v0;
var idxc=v0+(e.clientX-rc.left)/rc.width*span,f=e.deltaY<0?.85:1.18,ns=span*f,frac=(e.clientX-rc.left)/rc.width;
v0=idxc-frac*ns;v1=v0+ns;drawPlot()},{passive:false})}
var pl=document.querySelectorAll('.plt');for(var i=0;i<pl.length;i++)ax(pl[i])})();
function resetLog(){fetch('/flog/reset',{method:'POST'}).then(loadLog)}
function exitCfg(){$('st').textContent='Rebooting…';ap('exit=1')}
// ---- flight plan editor ----
var WP=[],MC={lat:39.1,lon:-84.5,scl:600},MG={lat:0,lon:0,ok:0},ploaded=false,BASE=null;
function coslat(){return Math.cos(MC.lat*Math.PI/180)}
function pxOf(la,lo,rc){return{x:rc.width/2+(lo-MC.lon)*coslat()*MC.scl,y:rc.height/2-(la-MC.lat)*MC.scl}}
function llOf(px,py,rc){return{lat:MC.lat-(py-rc.height/2)/MC.scl,lon:MC.lon+(px-rc.width/2)/(coslat()*MC.scl)}}
function planShow(){if(!ploaded)planLoad();else drawMap()}
function planLoad(){fetch('/plan').then(function(r){return r.ok?r.json():{}}).then(function(d){ploaded=true;
WP=(d.wp||[]).map(function(w){return{n:w.n||'',lat:+w.lat,lon:+w.lon}});
if(d.gps){MG={lat:+d.gps.lat,lon:+d.gps.lon,ok:d.gps.ok?1:0}}
if(d.def){MC.lat=+d.def.lat;MC.lon=+d.def.lon}
if(WP.length){var la=0,lo=0;WP.forEach(function(w){la+=w.lat;lo+=w.lon});MC.lat=la/WP.length;MC.lon=lo/WP.length}
else if(MG.ok){MC.lat=MG.lat;MC.lon=MG.lon}
wpList();if(!BASE)fetch('/basemap').then(function(r){return r.ok?r.json():null}).then(function(b){BASE=b||{p:[]};drawMap()}).catch(function(){BASE={p:[]};drawMap()});else drawMap()
}).catch(function(){ploaded=true;wpList();drawMap()})}
function drawBase(x,rc){if(!BASE||!BASE.p)return;x.strokeStyle='#243039';x.lineWidth=1;
BASE.p.forEach(function(ln){x.beginPath();for(var i=0;i<ln.length;i+=2){var p=pxOf(ln[i],ln[i+1],rc);i?x.lineTo(p.x,p.y):x.moveTo(p.x,p.y)}x.stroke()});
if(BASE.a){x.fillStyle='#39505f';x.font='9px monospace';BASE.a.forEach(function(ap){var p=pxOf(ap.lat,ap.lon,rc);
if(p.x<-20||p.x>rc.width+20||p.y<-20||p.y>rc.height+20)return;x.beginPath();x.arc(p.x,p.y,2,0,7);x.fill();if(MC.scl>500&&ap.n)x.fillText(ap.n,p.x+4,p.y+3)})}}
function drawMap(){var cv=$('pmap');if(!cv)return;var w=cv.clientWidth,h=cv.clientHeight;cv.width=w;cv.height=h;
var x=cv.getContext('2d'),rc={width:w,height:h};x.clearRect(0,0,w,h);
var step=MC.scl>2500?0.1:MC.scl>800?0.25:MC.scl>300?1:5;
x.strokeStyle='#141a1f';x.lineWidth=1;x.fillStyle='#3a444d';x.font='9px monospace';x.textAlign='left';
var tl=llOf(0,0,rc),br=llOf(w,h,rc);
for(var lo=Math.ceil(tl.lon/step)*step;lo<br.lon;lo+=step){var p=pxOf(MC.lat,lo,rc);x.beginPath();x.moveTo(p.x,0);x.lineTo(p.x,h);x.stroke();x.fillText(lo.toFixed(2),p.x+2,h-3)}
for(var la=Math.floor(tl.lat/step)*step;la>br.lat;la-=step){var p=pxOf(la,MC.lon,rc);x.beginPath();x.moveTo(0,p.y);x.lineTo(w,p.y);x.stroke();x.fillText(la.toFixed(2),2,p.y-2)}
drawBase(x,rc);
if(MG.ok){var g=pxOf(MG.lat,MG.lon,rc);x.fillStyle='#37d067';x.beginPath();x.arc(g.x,g.y,4,0,7);x.fill();x.fillText('GPS',g.x+6,g.y+3)}
x.strokeStyle='#fd6800';x.lineWidth=2;x.beginPath();
WP.forEach(function(wp,i){var p=pxOf(wp.lat,wp.lon,rc);i?x.lineTo(p.x,p.y):x.moveTo(p.x,p.y)});x.stroke();
WP.forEach(function(wp,i){var p=pxOf(wp.lat,wp.lon,rc);x.fillStyle='#fd6800';x.fillRect(p.x-4,p.y-4,8,8);
x.fillStyle='#ffd9a6';x.font='11px monospace';x.fillText(wp.n||('WP'+(i+1)),p.x+7,p.y+4)})}
function wpUpdRow(i){var rows=$('wplist').children;if(rows[i]){rows[i].children[1].value=WP[i].lat.toFixed(4);rows[i].children[2].value=WP[i].lon.toFixed(4)}}
function wpList(){var e=$('wplist');e.innerHTML='';$('wpn').textContent=WP.length+' waypoint'+(WP.length==1?'':'s');
WP.forEach(function(wp,i){var row=document.createElement('div');row.className='wp';
var nm=document.createElement('input');nm.className='nm';nm.value=wp.n;nm.placeholder='WP'+(i+1);nm.maxLength=8;
nm.oninput=function(){wp.n=this.value;drawMap()};
var la=document.createElement('input');la.className='ll';la.type='number';la.step=0.0001;la.value=wp.lat.toFixed(4);la.onchange=function(){wp.lat=+this.value;drawMap()};
var lo=document.createElement('input');lo.className='ll';lo.type='number';lo.step=0.0001;lo.value=wp.lon.toFixed(4);lo.onchange=function(){wp.lon=+this.value;drawMap()};
var dl=document.createElement('button');dl.className='del';dl.textContent='✕';dl.onclick=function(){WP.splice(i,1);wpList();drawMap()};
row.appendChild(nm);row.appendChild(la);row.appendChild(lo);row.appendChild(dl);e.appendChild(row)})}
$('wpadd').onclick=function(){WP.push({n:'',lat:MC.lat,lon:MC.lon});wpList();drawMap()};
$('wpctr').onclick=function(){if(MG.ok){MC.lat=MG.lat;MC.lon=MG.lon;drawMap()}};
$('wpclr').onclick=function(){WP=[];wpList();drawMap()};
function planSave(){var body=WP.map(function(p){return(p.n||'').replace(/[\t\n]/g,' ').slice(0,8)+'\t'+p.lat.toFixed(6)+'\t'+p.lon.toFixed(6)}).join('\n');
fetch('/plan',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'wp='+encodeURIComponent(body)})
.then(function(r){return r.text()}).then(function(t){$('planst').textContent='✓ saved ('+t+')';setTimeout(function(){$('planst').textContent=''},2500)})
.catch(function(){$('planst').textContent='error'})}
(function(){var cv=$('pmap'),P={},drag=-1,panL=null,lastD=0,moved=false,downXY=null;
function dist2(){var ids=Object.keys(P);return Math.hypot(P[ids[0]].x-P[ids[1]].x,P[ids[0]].y-P[ids[1]].y)||1}
function hit(px,py,rc){for(var i=WP.length-1;i>=0;i--){var p=pxOf(WP[i].lat,WP[i].lon,rc);if(Math.abs(px-p.x)<11&&Math.abs(py-p.y)<11)return i}return -1}
cv.addEventListener('pointerdown',function(e){cv.setPointerCapture(e.pointerId);P[e.pointerId]={x:e.clientX,y:e.clientY};
var ids=Object.keys(P),rc=cv.getBoundingClientRect();
if(ids.length>=2){drag=-1;panL=null;moved=true;lastD=dist2()}else{moved=false;downXY={x:e.clientX-rc.left,y:e.clientY-rc.top};drag=hit(downXY.x,downXY.y,rc);if(drag<0)panL={x:e.clientX,y:e.clientY}}});
cv.addEventListener('pointermove',function(e){if(!P[e.pointerId])return;P[e.pointerId]={x:e.clientX,y:e.clientY};
var ids=Object.keys(P),rc=cv.getBoundingClientRect();
if(ids.length>=2){var d=dist2(),mx=(P[ids[0]].x+P[ids[1]].x)/2-rc.left,my=(P[ids[0]].y+P[ids[1]].y)/2-rc.top;
var b=llOf(mx,my,rc);MC.scl*=d/lastD;lastD=d;var af=llOf(mx,my,rc);MC.lat+=b.lat-af.lat;MC.lon+=b.lon-af.lon;drawMap();return}
moved=true;var lx=e.clientX-rc.left,ly=e.clientY-rc.top;
if(drag>=0){var ll=llOf(lx,ly,rc);WP[drag].lat=ll.lat;WP[drag].lon=ll.lon;drawMap();wpUpdRow(drag)}
else if(panL){MC.lon-=(e.clientX-panL.x)/(coslat()*MC.scl);MC.lat+=(e.clientY-panL.y)/MC.scl;panL={x:e.clientX,y:e.clientY};drawMap()}});
function up(e){var wd=drag,mv=moved;delete P[e.pointerId];var ids=Object.keys(P);
if(ids.length==0){if(!mv&&wd<0&&downXY){var rc=cv.getBoundingClientRect(),ll=llOf(downXY.x,downXY.y,rc);WP.push({n:'',lat:ll.lat,lon:ll.lon});wpList();drawMap()}
else if(mv&&wd>=0)wpList();drag=-1;panL=null;downXY=null}else if(ids.length==1)lastD=0}
cv.addEventListener('pointerup',up);cv.addEventListener('pointercancel',up);
cv.addEventListener('wheel',function(e){e.preventDefault();var rc=cv.getBoundingClientRect(),lx=e.clientX-rc.left,ly=e.clientY-rc.top;
var b=llOf(lx,ly,rc);MC.scl*=e.deltaY<0?1.15:0.87;var af=llOf(lx,ly,rc);MC.lat+=b.lat-af.lat;MC.lon+=b.lon-af.lon;drawMap()},{passive:false})})();
load();showPane('log');
</script></body></html>)HTML";

static void cfgHandleRoot() { cfgServer.send_P(200, "text/html", CFG_PAGE); }

static void cfgHandleApiGet() {
  String j = "{";
  j += "\"oriV\":" + String(gOriFlipV) + ",\"oriR\":" + String(gOriFlipR) +
       ",\"oriP\":" + String(gOriFlipP) + ",\"oriS\":" + String(gOriSwap) + ",";
  j += "\"zoom\":" + String(mapZoomIdx()) + ",";
  j += "\"baro\":" + String(gBaroInHg, 2) + ",";
  j += "\"hdg\":" + String((int)lroundf(gHeadingOffset)) + ",";
  j += "\"ridble\":" + String((int)gRidBle) + ",\"ridwifi\":" + String((int)gRidWifi) + ",";
  j += "\"ptrim\":" + String(gPitchTrim, 1) + ",\"rtrim\":" + String(gRollTrim, 1) + ",";
  j += "\"aatt\":" + String(gAlphaAtt, 3) + ",\"ag\":" + String(gAlphaG, 3) + ",\"aalt\":" + String(gAlphaAlt, 3) +
       ",\"avs\":" + String(gAlphaVs, 3) + ",\"aasi\":" + String(gAlphaAsi, 3) + ",";
  j += "\"vsifs\":" + String(gVsiFs, 1) + ",\"gmfs\":" + String(gGmeterFs, 1) + ",";
  j += "\"uasi\":" + String((int)gUnitAsi) + ",\"ugs\":" + String((int)gUnitGs) + ",\"ualt\":" + String((int)gUnitAlt) + ",";
  j += "\"v1\":" + String(gV1, 0) + ",\"vr\":" + String(gVr, 0) + ",\"vst\":" + String(gVStall, 0) + ",\"vmx\":" + String(gVMax, 0) + ",";
  j += "\"pal\":[";
  for (int i = 0; i < NUM_COLORS; i++) {
    char hx[8]; rgb565ToHex(color_index[i], hx);
    j += "\""; j += hx; j += "\"";
    if (i + 1 < NUM_COLORS) j += ",";
  }
  j += "],";
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
  if (cfgServer.hasArg("exit")) {            // "Reboot" button -> save log + restart (back into always-on)
#if !ALWAYS_ON_MODE
    webConfigSetFlightMode();                // legacy: exit to flight (no AP)
#endif
    cfgServer.send(200, "text/plain", "ok");
    gPendingReboot = true;                   // loop() saves the log + reboots
    return;
  }
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
  if (cfgServer.hasArg("hdg")) {
    float h = cfgServer.arg("hdg").toFloat();
    while (h < 0)       h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    gHeadingOffset = h;
  }
  if (cfgServer.hasArg("ridble"))  gRidBle  = cfgServer.arg("ridble").toInt() ? 1 : 0;   // applied next boot
  if (cfgServer.hasArg("ridwifi")) gRidWifi = cfgServer.arg("ridwifi").toInt() ? 1 : 0;
  if (cfgServer.hasArg("level"))   gLevelCapture = true;                                 // capture mount level
  if (cfgServer.hasArg("ptrim"))   gPitchTrim = cfgServer.arg("ptrim").toFloat();
  if (cfgServer.hasArg("rtrim"))   gRollTrim  = cfgServer.arg("rtrim").toFloat();
  if (cfgServer.hasArg("aatt")) gAlphaAtt = constrain(cfgServer.arg("aatt").toFloat(), 0.01f, 1.0f);
  if (cfgServer.hasArg("ag"))   gAlphaG   = constrain(cfgServer.arg("ag").toFloat(),   0.01f, 1.0f);
  if (cfgServer.hasArg("aalt")) gAlphaAlt = constrain(cfgServer.arg("aalt").toFloat(), 0.01f, 1.0f);
  if (cfgServer.hasArg("avs"))  gAlphaVs  = constrain(cfgServer.arg("avs").toFloat(),  0.01f, 1.0f);
  if (cfgServer.hasArg("aasi")) gAlphaAsi = constrain(cfgServer.arg("aasi").toFloat(), 0.01f, 1.0f);
  if (cfgServer.hasArg("vsifs")) gVsiFs   = constrain(cfgServer.arg("vsifs").toFloat(), 1.0f, 200.0f);
  if (cfgServer.hasArg("gmfs"))  gGmeterFs= constrain(cfgServer.arg("gmfs").toFloat(),  1.0f, 20.0f);
  if (cfgServer.hasArg("uasi")) gUnitAsi = constrain(cfgServer.arg("uasi").toInt(), 0, 2);
  if (cfgServer.hasArg("ugs"))  gUnitGs  = constrain(cfgServer.arg("ugs").toInt(),  0, 2);
  if (cfgServer.hasArg("ualt")) gUnitAlt = constrain(cfgServer.arg("ualt").toInt(), 0, 1);
  if (cfgServer.hasArg("v1"))  gV1     = constrain(cfgServer.arg("v1").toFloat(),  0.0f, 2000.0f);
  if (cfgServer.hasArg("vr"))  gVr     = constrain(cfgServer.arg("vr").toFloat(),  0.0f, 2000.0f);
  if (cfgServer.hasArg("vst")) gVStall = constrain(cfgServer.arg("vst").toFloat(), 0.0f, 2000.0f);
  if (cfgServer.hasArg("vmx")) gVMax   = constrain(cfgServer.arg("vmx").toFloat(), 0.0f, 2000.0f);
  bool palChanged = false;
  for (int i = 0; i < NUM_COLORS; i++) {
    char key[8]; snprintf(key, sizeof key, "pal%d", i);
    if (cfgServer.hasArg(key)) { color_index[i] = hexToRgb565(cfgServer.arg(key)); palChanged = true; }
  }
  if (palChanged) gPaletteDirty = true;   // AMOLED rebuilds its RGB332 LUT next frame

  Preferences p; p.begin("cfg", false);
  p.putUChar("oriV", gOriFlipV); p.putUChar("oriR", gOriFlipR);
  p.putUChar("oriP", gOriFlipP); p.putUChar("oriS", gOriSwap);
  p.putInt("zoom", mapZoomIdx());
  p.putFloat("hdg", gHeadingOffset);
  p.putUChar("ridble", gRidBle); p.putUChar("ridwifi", gRidWifi);
  p.putFloat("ptrim", gPitchTrim); p.putFloat("rtrim", gRollTrim);
  p.putFloat("aatt", gAlphaAtt); p.putFloat("ag", gAlphaG); p.putFloat("aalt", gAlphaAlt);
  p.putFloat("avs", gAlphaVs); p.putFloat("aasi", gAlphaAsi);
  p.putFloat("vsifs", gVsiFs); p.putFloat("gmfs", gGmeterFs);
  p.putUChar("uasi", gUnitAsi); p.putUChar("ugs", gUnitGs); p.putUChar("ualt", gUnitAlt);
  p.putFloat("v1", gV1); p.putFloat("vr", gVr); p.putFloat("vst", gVStall); p.putFloat("vmx", gVMax);
  if (palChanged) p.putBytes("pal", (const void *)color_index, NUM_COLORS * sizeof(uint16_t));
  if (cfgServer.hasArg("pass")) p.putString("appass", cfgServer.arg("pass"));   // applies next config boot
  p.end();
  cfgServer.send(200, "text/plain", "ok");
}

// ---- flight log endpoints (FlightLog.ino) ----------------------------------
// /flog      -> JSON: session peaks + a downsampled series (<=720 pts/metric) for the plot
// /flog.csv  -> full 10 Hz log as a CSV download (streamed)
// /flog/reset-> clear the log + peaks
extern volatile float gFlogMaxGs, gFlogMaxAsi, gFlogMaxAlt, gFlogMaxG;
extern uint32_t flightLogLastEpochSec();   // epoch (UTC s) of the newest sample; 0 if never stamped
extern uint8_t  flightLogTimeSrc();        // 0=none 1=system(relative) 2=GPS UTC

// Stream one downsampled metric array as a chunked JSON fragment: "key":[v,v,...]
// which: 0=gps-speed 1=airspeed 2=altitude 3=g. Bucket = the peak over `step` samples.
// Kept to a ~1.6KB chunk so the whole /flog response peaks at ~2KB heap (was ~34KB when it
// built four arrays + a combined String — that OOM'd at low free memory and boot-looped).
static void flogStreamArray(const char *key, uint32_t cnt, int step, int which) {
  String chunk; chunk.reserve(1700);
  chunk = "\""; chunk += key; chunk += "\":[";
  bool first = true;
  for (uint32_t b = 0; b < cnt; b += step) {
    float m = (which == 2) ? -1e9f : 0.0f;                  // altitude can be negative
    uint32_t end = b + step; if (end > cnt) end = cnt;
    for (uint32_t k = b; k < end; k++) {
      float gs, asi, alt, g; flightLogGet(k, &gs, &asi, &alt, &g);
      float v = which == 0 ? gs : which == 1 ? asi : which == 2 ? alt : g;
      if (v > m) m = v;
    }
    if (!first) chunk += ",";
    first = false;
    if      (which == 2) chunk += String((int)m);
    else if (which == 3) chunk += String(m, 2);
    else                 chunk += String(m, 1);
    if (chunk.length() > 1500) { cfgServer.sendContent(chunk); chunk = ""; }
  }
  chunk += "]";
  cfgServer.sendContent(chunk);
}

static void cfgHandleFlog() {
  uint32_t cnt = flightLogCount();
  const int target = 720;
  int step = (cnt > (uint32_t)target) ? (int)((cnt + target - 1) / target) : 1;
  int outN = step ? (int)((cnt + step - 1) / step) : 0;     // ceil(cnt/step) = bucket count
  cfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);        // chunked
  cfgServer.send(200, "application/json", "");
  String head = "{\"hz\":10,\"count\":" + String(cnt) + ",\"secs\":" + String(cnt / 10) +
                ",\"tend\":" + String(flightLogLastEpochSec()) + ",\"tsrc\":" + String((int)flightLogTimeSrc()) + ",";
  head += "\"maxgs\":" + String(gFlogMaxGs, 1) + ",\"maxasi\":" + String(gFlogMaxAsi, 1) +
          ",\"maxalt\":" + String((int)gFlogMaxAlt) + ",\"maxg\":" + String(gFlogMaxG, 2) + ",";
  head += "\"n\":" + String(outN) + ",";
  cfgServer.sendContent(head);
  flogStreamArray("gs",  cnt, step, 0); cfgServer.sendContent(",");
  flogStreamArray("asi", cnt, step, 1); cfgServer.sendContent(",");
  flogStreamArray("alt", cnt, step, 2); cfgServer.sendContent(",");
  flogStreamArray("g",   cnt, step, 3);
  cfgServer.sendContent("}");
  cfgServer.sendContent("");                                // terminate the chunked response
}

static void cfgHandleFlogCsv() {
  cfgServer.sendHeader("Content-Disposition", "attachment; filename=flightlog.csv");
  cfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  cfgServer.send(200, "text/csv", "");
  cfgServer.sendContent("t_s,utc,gps_kt,asi_mph,alt_ft,g\n");
  uint32_t cnt = flightLogCount();
  uint32_t tend = flightLogLastEpochSec();      // UTC epoch (s) of the NEWEST sample
  bool utcOk = (flightLogTimeSrc() == 2) && tend;
  String chunk; chunk.reserve(2048);
  for (uint32_t j = 0; j < cnt; j++) {
    float gs, asi, alt, g; flightLogGet(j, &gs, &asi, &alt, &g);
    chunk += String(j / 10.0f, 1); chunk += ',';
    if (utcOk) {                                // actual UTC clock time of this sample (10 Hz back-count)
      long tenths = (long)tend * 10 - (long)(cnt - 1 - j);
      time_t sec = (time_t)(tenths / 10); int f = (int)(tenths % 10);
      struct tm tmv; gmtime_r(&sec, &tmv);
      char ts[24]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tmv);
      chunk += ts; chunk += '.'; chunk += String(f); chunk += 'Z';
    }
    chunk += ','; chunk += String(gs, 1); chunk += ',';
    chunk += String(asi, 1); chunk += ','; chunk += String((int)alt); chunk += ','; chunk += String(g, 2);
    chunk += '\n';
    if (chunk.length() > 1800) { cfgServer.sendContent(chunk); chunk = ""; }
  }
  if (chunk.length()) cfgServer.sendContent(chunk);
  cfgServer.sendContent("");          // terminate the chunked response
}

static void cfgHandleFlogReset() { flightLogReset(); cfgServer.send(200, "text/plain", "ok"); }

// ---- Flight plan -----------------------------------------------------------
extern int      fplanCount();
extern bool     fplanGet(int, float *, float *, char *);
extern int      fplanSetFromText(const String &);
static void jsonEsc(String &j, const char *s) { for (; s && *s; s++) { if (*s == '"' || *s == '\\') j += '\\'; if ((uint8_t)*s >= 32) j += *s; } }

// GET /plan -> {def, gps, wp:[{n,lat,lon}]}: the saved waypoints + map-center hints for the editor.
static void cfgHandlePlanGet() {
  state s; getState(&s);
  float glat = s.has_pos ? s.last_lat : (float)MAP_DEFAULT_LAT;
  float glon = s.has_pos ? s.last_lon : (float)MAP_DEFAULT_LON;
  String j = "{\"def\":{\"lat\":" + String((float)MAP_DEFAULT_LAT, 5) + ",\"lon\":" + String((float)MAP_DEFAULT_LON, 5) + "},";
  j += "\"gps\":{\"ok\":" + String(s.GPS ? 1 : 0) + ",\"lat\":" + String(glat, 6) + ",\"lon\":" + String(glon, 6) + "},\"wp\":[";
  int n = fplanCount();
  for (int i = 0; i < n; i++) {
    float la, lo; char nm[10]; fplanGet(i, &la, &lo, nm);
    if (i) j += ",";
    j += "{\"n\":\""; jsonEsc(j, nm);
    j += "\",\"lat\":" + String(la, 6) + ",\"lon\":" + String(lo, 6) + "}";
  }
  j += "]}";
  cfgServer.send(200, "application/json", j);
}

// POST /plan  form-urlencoded: wp=<url-encoded "name\tlat\tlon\n" per line> -> replace + persist.
// (Form args are parsed reliably by the WebServer; a raw text/plain body via arg("plain") is not.)
static void cfgHandlePlanPost() {
  String body = cfgServer.hasArg("wp") ? cfgServer.arg("wp") : cfgServer.arg("plain");
  int n = fplanSetFromText(body);
  cfgServer.send(200, "text/plain", String(n) + " waypoints");
}

// GET /basemap -> {p:[[la,lo,...],...], a:[{lat,lon,n}]}: a coarse LOD basemap for the editor
// (L1 polylines for coast/borders orientation + L2 major airports as named reference points).
static void cfgHandleBasemap() {
  cfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  cfgServer.send(200, "application/json", "");
  String chunk; chunk.reserve(1900);
  chunk = "{\"p\":[";
  const ChartPoly *POLYS = LOD_POLYS[1]; int np = LOD_NPOLYS[1];
  for (int i = 0; i < np; i++) {
    const ChartPoly &P = POLYS[i];
    if (i) chunk += ",";
    chunk += "[";
    for (int k = 0; k < P.n; k++) { if (k) chunk += ","; chunk += String(P.pts[2 * k], 4); chunk += ","; chunk += String(P.pts[2 * k + 1], 4); }
    chunk += "]";
    if (chunk.length() > 1500) { cfgServer.sendContent(chunk); chunk = ""; }
  }
  chunk += "],\"a\":[";
  const ChartPt *PTS = LOD_PTS[2]; int na = LOD_NPTS[2]; bool first = true;
  for (int i = 0; i < na; i++) {
    const ChartPt &pt = PTS[i];
    if (pt.type == PT_CITY) continue;
    if (!first) chunk += ","; first = false;
    chunk += "{\"lat\":" + String(pt.lat, 4) + ",\"lon\":" + String(pt.lon, 4) + ",\"n\":\"";
    jsonEsc(chunk, pt.id); chunk += "\"}";
    if (chunk.length() > 1500) { cfgServer.sendContent(chunk); chunk = ""; }
  }
  chunk += "]}";
  cfgServer.sendContent(chunk);
  cfgServer.sendContent("");
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
#if ALWAYS_ON_MODE
  return true;                             // one always-on mode: AP is always up (ignores NVS)
#else
  Preferences p; p.begin("cfg", true);
  bool m = p.getBool("apmode", false);     // default: flight (8-bit, no AP); BOOT-hold to config
  p.end();
  return m;
#endif
}
void webConfigToggleApMode() {
  Preferences p; p.begin("cfg", false);
  bool m = p.getBool("apmode", true);
  p.putBool("apmode", !m);
  p.end();
}
// Force flight mode (used by the page's "Exit to flight mode" button — definitive, not a toggle).
void webConfigSetFlightMode() {
  Preferences p; p.begin("cfg", false);
  p.putBool("apmode", false);
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
  ap.ap.channel         = 6;            // ch 6: where Remote-ID WiFi beacons almost always live, so
                                        // WiFi RID (which rides the AP's channel, no hop) actually
                                        // catches drones. The phone scans all channels to find us.
  ap.ap.max_connection  = 2;           // allow a reconnect slot (config mode has the RAM now)
  ap.ap.beacon_interval = 100;         // default cadence -> easier discovery + association
                                        // (we no longer starve coex: BLE RID runs at low duty)
  if (wpa2) {
    strncpy((char *)ap.ap.password, pass.c_str(), sizeof(ap.ap.password) - 1);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    ap.ap.authmode = WIFI_AUTH_OPEN;   // the default <8-char pass -> open network
  }
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_err_t se = esp_wifi_start();
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);   // BLE RID shares this radio — let the AP win
  esp_wifi_set_ps(WIFI_PS_NONE);        // AP: never modem-sleep
  esp_wifi_set_max_tx_power(44);        // ~11 dBm (0.25 dBm units); plenty for cockpit range

  cfgDns.start(53, "*", IPAddress(192, 168, 4, 1));   // all DNS -> us (captive)
  cfgServer.on("/", cfgHandleRoot);
  cfgServer.on("/api", HTTP_GET,  cfgHandleApiGet);
  cfgServer.on("/api", HTTP_POST, cfgHandleApiSet);
  cfgServer.on("/flog", HTTP_GET, cfgHandleFlog);
  cfgServer.on("/flog.csv", HTTP_GET, cfgHandleFlogCsv);
  cfgServer.on("/flog/reset", HTTP_POST, cfgHandleFlogReset);
  cfgServer.on("/plan", HTTP_GET,  cfgHandlePlanGet);
  cfgServer.on("/plan", HTTP_POST, cfgHandlePlanPost);
  cfgServer.on("/basemap", HTTP_GET, cfgHandleBasemap);
  cfgServer.onNotFound(cfgHandleNotFound);
  cfgServer.begin();
  USBSerial.printf("WiFi AP '%s' (%s) init=%d start=%d internal_free=%u at http://%s/\n",
                   AP_SSID, wpa2 ? "WPA2" : "OPEN", (int)ie, (int)se,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), AP_IP_STR);
  xTaskCreatePinnedToCore(cfgTask, "cfgweb", 4096, NULL, 1, NULL, CORE_SENSORS);
}
