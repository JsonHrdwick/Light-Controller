/*
 * Cabinet Lighting Controller
 * ESP32 + Dual-channel LED strip (Warm White + Cool White)
 *
 * Required Libraries (install via Arduino Library Manager or PlatformIO):
 *   - ESP Async WebServer  (me-no-dev/ESPAsyncWebServer)
 *   - AsyncTCP             (me-no-dev/AsyncTCP)
 *   - ArduinoJson          (bblanchon/ArduinoJson) v6.x
 *
 * Hardware:
 *   - GPIO 25 → MOSFET gate for Warm White channel
 *   - GPIO 26 → MOSFET gate for Cool White channel
 *   - GPIO 34 → LDR/Photodiode voltage divider (ADC input only, no pullup)
 *
 * Wiring notes:
 *   - Use N-channel MOSFETs (e.g. IRLZ44N, AO3400) on each PWM pin.
 *     Gate → ESP32 GPIO (via 100Ω resistor), Drain → LED - lead, Source → GND.
 *     LED + leads go to 24V supply. Common - connects through MOSFET drain.
 *   - LDR: one leg to 3.3V, other leg to GPIO34 AND a 10kΩ resistor to GND.
 *     Bright = higher ADC value, Dark = lower value.
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>
#include <algorithm>

// ============================================================
// USER CONFIGURATION  —  edit these before flashing
// ============================================================
const char* WIFI_SSID     = "Camburn";
const char* WIFI_PASSWORD = "MeowMeow";

// Timezone: seconds offset from UTC
//  UTC-5 (US Eastern Standard) = -18000
//  UTC-6 (US Central Standard) = -21600
//  UTC-7 (US Mountain Standard)= -25200
//  UTC-8 (US Pacific Standard) = -28800
const long  GMT_OFFSET_SEC      = -18000;
const int   DAYLIGHT_OFFSET_SEC = 3600;   // 3600 if your region uses DST, else 0
// ============================================================

// --- Pin assignments ---
#define PIN_WARM_WHITE   25
#define PIN_COOL_WHITE   26
#define PIN_LIGHT_SENSOR 34   // ADC1 channel — must be input-only pin

// --- LEDC (PWM) config ---
#define PWM_FREQ        1000   // Hz — above audible whine range for most strips
#define PWM_RESOLUTION  8      // bits → duty range 0–255
#define LEDC_CH_WARM    0
#define LEDC_CH_COOL    1

// ============================================================
//  Data structures
// ============================================================
struct LightState {
  bool    on         = false;
  uint8_t brightness = 80;  // 1–100 %
  uint8_t colorTemp  = 30;  // 0 = full warm, 100 = full cool
};

struct Schedule {
  String  id;
  String  time;       // "HH:MM" 24-hour
  uint8_t brightness;
  uint8_t colorTemp;
  bool    enabled;
  bool    days[7];    // index 0 = Sunday … 6 = Saturday
};

struct SensorConfig {
  bool    enabled   = false;
  int     threshold = 500;   // ADC 0–4095; reading BELOW this → "dark"
  String  action    = "dim"; // "dim" | "off" | "on"
  uint8_t dimLevel  = 20;    // brightness % used when action == "dim"
};

// ============================================================
//  Globals
// ============================================================
AsyncWebServer server(80);

LightState   currentState;
SensorConfig sensorCfg;
std::vector<Schedule> schedules;

bool       sensorTriggered   = false;
LightState preTriggerState;

unsigned long lastScheduleCheck = 0;  // millis
unsigned long lastSensorCheck   = 0;
int           lastMinuteChecked = -1; // prevents multiple firings per minute

// ============================================================
//  PWM helpers
// ============================================================
void applyState(const LightState& s) {
  if (!s.on) {
    ledcWrite(LEDC_CH_WARM, 0);
    ledcWrite(LEDC_CH_COOL, 0);
    return;
  }
  float bri = constrain(s.brightness, 1, 100) / 100.0f;
  float ct  = constrain(s.colorTemp,  0, 100) / 100.0f;
  ledcWrite(LEDC_CH_WARM, (uint8_t)(255.0f * bri * (1.0f - ct)));
  ledcWrite(LEDC_CH_COOL, (uint8_t)(255.0f * bri * ct));
}

// ============================================================
//  SPIFFS persistence
// ============================================================
void saveState() {
  File f = SPIFFS.open("/state.json", "w");
  if (!f) return;
  StaticJsonDocument<128> doc;
  doc["on"]         = currentState.on;
  doc["brightness"] = currentState.brightness;
  doc["colorTemp"]  = currentState.colorTemp;
  serializeJson(doc, f);
  f.close();
}

void loadState() {
  if (!SPIFFS.exists("/state.json")) return;
  File f = SPIFFS.open("/state.json", "r");
  if (!f) return;
  StaticJsonDocument<128> doc;
  if (!deserializeJson(doc, f)) {
    currentState.on         = doc["on"]         | false;
    currentState.brightness = doc["brightness"] | 80;
    currentState.colorTemp  = doc["colorTemp"]  | 30;
  }
  f.close();
}

void saveSchedules() {
  File f = SPIFFS.open("/schedules.json", "w");
  if (!f) return;
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& s : schedules) {
    JsonObject obj = arr.createNestedObject();
    obj["id"]         = s.id;
    obj["time"]       = s.time;
    obj["brightness"] = s.brightness;
    obj["colorTemp"]  = s.colorTemp;
    obj["enabled"]    = s.enabled;
    JsonArray d = obj.createNestedArray("days");
    for (int i = 0; i < 7; i++) d.add(s.days[i]);
  }
  serializeJson(doc, f);
  f.close();
}

void loadSchedules() {
  if (!SPIFFS.exists("/schedules.json")) return;
  File f = SPIFFS.open("/schedules.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, f)) { f.close(); return; }
  schedules.clear();
  for (JsonObject obj : doc.as<JsonArray>()) {
    Schedule s;
    s.id         = obj["id"].as<String>();
    s.time       = obj["time"].as<String>();
    s.brightness = obj["brightness"] | 80;
    s.colorTemp  = obj["colorTemp"]  | 30;
    s.enabled    = obj["enabled"]    | true;
    JsonArray d  = obj["days"].as<JsonArray>();
    for (int i = 0; i < 7 && i < (int)d.size(); i++) s.days[i] = (bool)d[i];
    schedules.push_back(s);
  }
  f.close();
}

void saveSensorCfg() {
  File f = SPIFFS.open("/sensor.json", "w");
  if (!f) return;
  StaticJsonDocument<256> doc;
  doc["enabled"]   = sensorCfg.enabled;
  doc["threshold"] = sensorCfg.threshold;
  doc["action"]    = sensorCfg.action;
  doc["dimLevel"]  = sensorCfg.dimLevel;
  serializeJson(doc, f);
  f.close();
}

void loadSensorCfg() {
  if (!SPIFFS.exists("/sensor.json")) return;
  File f = SPIFFS.open("/sensor.json", "r");
  if (!f) return;
  StaticJsonDocument<256> doc;
  if (!deserializeJson(doc, f)) {
    sensorCfg.enabled   = doc["enabled"]   | false;
    sensorCfg.threshold = doc["threshold"] | 500;
    sensorCfg.action    = doc["action"] | "dim";
    sensorCfg.dimLevel  = doc["dimLevel"]  | 20;
  }
  f.close();
}

// ============================================================
//  Schedule checker  (called once per minute)
// ============================================================
void checkSchedules() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;

  int minute = ti.tm_hour * 60 + ti.tm_min;
  if (minute == lastMinuteChecked) return;
  lastMinuteChecked = minute;

  char hhmm[6];
  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
  int dow = ti.tm_wday; // 0=Sun

  for (auto& s : schedules) {
    if (!s.enabled)       continue;
    if (!s.days[dow])     continue;
    if (s.time != hhmm)   continue;

    currentState.on         = (s.brightness > 0);
    currentState.brightness = s.brightness;
    currentState.colorTemp  = s.colorTemp;
    applyState(currentState);
    saveState();
    Serial.printf("[Schedule] Fired: %s  bri=%d  ct=%d\n", hhmm, s.brightness, s.colorTemp);
  }
}

// ============================================================
//  Light sensor logic  (polled every 2 s)
// ============================================================
void checkLightSensor() {
  if (!sensorCfg.enabled) return;

  int reading = analogRead(PIN_LIGHT_SENSOR);
  bool dark = (reading < sensorCfg.threshold);

  if (dark && !sensorTriggered) {
    preTriggerState = currentState;
    sensorTriggered = true;
    if (sensorCfg.action == "on") {
      currentState.on = true;
      currentState.brightness = 100;
    } else if (sensorCfg.action == "off") {
      currentState.on = false;
    } else { // "dim"
      currentState.on = true;
      currentState.brightness = sensorCfg.dimLevel;
    }
    applyState(currentState);
    Serial.printf("[Sensor] Dark triggered (ADC=%d). Action: %s\n", reading, sensorCfg.action.c_str());

  } else if (!dark && sensorTriggered) {
    currentState    = preTriggerState;
    sensorTriggered = false;
    applyState(currentState);
    Serial.printf("[Sensor] Light restored (ADC=%d)\n", reading);
  }
}

// ============================================================
//  Inline HTML/CSS/JS web portal
// ============================================================
static const char INDEX_HTML[] PROGMEM = R"HTMLEOF(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cabinet Lights</title>
<style>
:root{--bg:#0e0e14;--card:#17171f;--border:#262636;--accent:#f5a623;--cool:#87ceeb;--text:#dde;--muted:#66668a;--red:#e05555;--green:#4ecb71}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px;max-width:520px;margin:auto}
h1{font-size:1.3rem;color:var(--accent);text-align:center;margin-bottom:20px;letter-spacing:.06em}
h2{font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:18px;margin-bottom:14px}
.row{display:flex;align-items:center;gap:10px;margin-bottom:12px}
.row:last-child{margin-bottom:0}
label.lbl{font-size:.78rem;color:var(--muted);min-width:88px;flex-shrink:0}
input[type=range]{flex:1;accent-color:var(--accent);cursor:pointer;height:5px}
.val{font-size:.8rem;min-width:38px;text-align:right}
/* toggle */
.tog{position:relative;width:48px;height:26px;flex-shrink:0}
.tog input{opacity:0;width:0;height:0}
.tslider{position:absolute;inset:0;background:var(--border);border-radius:26px;cursor:pointer;transition:.25s}
.tslider::before{content:'';position:absolute;width:18px;height:18px;left:4px;bottom:4px;background:#fff;border-radius:50%;transition:.25s}
.tog input:checked+.tslider{background:var(--accent)}
.tog input:checked+.tslider::before{transform:translateX(22px)}
/* color-temp gradient track */
#ctSlider{background:linear-gradient(to right,#ffb347,#ffe5b4,#ffffff,#cce8ff,#87ceeb);border-radius:4px}
/* buttons */
button{border:none;border-radius:7px;cursor:pointer;font-size:.8rem;font-weight:600;padding:7px 14px;transition:opacity .15s}
button:active{opacity:.65}
.btn-p{background:var(--accent);color:#000}
.btn-d{background:var(--red);color:#fff}
.btn-s{padding:4px 9px;font-size:.72rem}
/* schedule list */
.sched-item{display:flex;align-items:center;gap:8px;padding:9px 10px;background:var(--bg);border-radius:8px;margin-bottom:7px;font-size:.78rem}
.sched-time{font-weight:700;color:var(--accent);min-width:40px}
.sched-info{flex:1;color:var(--muted)}
.days{display:flex;gap:3px;margin-top:3px}
.day{width:22px;height:22px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:.62rem;background:var(--border);color:var(--muted);cursor:pointer;user-select:none;font-weight:600}
.day.on{background:var(--accent);color:#000}
/* add-schedule form */
details summary{cursor:pointer;font-size:.8rem;color:var(--accent);padding:4px 0;margin-top:4px}
.frow{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:10px;align-items:flex-end}
.fg label{display:block;font-size:.72rem;color:var(--muted);margin-bottom:3px}
input[type=time],input[type=number],select{background:var(--bg);border:1px solid var(--border);color:var(--text);border-radius:6px;padding:5px 8px;font-size:.78rem}
/* status */
.foot{text-align:center;font-size:.72rem;color:var(--muted);margin-top:8px}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--green);margin-right:5px}
.dot.err{background:var(--red)}
/* quick actions */
.qa-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:4px}
.qa-btn{display:flex;flex-direction:column;align-items:center;gap:5px;padding:14px 8px;border-radius:10px;background:var(--border);color:var(--text);width:100%;cursor:pointer;transition:background .2s,color .2s;border:none}
.qa-btn:active{opacity:.7;transform:scale(.97)}
.qa-icon{font-size:1.5rem;line-height:1;display:block}
.qa-lbl{font-size:.6rem;font-weight:700;text-transform:uppercase;letter-spacing:.07em;display:block}
.qa-btn.active{background:var(--accent);color:#000}
.qa-btn.cool-active{background:var(--cool);color:#000}
</style>
</head>
<body>
<h1>&#9728; Cabinet Lighting</h1>

<!-- QUICK ACTIONS -->
<div class="card">
  <h2>Quick Actions</h2>
  <div class="qa-grid">
    <button class="qa-btn" id="qaOnOff" onclick="qaTogglePower()">
      <span class="qa-icon">&#x23FB;</span>
      <span class="qa-lbl">Power</span>
    </button>
    <button class="qa-btn" id="qaSensor" onclick="qaToggleSensor()">
      <span class="qa-icon">&#128161;</span>
      <span class="qa-lbl">Sensor</span>
    </button>
    <button class="qa-btn" id="qaSchedule" onclick="qaToggleSchedules()">
      <span class="qa-icon">&#128336;</span>
      <span class="qa-lbl">Schedule</span>
    </button>
    <button class="qa-btn" onclick="qaNightLight()">
      <span class="qa-icon">&#127769;</span>
      <span class="qa-lbl">Night</span>
    </button>
    <button class="qa-btn" onclick="qaFullCool()">
      <span class="qa-icon">&#10052;</span>
      <span class="qa-lbl">Full Cool</span>
    </button>
    <button class="qa-btn" onclick="qaFullWarm()">
      <span class="qa-icon">&#127774;</span>
      <span class="qa-lbl">Full Warm</span>
    </button>
  </div>
</div>

<!-- CONTROL CARD -->
<div class="card">
  <h2>Control</h2>
  <div class="row">
    <label class="lbl">Power</label>
    <label class="tog">
      <input type="checkbox" id="pwr" onchange="sendState()">
      <span class="tslider"></span>
    </label>
    <span id="pwrLbl" style="font-size:.8rem;color:var(--muted)">OFF</span>
  </div>
  <div class="row">
    <label class="lbl">Brightness</label>
    <input type="range" id="bri" min="1" max="100" value="80" oninput="onBri(this.value)" onchange="sendState()">
    <span class="val" id="briV">80%</span>
  </div>
  <div class="row">
    <label class="lbl">Color Temp</label>
    <input type="range" id="ct" min="0" max="100" value="30" oninput="onCt(this.value)" onchange="sendState()">
    <span class="val" id="ctV" style="font-size:.7rem">Warm</span>
  </div>
  <div style="display:flex;justify-content:space-between;font-size:.68rem;color:var(--muted);padding:0 2px;margin-top:-6px">
    <span>&#127774; Warm</span><span>&#10052; Cool</span>
  </div>
</div>

<!-- SCHEDULE CARD -->
<div class="card">
  <h2>Schedules</h2>
  <div id="schedList"><p style="color:var(--muted);font-size:.78rem">No schedules yet.</p></div>
  <details id="addForm">
    <summary>+ Add Schedule</summary>
    <div style="margin-top:12px">
      <div class="frow">
        <div class="fg"><label>Time</label><input type="time" id="nTime" value="08:00"></div>
        <div class="fg"><label>Brightness %</label><input type="number" id="nBri" min="0" max="100" value="80" style="width:64px"></div>
        <div class="fg"><label>Color Temp %</label><input type="number" id="nCt" min="0" max="100" value="30" style="width:64px"></div>
      </div>
      <div style="font-size:.72rem;color:var(--muted);margin-bottom:6px">Days active:</div>
      <div class="days" id="nDays">
        <span class="day" data-d="0">Su</span>
        <span class="day on" data-d="1">Mo</span>
        <span class="day on" data-d="2">Tu</span>
        <span class="day on" data-d="3">We</span>
        <span class="day on" data-d="4">Th</span>
        <span class="day on" data-d="5">Fr</span>
        <span class="day" data-d="6">Sa</span>
      </div>
      <button class="btn-p" style="margin-top:12px" onclick="addSchedule()">Save Schedule</button>
    </div>
  </details>
</div>

<!-- LIGHT SENSOR CARD -->
<div class="card">
  <h2>Light Sensor</h2>
  <div class="row">
    <label class="lbl">Enable</label>
    <label class="tog">
      <input type="checkbox" id="senEn" onchange="saveSensor()">
      <span class="tslider"></span>
    </label>
    <span id="senReading" style="flex:1;text-align:right;font-size:.72rem;color:var(--muted)">ADC: —</span>
  </div>
  <div class="row">
    <label class="lbl">Dark Threshold</label>
    <input type="range" id="senThr" min="0" max="4095" value="500" oninput="document.getElementById('senThrV').textContent=this.value" onchange="saveSensor()">
    <span class="val" id="senThrV" style="min-width:44px">500</span>
  </div>
  <div style="display:flex;justify-content:space-between;align-items:center;margin:-8px 0 10px 98px">
    <span style="font-size:.68rem;color:var(--muted)">&#9432; Trigger when ADC reading falls below this</span>
    <button class="btn-s" style="background:var(--border);color:var(--text);white-space:nowrap" onclick="setBaseline()">Use Current</button>
  </div>
  <div class="row">
    <label class="lbl">When Dark</label>
    <select id="senAct" onchange="onSenAct();saveSensor()" style="flex:1">
      <option value="dim">Dim to level</option>
      <option value="off">Turn off</option>
      <option value="on">Turn on (full)</option>
    </select>
  </div>
  <div class="row" id="dimRow">
    <label class="lbl">Dim Level</label>
    <input type="range" id="senDim" min="1" max="100" value="20" oninput="document.getElementById('senDimV').textContent=this.value+'%'" onchange="saveSensor()">
    <span class="val" id="senDimV">20%</span>
  </div>
</div>

<div class="foot">
  <span class="dot" id="dot"></span><span id="statusTxt">Connecting…</span>
  &nbsp;|&nbsp; <span id="clk">--:--</span>
</div>

<script>
const DN = ['Su','Mo','Tu','We','Th','Fr','Sa'];

function onBri(v){ document.getElementById('briV').textContent = v+'%' }
function onCt(v){
  const t = v<15?'Warm':v>85?'Cool':v<40?'Warm-ish':v>60?'Cool-ish':'Neutral';
  document.getElementById('ctV').textContent = t;
}
function onSenAct(){
  document.getElementById('dimRow').style.display =
    document.getElementById('senAct').value==='dim' ? 'flex' : 'none';
}

// ---- State ----
function sendState(){
  const on = document.getElementById('pwr').checked;
  document.getElementById('pwrLbl').textContent = on ? 'ON' : 'OFF';
  post('/api/state',{on,brightness:+document.getElementById('bri').value,colorTemp:+document.getElementById('ct').value});
}
function loadState(){
  get('/api/state',d=>{
    document.getElementById('pwr').checked = d.on;
    document.getElementById('pwrLbl').textContent = d.on?'ON':'OFF';
    document.getElementById('bri').value = d.brightness;
    document.getElementById('ct').value  = d.colorTemp;
    onBri(d.brightness); onCt(d.colorTemp);
    updateQaStates();
  });
}

// ---- Schedules ----
let scheds = [];
function loadSchedules(){
  get('/api/schedules', data=>{ scheds=data; renderSchedules(); updateQaStates(); });
}
function renderSchedules(){
  const el = document.getElementById('schedList');
  if(!scheds.length){ el.innerHTML='<p style="color:var(--muted);font-size:.78rem">No schedules yet.</p>'; return; }
  el.innerHTML = scheds.map(s=>`
    <div class="sched-item">
      <span class="sched-time">${s.time}</span>
      <span class="sched-info">
        Bri: ${s.brightness}% &nbsp; CT: ${s.colorTemp}%
        <div class="days">${[0,1,2,3,4,5,6].map(i=>`<span class="day ${s.days[i]?'on':''}">${DN[i]}</span>`).join('')}</div>
      </span>
      <label class="tog" style="width:44px;height:24px">
        <input type="checkbox" ${s.enabled?'checked':''} onchange="toggleSched('${s.id}',this.checked)">
        <span class="tslider"></span>
      </label>
      <button class="btn-d btn-s" onclick="deleteSched('${s.id}')">&#x2715;</button>
    </div>`).join('');
}
function addSchedule(){
  const days=[0,0,0,0,0,0,0];
  document.querySelectorAll('#nDays .day').forEach(el=>{
    if(el.classList.contains('on')) days[+el.dataset.d]=1;
  });
  post('/api/schedules',{
    time: document.getElementById('nTime').value,
    brightness: +document.getElementById('nBri').value,
    colorTemp:  +document.getElementById('nCt').value,
    enabled: true, days
  }, ()=>{ document.getElementById('addForm').removeAttribute('open'); loadSchedules(); });
}
function deleteSched(id){ post('/api/schedule/delete',{id},loadSchedules); }
function toggleSched(id,enabled){ post('/api/schedule/toggle',{id,enabled},loadSchedules); }

document.querySelectorAll('#nDays .day').forEach(el=>{
  el.addEventListener('click',()=>el.classList.toggle('on'));
});

// ---- Sensor ----
function loadSensor(){
  get('/api/sensor',d=>{
    document.getElementById('senEn').checked = d.enabled;
    document.getElementById('senThr').value  = d.threshold;
    document.getElementById('senThrV').textContent = d.threshold;
    document.getElementById('senAct').value  = d.action;
    document.getElementById('senDim').value  = d.dimLevel;
    document.getElementById('senDimV').textContent = d.dimLevel+'%';
    onSenAct(); updateQaStates();
  });
}
function saveSensor(){
  post('/api/sensor',{
    enabled:   document.getElementById('senEn').checked,
    threshold: +document.getElementById('senThr').value,
    action:    document.getElementById('senAct').value,
    dimLevel:  +document.getElementById('senDim').value
  });
}

function setBaseline(){
  get('/api/sensor/reading',d=>{
    if(d==null||d.value===undefined) return;
    const thr=Math.min(d.value,4095);
    document.getElementById('senThr').value=thr;
    document.getElementById('senThrV').textContent=thr;
    saveSensor();
  });
}

// ---- Sensor reading poll ----
function pollSensor(){
  get('/api/sensor/reading',d=>{
document.getElementById('senReading').textContent='ADC: '+(d!=null&&d.value!==undefined?d.value:'—');
  });
}

// ---- Quick Actions ----
function updateQaStates(){
  document.getElementById('qaOnOff').classList.toggle('active', document.getElementById('pwr').checked);
  document.getElementById('qaSensor').classList.toggle('active', document.getElementById('senEn').checked);
  document.getElementById('qaSchedule').classList.toggle('active', scheds.some(s=>s.enabled));
}
function qaTogglePower(){
  const el=document.getElementById('pwr'); el.checked=!el.checked;
  sendState(); updateQaStates();
}
function qaToggleSensor(){
  const el=document.getElementById('senEn'); el.checked=!el.checked;
  saveSensor(); updateQaStates();
}
function qaToggleSchedules(){
  const anyOn=scheds.some(s=>s.enabled);
  let p=Promise.resolve();
  scheds.forEach(s=>{
    if(anyOn?s.enabled:!s.enabled)
      p=p.then(()=>fetch('/api/schedule/toggle',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:s.id,enabled:!anyOn})}).then(r=>r.json()));
  });
  p.then(()=>loadSchedules());
}
function qaNightLight(){
  document.getElementById('pwr').checked=true;
  document.getElementById('bri').value=5; onBri(5);
  document.getElementById('ct').value=0; onCt(0);
  sendState(); updateQaStates();
}
function qaFullCool(){
  document.getElementById('pwr').checked=true;
  document.getElementById('bri').value=100; onBri(100);
  document.getElementById('ct').value=100; onCt(100);
  sendState(); updateQaStates();
}
function qaFullWarm(){
  document.getElementById('pwr').checked=true;
  document.getElementById('bri').value=100; onBri(100);
  document.getElementById('ct').value=0; onCt(0);
  sendState(); updateQaStates();
}

