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
</style></head><body><div class=w>
<h1>NANO&middot;PFD</h1><div class=s>configuration</div>
<div class=tabs>
<button class="tb on" data-t=att>Attitude</button><button class=tb data-t=disp>Display</button>
<button class=tb data-t=nav>Nav</button><button class=tb data-t=air>Air</button>
<button class=tb data-t=tune>Tune</button><button class=tb data-t=log>Log</button><button class=tb data-t=net>WiFi</button></div>
<div class=pane id=att>
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
<div class=r><label>Local pressure</label><span><input type=number id=b step=0.01 min=28 max=31><span class=u>inHg</span></span></div></div></div>
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
<div class="pane hide" id=log>
<div class=c><h2>Session peaks</h2>
<div class=grid>
<div class=mc><div class=ml>Top GPS</div><div class=mv id=mxgs>&ndash;</div></div>
<div class=mc><div class=ml>Top airspeed</div><div class=mv id=mxasi>&ndash;</div></div>
<div class=mc><div class=ml>Max altitude</div><div class=mv id=mxalt>&ndash;</div></div>
<div class=mc><div class=ml>Max g</div><div class=mv id=mxg>&ndash;</div></div></div></div>
<div class=c><h2>History <span class=u id=logdur></span></h2>
<div class=mt>
<button class="mb on" data-m=0>GPS kt</button><button class=mb data-m=1>ASI mph</button>
<button class=mb data-m=2>Alt ft</button><button class=mb data-m=3>G</button></div>
<canvas id=plot></canvas>
<div class=pc><button class=pb id=zout>&minus;</button><button class=pb id=zin>+</button>
<button class=pb id=zall>fit</button><span class=u id=plbl></span></div></div>
<div class=c><h2>Data</h2>
<a class=save id=dl href="/flog.csv">DOWNLOAD CSV</a>
<button class="save sec" onclick=resetLog()>RESET LOG</button></div></div>
<div class="pane hide" id=net>
<div class=c><h2>Remote ID receiver</h2>
<div class=r><label>Bluetooth LE</label><label class=tg><input type=checkbox id=rb><span class=sl></span></label></div>
<div class=r><label>WiFi</label><label class=tg><input type=checkbox id=rw><span class=sl></span></label></div>
<div class=u>Plots nearby drones as orange dots on the compass. WiFi RID also runs while this AP is on (on the AP's channel); in flight it lowers fps. Changes apply after the next reboot / mode switch.</div></div>
<div class=c><h2>WiFi AP</h2>
<div class=r><label>AP password</label><input type=text id=pw maxlength=63></div>
<div class=u>8&ndash;63 chars for WPA2; shorter = open network</div></div>
<div class=c><h2>Mode</h2>
<div class=u style="margin:0 0 10px">You're in <b>config mode</b> (AP on, display runs slow). Exit to <b>flight mode</b> for full frame rate (Wi-Fi off). To come back, hold the <b>BOOT</b> button ~3&nbsp;s.</div>
<button class=save onclick=exitCfg()>EXIT TO FLIGHT MODE (FULL FPS)</button></div></div>
<button class=save onclick=save()>SAVE TO FLASH</button><div class=st id=st></div>
</div><script>
var $=function(i){return document.getElementById(i)};
var PAL=['Background','Blue','Red / warning','Green','Cyan','Magenta','Yellow','White / text','Sky','Ground','Grey','Roads','Traffic','Water','Roads (med)'];
function ap(q){return fetch('/api?'+q,{method:'POST'})}
function flash(t){$('st').textContent=t;setTimeout(function(){$('st').textContent=''},2000)}
var tb=document.querySelectorAll('.tb');for(var k=0;k<tb.length;k++){tb[k].onclick=function(){
for(var j=0;j<tb.length;j++)tb[j].classList.remove('on');
var ps=document.querySelectorAll('.pane');for(var j=0;j<ps.length;j++)ps[j].classList.add('hide');
this.classList.add('on');$(this.dataset.t).classList.remove('hide');if(this.dataset.t=='log')loadLog()}}
function bind(){
['v','r','p','sw'].forEach(function(id,n){var key=['oriV','oriR','oriP','oriS'][n];
$(id).onchange=function(){ap(key+'='+(this.checked?1:0))}});
$('hdg').onchange=function(){ap('hdg='+(this.value||0))};
$('z').onchange=function(){ap('zoom='+this.value)};
$('b').onchange=function(){ap('baro='+this.value)};
$('pt').onchange=function(){ap('ptrim='+this.value)};$('rt').onchange=function(){ap('rtrim='+this.value)};
['aatt','ag','aalt','avs','aasi','vsifs','gmfs'].forEach(function(id){$(id).onchange=function(){ap(id+'='+this.value)}})}
function setLevel(){ap('level=1').then(function(){flash('✓ captured');setTimeout(load,500)})}
function load(){fetch('/api').then(function(r){return r.json()}).then(function(d){
$('v').checked=!!d.oriV;$('r').checked=!!d.oriR;$('p').checked=!!d.oriP;$('sw').checked=!!d.oriS;
$('hdg').value=d.hdg;$('b').value=d.baro.toFixed(2);$('pw').value=d.pass;
$('rb').checked=!!d.ridble;$('rw').checked=!!d.ridwifi;
$('pt').value=d.ptrim;$('rt').value=d.rtrim;$('aatt').value=d.aatt;$('ag').value=d.ag;
$('aalt').value=d.aalt;$('avs').value=d.avs;$('aasi').value=d.aasi;$('vsifs').value=d.vsifs;$('gmfs').value=d.gmfs;
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
+'&pass='+encodeURIComponent($('pw').value);
for(var i=0;i<PAL.length;i++){var e=$('pal'+i);if(e)q+='&pal'+i+'='+encodeURIComponent(e.value)}
ap(q).then(function(r){flash(r.ok?'✓ saved':'error')})}
var LOG=null,met=0,v0=0,v1=0;
function fmtT(s){var m=Math.floor(s/60),x=Math.round(s%60);return m+':'+(x<10?'0':'')+x}
function loadLog(){fetch('/flog').then(function(r){return r.json()}).then(function(d){LOG=d;
$('mxgs').textContent=d.maxgs.toFixed(0)+' kt';$('mxasi').textContent=d.maxasi.toFixed(0)+' mph';
$('mxalt').textContent=d.maxalt+' ft';$('mxg').textContent=d.maxg.toFixed(2)+' g';
$('logdur').textContent='last '+fmtT(d.secs);v0=0;v1=d.n;drawPlot()})}
function curArr(){return[LOG.gs,LOG.asi,LOG.alt,LOG.g][met]}
function drawPlot(){if(!LOG||!LOG.n)return;var cv=$('plot'),w=cv.clientWidth,h=190;cv.width=w;cv.height=h;
var x=cv.getContext('2d');x.clearRect(0,0,w,h);var a=curArr();
if(v1<=v0){v0=0;v1=LOG.n}var i0=Math.max(0,Math.floor(v0)),i1=Math.min(LOG.n,Math.ceil(v1));if(i1<=i0)return;
var mn=1e9,mx=-1e9,i;for(i=i0;i<i1;i++){if(a[i]<mn)mn=a[i];if(a[i]>mx)mx=a[i]}
if(mx<=mn)mx=mn+1;var pd=(mx-mn)*0.1;mn-=pd;mx+=pd;
x.strokeStyle='#1b2228';for(var gy=0;gy<=4;gy++){var yy=h-1-gy/4*(h-2);x.beginPath();x.moveTo(0,yy);x.lineTo(w,yy);x.stroke()}
x.strokeStyle=['#22d3ee','#37d067','#5aa9e6','#fd6800'][met];x.lineWidth=1.5;x.beginPath();
var n=i1-i0;for(i=0;i<n;i++){var v=a[i0+i],px=n>1?i/(n-1)*w:0,py=h-1-(v-mn)/(mx-mn)*(h-2);
i?x.lineTo(px,py):x.moveTo(px,py)}x.stroke();
x.fillStyle='#6b7280';x.font='10px monospace';var dc=met==3?2:0;
x.fillText(mx.toFixed(dc),3,11);x.fillText(mn.toFixed(dc),3,h-4);
var t0=i0/LOG.n*LOG.secs,t1=i1/LOG.n*LOG.secs;$('plbl').textContent=fmtT(t0)+'-'+fmtT(t1)}
var mb=document.querySelectorAll('.mb');for(var mi=0;mi<mb.length;mi++){mb[mi].onclick=function(){
for(var j=0;j<mb.length;j++)mb[j].classList.remove('on');this.classList.add('on');met=+this.dataset.m;drawPlot()}}
$('zin').onclick=function(){var c=(v0+v1)/2,r=(v1-v0)/4;v0=c-r;v1=c+r;drawPlot()};
$('zout').onclick=function(){var c=(v0+v1)/2,r=v1-v0;v0=c-r;v1=c+r;if(v0<0)v0=0;if(v1>LOG.n)v1=LOG.n;drawPlot()};
$('zall').onclick=function(){v0=0;v1=LOG.n;drawPlot()};
(function(){var cv=$('plot'),dn=false,sx=0,s0=0,s1=0;
cv.addEventListener('pointerdown',function(e){dn=true;sx=e.clientX;s0=v0;s1=v1;cv.setPointerCapture(e.pointerId)});
cv.addEventListener('pointermove',function(e){if(!dn||!LOG)return;var d=(e.clientX-sx)/cv.clientWidth*(s1-s0);
v0=s0-d;v1=s1-d;if(v0<0){v1-=v0;v0=0}if(v1>LOG.n){v0-=v1-LOG.n;v1=LOG.n;if(v0<0)v0=0}drawPlot()});
cv.addEventListener('pointerup',function(){dn=false})})();
function resetLog(){fetch('/flog/reset',{method:'POST'}).then(loadLog)}
function exitCfg(){$('st').textContent='Rebooting to flight mode…';ap('exit=1')}
load();
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
  if (cfgServer.hasArg("exit")) {            // "Exit to flight mode" -> full-fps, no AP, reboot
    webConfigSetFlightMode();
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

static void cfgHandleFlog() {
  uint32_t cnt = flightLogCount();
  const int target = 720;
  int step = (cnt > (uint32_t)target) ? (int)((cnt + target - 1) / target) : 1;
  String ags = "[", aasi = "[", aalt = "[", ag = "[";
  int outN = 0;
  for (uint32_t b = 0; b < cnt; b += step) {
    float mgs = 0, masi = 0, malt = -1e9f, mg = 0;          // bucket peaks (preserve maxima)
    uint32_t end = b + step; if (end > cnt) end = cnt;
    for (uint32_t k = b; k < end; k++) {
      float gs, asi, alt, g; flightLogGet(k, &gs, &asi, &alt, &g);
      if (gs > mgs) mgs = gs; if (asi > masi) masi = asi;
      if (alt > malt) malt = alt; if (g > mg) mg = g;
    }
    if (outN) { ags += ","; aasi += ","; aalt += ","; ag += ","; }
    ags += String(mgs, 1); aasi += String(masi, 1);
    aalt += String((int)malt); ag += String(mg, 2);
    outN++;
  }
  ags += "]"; aasi += "]"; aalt += "]"; ag += "]";
  String j = "{\"hz\":10,\"count\":" + String(cnt) + ",\"secs\":" + String(cnt / 10) + ",";
  j += "\"maxgs\":" + String(gFlogMaxGs, 1) + ",\"maxasi\":" + String(gFlogMaxAsi, 1) +
       ",\"maxalt\":" + String((int)gFlogMaxAlt) + ",\"maxg\":" + String(gFlogMaxG, 2) + ",";
  j += "\"n\":" + String(outN) + ",\"gs\":" + ags + ",\"asi\":" + aasi +
       ",\"alt\":" + aalt + ",\"g\":" + ag + "}";
  cfgServer.send(200, "application/json", j);
}

static void cfgHandleFlogCsv() {
  cfgServer.sendHeader("Content-Disposition", "attachment; filename=flightlog.csv");
  cfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  cfgServer.send(200, "text/csv", "");
  cfgServer.sendContent("t_s,gps_kt,asi_mph,alt_ft,g\n");
  uint32_t cnt = flightLogCount();
  String chunk; chunk.reserve(2048);
  for (uint32_t j = 0; j < cnt; j++) {
    float gs, asi, alt, g; flightLogGet(j, &gs, &asi, &alt, &g);
    chunk += String(j / 10.0f, 1); chunk += ','; chunk += String(gs, 1); chunk += ',';
    chunk += String(asi, 1); chunk += ','; chunk += String((int)alt); chunk += ','; chunk += String(g, 2);
    chunk += '\n';
    if (chunk.length() > 1800) { cfgServer.sendContent(chunk); chunk = ""; }
  }
  if (chunk.length()) cfgServer.sendContent(chunk);
  cfgServer.sendContent("");          // terminate the chunked response
}

static void cfgHandleFlogReset() { flightLogReset(); cfgServer.send(200, "text/plain", "ok"); }

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
  bool m = p.getBool("apmode", false);    // default: FLIGHT mode (full fps). Config/AP is opt-in.
  p.end();
  return m;
}
void webConfigToggleApMode() {
  Preferences p; p.begin("cfg", false);
  bool m = p.getBool("apmode", false);
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
  ap.ap.channel         = 1;            // fixed channel: no scan, smaller footprint
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
  cfgServer.onNotFound(cfgHandleNotFound);
  cfgServer.begin();
  USBSerial.printf("WiFi AP '%s' (%s) init=%d start=%d internal_free=%u at http://%s/\n",
                   AP_SSID, wpa2 ? "WPA2" : "OPEN", (int)ie, (int)se,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), AP_IP_STR);
  xTaskCreatePinnedToCore(cfgTask, "cfgweb", 4096, NULL, 1, NULL, CORE_SENSORS);
}