// ---- Fetch helpers ----
function get(url, cb){
  fetch(url).then(r=>r.json()).then(cb)
    .catch(e=>setStatus('Error: '+e.message,true));
}
function post(url, body, cb){
  fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{ if(cb) cb(d); })
    .catch(e=>setStatus('Error: '+e.message,true));
}
function setStatus(msg,err){
  document.getElementById('statusTxt').textContent=msg;
  document.getElementById('dot').className='dot'+(err?' err':'');
}

// ---- Clock ----
function tick(){
  const n=new Date();
  document.getElementById('clk').textContent=
    String(n.getHours()).padStart(2,'0')+':'+String(n.getMinutes()).padStart(2,'0');
}

// ---- Init ----
loadState(); loadSchedules(); loadSensor(); tick();
setStatus('Connected');
setInterval(tick, 15000);
setInterval(pollSensor, 4000);
</script>
</body>
</html>
)HTMLEOF";

// ============================================================
//  JSON helpers
// ============================================================
String stateJson() {
  StaticJsonDocument<128> doc;
  doc["on"]         = currentState.on;
  doc["brightness"] = currentState.brightness;
  doc["colorTemp"]  = currentState.colorTemp;
  String s; serializeJson(doc, s); return s;
}

String sensorJson() {
  StaticJsonDocument<256> doc;
  doc["enabled"]   = sensorCfg.enabled;
  doc["threshold"] = sensorCfg.threshold;
  doc["action"]    = sensorCfg.action;
  doc["dimLevel"]  = sensorCfg.dimLevel;
  String s; serializeJson(doc, s); return s;
}

String schedulesJson() {
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& s : schedules) {
    JsonObject obj = arr.createNestedObject();
    obj["id"]         = s.id;
    obj["time"]       = s.time;
    obj["brightness"] = s.brightness;
    obj["colorTemp"]  = s.colorTemp;
    obj["enabled"]    = s.enabled;
    JsonArray d = obj.createNestedArray("days");
    for (int i = 0; i < 7; i++) d.add(s.days[i]);
  }
  String s; serializeJson(doc, s); return s;
}

String makeId() {
  return String(millis(), HEX) + String(esp_random() & 0xFFFF, HEX);
}

// ============================================================
//  Route setup
// ============================================================
void setupRoutes() {

  // ---- Serve index ----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // ---- GET /api/state ----
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", stateJson());
  });

  // ---- POST /api/state ----
  auto* stateH = new AsyncCallbackJsonWebHandler("/api/state",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      JsonObject obj = json.as<JsonObject>();
      if (obj.containsKey("on"))         currentState.on         = obj["on"].as<bool>();
      if (obj.containsKey("brightness")) currentState.brightness = obj["brightness"].as<uint8_t>();
      if (obj.containsKey("colorTemp"))  currentState.colorTemp  = obj["colorTemp"].as<uint8_t>();
      applyState(currentState);
      saveState();
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(stateH);

  // ---- GET /api/schedules ----
  server.on("/api/schedules", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", schedulesJson());
  });

  // ---- POST /api/schedules  (add) ----
  auto* addSchedH = new AsyncCallbackJsonWebHandler("/api/schedules",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      JsonObject obj = json.as<JsonObject>();
      Schedule s;
      s.id         = makeId();
      s.time       = obj["time"].as<String>();
      s.brightness = obj["brightness"] | 80;
      s.colorTemp  = obj["colorTemp"]  | 30;
      s.enabled    = obj["enabled"]    | true;
      JsonArray d  = obj["days"].as<JsonArray>();
      for (int i = 0; i < 7 && i < (int)d.size(); i++) s.days[i] = (bool)d[i];
      schedules.push_back(s);
      saveSchedules();
      req->send(200, "application/json", "{\"ok\":true,\"id\":\"" + s.id + "\"}");
    });
  server.addHandler(addSchedH);

  // ---- POST /api/schedule/delete ----
  auto* delSchedH = new AsyncCallbackJsonWebHandler("/api/schedule/delete",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      String id = json["id"].as<String>();
      schedules.erase(std::remove_if(schedules.begin(), schedules.end(),
        [&id](const Schedule& s){ return s.id == id; }), schedules.end());
      saveSchedules();
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(delSchedH);

  // ---- POST /api/schedule/toggle ----
  auto* togSchedH = new AsyncCallbackJsonWebHandler("/api/schedule/toggle",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      String id      = json["id"].as<String>();
      bool   enabled = json["enabled"].as<bool>();
      for (auto& s : schedules) {
        if (s.id == id) { s.enabled = enabled; break; }
      }
      saveSchedules();
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(togSchedH);

  // ---- GET /api/sensor/reading (must be before /api/sensor) ----
  server.on("/api/sensor/reading", HTTP_GET, [](AsyncWebServerRequest* req) {
    int v = analogRead(PIN_LIGHT_SENSOR);
    req->send(200, "application/json", "{\"value\":" + String(v) + "}");
  });

  // ---- GET /api/sensor ----
  server.on("/api/sensor", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", sensorJson());
  });

  // ---- POST /api/sensor ----
  auto* sensorH = new AsyncCallbackJsonWebHandler("/api/sensor",
    [](AsyncWebServerRequest* req, JsonVariant& json) {
      JsonObject obj = json.as<JsonObject>();
      if (obj.containsKey("enabled"))   sensorCfg.enabled   = obj["enabled"].as<bool>();
      if (obj.containsKey("threshold")) sensorCfg.threshold = obj["threshold"].as<int>();
      if (obj.containsKey("action"))    sensorCfg.action    = obj["action"].as<String>();
      if (obj.containsKey("dimLevel"))  sensorCfg.dimLevel  = obj["dimLevel"].as<uint8_t>();
      saveSensorCfg();
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.addHandler(sensorH);
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);

  // PWM channels
  ledcSetup(LEDC_CH_WARM, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(LEDC_CH_COOL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PIN_WARM_WHITE,  LEDC_CH_WARM);
  ledcAttachPin(PIN_COOL_WHITE,  LEDC_CH_COOL);

  // Start with lights off
  ledcWrite(LEDC_CH_WARM, 0);
  ledcWrite(LEDC_CH_COOL, 0);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed — formatting…");
  }

  // Restore persisted state
  loadState();
  loadSchedules();
  loadSensorCfg();
  applyState(currentState);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(400); Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed — running in offline mode");
  }

  // NTP time sync
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  Serial.println("[NTP] Syncing…");

  // HTTP routes
  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Server started on port 80");
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // Schedule check — every 30 s is fine, deduplicated by minute
  if (now - lastScheduleCheck >= 30000) {
    lastScheduleCheck = now;
    checkSchedules();
  }

  // Sensor check — every 2 s
  if (now - lastSensorCheck >= 2000) {
    lastSensorCheck = now;
    checkLightSensor();
  }
}
