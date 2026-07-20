/*
  Smart Industrial Control Panel v4.0
  ESP32 + OLED + DHT11 + HC-SR04 + IR Sensor

  v4.0 adds (on top of v3's AJAX live dashboard + non-blocking buzzer):
   - Live line-graph history for Temperature/Humidity (canvas, no external libs)
   - Live line-graph history for Distance
   - Uptime clock (HH:MM:SS from millis(), no RTC required)
   - Wi-Fi signal strength (RSSI -> bars)
   - Optional supply-voltage monitor (needs a resistor-divider on an ADC pin -
     disabled by default, flip ENABLE_VOLTAGE_MONITOR to 1 once you wire it up)
   - Full-screen flashing alarm banner on DANGER level
   - Animated buzzer icon (pulses with the actual beep pattern)
   - Smooth animated circular gauges
   - Light / dark mode toggle (saved in the browser)
   - CSV download of logged sensor history
   - Settings panel: change the danger/warning distance thresholds from the browser
   - Remote buzzer ON / OFF / AUTO override
   - Remote LED control (force green / red / off / auto)
   - Mobile-responsive layout
   - Still fully AJAX: only /data (1s) and /history (5s) are polled, page never reloads

  No new libraries needed - charts and gauges are hand-rolled with <canvas> + CSS,
  so this keeps working on a WiFi network with no internet access.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>

const char* ssid     = "HKTHEMASS 8077";
const char* password = "hkalagi9002";
WebServer server(80);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define GREEN_LED 25
#define RED_LED   26
#define BUZZER    19
#define BUTTON    27
#define DHTPIN    4
#define DHTTYPE   DHT11
#define TRIG_PIN  5
#define ECHO_PIN  18
#define IR_PIN    23

// ---- Optional supply-voltage monitor ----
// Wire a resistor divider (e.g. 30k / 7.5k) from the supply to this ADC pin,
// then set ENABLE_VOLTAGE_MONITOR to 1 and adjust R1/R2 to your real values.
#define ENABLE_VOLTAGE_MONITOR 0
#define VOLTAGE_PIN 34
#define VDIV_R1 30000.0
#define VDIV_R2 7500.0

// Buzzer intermittent beep interval (ms) at WARNING level
#define BUZZER_BEEP_INTERVAL 300

// History logging (used by charts + CSV export)
#define LOG_SIZE 300
#define LOG_INTERVAL_MS 5000

DHT dht(DHTPIN, DHTTYPE);

bool systemRunning   = false;
bool lastButtonState = HIGH;
unsigned long pressTime = 0;

float temperature = 0, humidity = 0, distanceCM = -1;
bool  irDetected  = false;

// ---- Buzzer / alarm state ----
int alarmLevel           = 0;   // 0 safe, 1 warning, 2 danger
unsigned long buzzerTimer = 0;
bool buzzerToggle         = false;

enum OverrideMode { MODE_AUTO, MODE_FORCE_ON, MODE_FORCE_OFF };
OverrideMode buzzerMode = MODE_AUTO;

enum LedMode { LED_AUTO, LED_FORCE_GREEN, LED_FORCE_RED, LED_FORCE_OFF };
LedMode ledMode = LED_AUTO;

// ---- Adjustable thresholds (changeable from the Settings panel) ----
float dangerDistance  = 10.0;
float warningDistance = 20.0;

// ---- History ring buffer for charts + CSV ----
struct LogEntry {
  unsigned long t; // seconds since boot
  float temp;
  float hum;
  float dist;
};
LogEntry logBuf[LOG_SIZE];
int logIndex = 0;
int logCount = 0;
unsigned long lastLogTime = 0;

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.0343 / 2.0;
}

void bootScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(22, 10);
  display.println("HK");
  display.setTextSize(1);
  display.setCursor(8, 40);
  display.println("SMART INDUSTRIAL");
  display.setCursor(35, 54);
  display.println("PANEL");
  display.display();
  delay(2000);
}

void selfTest() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Running Self Test...");
  display.display();
  digitalWrite(GREEN_LED, HIGH); delay(250); digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);   delay(250); digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER, HIGH);    delay(250); digitalWrite(BUZZER, LOW);
}

// ---------------------------------------------------------------------
// Non-blocking buzzer control. Call every loop() iteration.
// AUTO: level 0 off / level 1 intermittent beep / level 2 continuous on
// Manual override forces ON or OFF regardless of alarm level.
// ---------------------------------------------------------------------
void updateBuzzer() {
  if (buzzerMode == MODE_FORCE_OFF) { digitalWrite(BUZZER, LOW); return; }
  if (buzzerMode == MODE_FORCE_ON)  { digitalWrite(BUZZER, HIGH); return; }

  switch (alarmLevel) {
    case 0:
      digitalWrite(BUZZER, LOW);
      buzzerToggle = false;
      break;
    case 1:
      if (millis() - buzzerTimer >= BUZZER_BEEP_INTERVAL) {
        buzzerTimer = millis();
        buzzerToggle = !buzzerToggle;
        digitalWrite(BUZZER, buzzerToggle ? HIGH : LOW);
      }
      break;
    case 2:
      digitalWrite(BUZZER, HIGH);
      break;
  }
}

void updateLeds() {
  if (ledMode == LED_FORCE_GREEN) { digitalWrite(GREEN_LED, HIGH); digitalWrite(RED_LED, LOW); return; }
  if (ledMode == LED_FORCE_RED)   { digitalWrite(GREEN_LED, LOW);  digitalWrite(RED_LED, HIGH); return; }
  if (ledMode == LED_FORCE_OFF)   { digitalWrite(GREEN_LED, LOW);  digitalWrite(RED_LED, LOW);  return; }

  // AUTO
  if (!systemRunning) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, HIGH);
  } else if ((distanceCM > 0 && distanceCM < warningDistance) || irDetected) {
    digitalWrite(RED_LED, HIGH);
    digitalWrite(GREEN_LED, LOW);
  } else {
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);
  }
}

// Decide the current alarm level from live sensor data
void evaluateAlarmLevel() {
  if (!systemRunning) {
    alarmLevel = 0;
    return;
  }
  if ((distanceCM > 0 && distanceCM < dangerDistance) || irDetected) {
    alarmLevel = 2;
  } else if (distanceCM >= dangerDistance && distanceCM < warningDistance) {
    alarmLevel = 1;
  } else {
    alarmLevel = 0;
  }
}

float readSupplyVoltage() {
#if ENABLE_VOLTAGE_MONITOR
  int raw = analogRead(VOLTAGE_PIN);       // 0-4095 on ESP32 (12-bit ADC)
  float vAtPin = (raw / 4095.0) * 3.3;     // ESP32 ADC reference ~3.3V
  float vSupply = vAtPin * ((VDIV_R1 + VDIV_R2) / VDIV_R2);
  return vSupply;
#else
  return 0.0;
#endif
}

void logReading() {
  logBuf[logIndex].t    = millis() / 1000;
  logBuf[logIndex].temp = temperature;
  logBuf[logIndex].hum  = humidity;
  logBuf[logIndex].dist = distanceCM;
  logIndex = (logIndex + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
}

String modeToString(OverrideMode m) {
  if (m == MODE_FORCE_ON) return "on";
  if (m == MODE_FORCE_OFF) return "off";
  return "auto";
}
String ledModeToString(LedMode m) {
  if (m == LED_FORCE_GREEN) return "green";
  if (m == LED_FORCE_RED) return "red";
  if (m == LED_FORCE_OFF) return "off";
  return "auto";
}

// ---------------------------------------------------------------------
// /data -> JSON snapshot of live sensor + system state, polled every 1s
// ---------------------------------------------------------------------
void handleData() {
  String json = "{";
  json += "\"running\":" + String(systemRunning ? "true" : "false") + ",";
  json += "\"temperature\":" + String(temperature, 1) + ",";
  json += "\"humidity\":" + String(humidity, 1) + ",";
  json += "\"distance\":" + String(distanceCM, 1) + ",";
  json += "\"ir\":" + String(irDetected ? "true" : "false") + ",";
  json += "\"alarmLevel\":" + String(alarmLevel) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"voltageEnabled\":" + String(ENABLE_VOLTAGE_MONITOR ? "true" : "false") + ",";
  json += "\"voltage\":" + String(readSupplyVoltage(), 2) + ",";
  json += "\"dangerDistance\":" + String(dangerDistance, 1) + ",";
  json += "\"warningDistance\":" + String(warningDistance, 1) + ",";
  json += "\"buzzerMode\":\"" + modeToString(buzzerMode) + "\",";
  json += "\"ledMode\":\"" + ledModeToString(ledMode) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// /history -> JSON array of logged readings, polled every 5s for the charts
// ---------------------------------------------------------------------
void handleHistory() {
  String json = "[";
  int start = (logCount < LOG_SIZE) ? 0 : logIndex; // oldest entry
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_SIZE;
    if (i > 0) json += ",";
    json += "{\"t\":" + String(logBuf[idx].t);
    json += ",\"temp\":" + String(logBuf[idx].temp, 1);
    json += ",\"hum\":" + String(logBuf[idx].hum, 1);
    json += ",\"dist\":" + String(logBuf[idx].dist, 1) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// /csv -> downloadable CSV of the same log buffer
// ---------------------------------------------------------------------
void handleCsv() {
  String csv = "seconds_since_boot,temperature_c,humidity_pct,distance_cm\n";
  int start = (logCount < LOG_SIZE) ? 0 : logIndex;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_SIZE;
    csv += String(logBuf[idx].t) + "," + String(logBuf[idx].temp, 1) + "," +
           String(logBuf[idx].hum, 1) + "," + String(logBuf[idx].dist, 1) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=sensor_log.csv");
  server.send(200, "text/csv", csv);
}

// ---------------------------------------------------------------------
// /setThreshold?danger=10&warning=20
// ---------------------------------------------------------------------
void handleSetThreshold() {
  if (server.hasArg("danger"))  dangerDistance  = server.arg("danger").toFloat();
  if (server.hasArg("warning")) warningDistance = server.arg("warning").toFloat();
  if (dangerDistance < 1) dangerDistance = 1;
  if (warningDistance <= dangerDistance) warningDistance = dangerDistance + 1;
  String json = "{\"status\":\"ok\",\"dangerDistance\":" + String(dangerDistance, 1) +
                ",\"warningDistance\":" + String(warningDistance, 1) + "}";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// /buzzer?mode=auto|on|off
// ---------------------------------------------------------------------
void handleBuzzerControl() {
  String m = server.arg("mode");
  if (m == "on") buzzerMode = MODE_FORCE_ON;
  else if (m == "off") buzzerMode = MODE_FORCE_OFF;
  else buzzerMode = MODE_AUTO;
  server.send(200, "application/json", "{\"status\":\"ok\",\"buzzerMode\":\"" + modeToString(buzzerMode) + "\"}");
}

// ---------------------------------------------------------------------
// /led?mode=auto|green|red|off
// ---------------------------------------------------------------------
void handleLedControl() {
  String m = server.arg("mode");
  if (m == "green") ledMode = LED_FORCE_GREEN;
  else if (m == "red") ledMode = LED_FORCE_RED;
  else if (m == "off") ledMode = LED_FORCE_OFF;
  else ledMode = LED_AUTO;
  server.send(200, "application/json", "{\"status\":\"ok\",\"ledMode\":\"" + ledModeToString(ledMode) + "\"}");
}

// ---------------------------------------------------------------------
// Root dashboard page
// ---------------------------------------------------------------------
void handleRoot() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Industrial Control Panel</title>
<style>
  :root{
    --safe:#2ecc71; --warn:#f1c40f; --danger:#e74c3c;
    --bg:#0d1117; --panel:#161b22; --text:#e6edf3; --muted:#8b949e;
    --grid:#2b3340;
  }
  body.light{
    --bg:#f4f6f8; --panel:#ffffff; --text:#1b1f24; --muted:#57606a; --grid:#e2e6ea;
  }
  *{box-sizing:border-box;}
  body{
    margin:0; font-family:'Segoe UI',Roboto,Arial,sans-serif;
    background:radial-gradient(circle at top, color-mix(in srgb, var(--bg) 85%, #2a3446), var(--bg) 70%);
    color:var(--text); min-height:100vh; padding:16px 12px 40px;
    transition:background 0.4s ease, color 0.4s ease;
  }
  .topbar{
    max-width:1000px; margin:0 auto 8px auto;
    display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:8px;
  }
  h1{ font-size:1.3rem; letter-spacing:1.5px; text-transform:uppercase; margin:0; }
  .subtitle{ color:var(--muted); font-size:0.75rem; }
  .top-controls{ display:flex; align-items:center; gap:14px; font-size:0.8rem; color:var(--muted); flex-wrap:wrap; }
  .icon-btn{
    background:var(--panel); border:1px solid rgba(255,255,255,0.08); color:var(--text);
    border-radius:10px; padding:6px 10px; cursor:pointer; font-size:0.9rem;
  }
  .wifi-bars{ display:inline-flex; align-items:flex-end; gap:2px; height:14px; vertical-align:middle; }
  .wifi-bars span{ width:3px; background:var(--muted); border-radius:1px; }
  .wifi-bars span.on{ background:var(--safe); }

  .grid{ display:grid; grid-template-columns:repeat(auto-fit,minmax(200px,1fr)); gap:16px; max-width:1000px; margin:0 auto; }
  .card{
    background:var(--panel); border-radius:16px; padding:18px; text-align:center;
    box-shadow:0 4px 18px rgba(0,0,0,0.25); border:1px solid rgba(255,255,255,0.05);
    transition:box-shadow 0.4s ease, transform 0.3s ease, background 0.4s ease;
  }
  .card:hover{ transform:translateY(-3px); }
  .card h2{ font-size:0.72rem; color:var(--muted); letter-spacing:1px; text-transform:uppercase; margin:0 0 10px 0; }
  .wide{ grid-column:1 / -1; }

  .gauge{
    width:110px; height:110px; margin:0 auto 8px auto; border-radius:50%;
    display:flex; align-items:center; justify-content:center;
    background:conic-gradient(var(--gauge-color,#2ecc71) calc(var(--pct,0)*1%), var(--grid) 0);
    transition:background 0.8s cubic-bezier(.4,0,.2,1); position:relative;
  }
  .gauge::before{ content:""; position:absolute; width:84px; height:84px; border-radius:50%; background:var(--panel); transition:background 0.4s ease; }
  .gauge span{ position:relative; font-size:1.25rem; font-weight:600; }

  .bar-track{ width:100%; height:14px; border-radius:8px; background:var(--grid); overflow:hidden; margin-top:6px; }
  .bar-fill{ height:100%; width:0%; border-radius:8px; background:linear-gradient(90deg,#2ecc71,#f1c40f,#e74c3c); transition:width 0.6s ease; }

  .status-dot{ width:16px; height:16px; border-radius:50%; display:inline-block; margin-right:8px; background:var(--muted); transition:background 0.4s ease, box-shadow 0.4s ease; }
  .status-dot.safe{ background:var(--safe); box-shadow:0 0 12px var(--safe); }
  .status-dot.warn{ background:var(--warn); box-shadow:0 0 12px var(--warn); animation:pulse 1s infinite; }
  .status-dot.danger{ background:var(--danger); box-shadow:0 0 16px var(--danger); animation:pulse 0.5s infinite; }
  @keyframes pulse{ 0%{opacity:1;transform:scale(1);} 50%{opacity:.5;transform:scale(1.25);} 100%{opacity:1;transform:scale(1);} }

  .banner{
    max-width:1000px; margin:0 auto 16px auto; padding:12px 18px; border-radius:12px;
    font-weight:600; text-align:center; letter-spacing:1px; background:var(--panel);
    border:1px solid rgba(255,255,255,0.06); transition:background 0.4s ease, color 0.4s ease;
  }
  .banner.safe{ color:var(--safe); }
  .banner.warn{ color:var(--warn); }
  .banner.danger{ color:#fff; background:var(--danger); animation:flash 1s infinite; }
  @keyframes flash{ 0%,100%{filter:brightness(1);} 50%{filter:brightness(1.3);} }

  /* Full-screen danger overlay */
  #fullAlarm{
    position:fixed; inset:0; background:rgba(231,76,60,0.92); z-index:999;
    display:none; align-items:center; justify-content:center; flex-direction:column;
    color:#fff; text-align:center; animation:flashBg 0.6s infinite;
  }
  #fullAlarm.show{ display:flex; }
  #fullAlarm .big{ font-size:2.4rem; font-weight:800; letter-spacing:2px; }
  #fullAlarm .small{ margin-top:10px; font-size:1rem; opacity:0.9; }
  @keyframes flashBg{ 0%,100%{ background:rgba(231,76,60,0.92);} 50%{ background:rgba(150,20,15,0.95);} }

  .buzzer-icon{ font-size:2.2rem; display:inline-block; transition:transform 0.15s ease, color 0.3s ease; }
  .buzzer-icon.beeping{ animation:buzz 0.3s infinite; color:var(--warn); }
  .buzzer-icon.blaring{ animation:buzz 0.15s infinite; color:var(--danger); }
  @keyframes buzz{ 0%,100%{ transform:scale(1) rotate(0deg);} 50%{ transform:scale(1.2) rotate(-6deg);} }

  .ir-icon{ font-size:2rem; transition:transform 0.3s ease, color 0.3s ease; }
  .ir-icon.active{ color:var(--danger); transform:scale(1.3); }

  canvas.chart{ width:100%; height:160px; background:var(--grid); border-radius:10px; }
  .legend{ display:flex; gap:14px; justify-content:center; margin-top:8px; font-size:0.72rem; color:var(--muted); flex-wrap:wrap; }
  .legend span{ display:inline-flex; align-items:center; gap:5px; }
  .swatch{ width:10px; height:10px; border-radius:2px; display:inline-block; }

  .btn-row{ display:flex; gap:8px; justify-content:center; flex-wrap:wrap; margin-top:8px; }
  .btn{
    background:var(--grid); color:var(--text); border:1px solid rgba(255,255,255,0.08);
    padding:6px 12px; border-radius:8px; font-size:0.75rem; cursor:pointer; transition:background 0.2s ease, color 0.2s ease;
  }
  .btn.active{ background:var(--safe); color:#04210f; font-weight:700; }
  .btn.active.danger-active{ background:var(--danger); color:#fff; }

  .settings-form{ display:flex; gap:10px; justify-content:center; flex-wrap:wrap; align-items:flex-end; margin-top:10px; }
  .settings-form label{ font-size:0.7rem; color:var(--muted); display:block; margin-bottom:4px; }
  .settings-form input{
    width:90px; padding:6px 8px; border-radius:6px; border:1px solid rgba(255,255,255,0.1);
    background:var(--bg); color:var(--text);
  }

  .download-link{
    display:inline-block; margin-top:10px; padding:8px 16px; border-radius:8px;
    background:var(--safe); color:#04210f; font-weight:700; text-decoration:none; font-size:0.8rem;
  }

  .footer-row{ display:flex; justify-content:center; align-items:center; gap:10px; margin-top:20px; color:var(--muted); font-size:0.72rem; }

  @media (max-width:480px){
    h1{ font-size:1.05rem; }
    .gauge{ width:90px; height:90px; }
    .gauge::before{ width:68px; height:68px; }
  }
</style>
</head>
<body>

<div id="fullAlarm">
  <div class="big">🚨 DANGER 🚨</div>
  <div class="small">Object too close or IR triggered — buzzer active</div>
</div>

<div class="topbar">
  <div>
    <h1>Smart Industrial Control Panel</h1>
    <div class="subtitle">Live dashboard &middot; <span id="connStatus">connecting...</span></div>
  </div>
  <div class="top-controls">
    <span>⏱ <span id="uptime">--:--:--</span></span>
    <span>📶 <span class="wifi-bars" id="wifiBars">
      <span style="height:4px"></span><span style="height:7px"></span><span style="height:10px"></span><span style="height:13px"></span>
    </span> <span id="rssiVal">--</span> dBm</span>
    <span id="voltageWrap">🔋 <span id="voltageVal">--</span> V</span>
    <button class="icon-btn" id="themeToggle">🌙 / ☀️</button>
  </div>
</div>

<div id="banner" class="banner safe">SYSTEM STOPPED</div>

<div class="grid">

  <div class="card">
    <h2>Temperature</h2>
    <div class="gauge" id="tempGauge"><span id="tempVal">--</span></div>
    <div>&deg;C</div>
  </div>

  <div class="card">
    <h2>Humidity</h2>
    <div class="gauge" id="humGauge"><span id="humVal">--</span></div>
    <div>%</div>
  </div>

  <div class="card">
    <h2>Distance</h2>
    <div style="font-size:1.5rem;font-weight:600;" id="distVal">-- cm</div>
    <div class="bar-track"><div class="bar-fill" id="distBar"></div></div>
  </div>

  <div class="card">
    <h2>IR Sensor</h2>
    <div class="ir-icon" id="irIcon">&#128225;</div>
    <div id="irText">CLEAR</div>
  </div>

  <div class="card">
    <h2>System State</h2>
    <div style="margin-top:8px;">
      <span class="status-dot safe" id="sysDot"></span>
      <span id="sysText" style="font-weight:600;">STOPPED</span>
    </div>
  </div>

  <div class="card">
    <h2>Buzzer / Alarm</h2>
    <div class="buzzer-icon" id="buzzerIcon">🔊</div>
    <div style="margin-top:6px;">
      <span class="status-dot safe" id="alarmDot"></span>
      <span id="alarmText" style="font-weight:600;">SAFE</span>
    </div>
    <div class="btn-row">
      <button class="btn" data-buzzer="auto">Auto</button>
      <button class="btn" data-buzzer="on">On</button>
      <button class="btn" data-buzzer="off">Off</button>
    </div>
  </div>

  <div class="card">
    <h2>LED Control</h2>
    <div style="font-size:1.6rem;">💡</div>
    <div class="btn-row">
      <button class="btn" data-led="auto">Auto</button>
      <button class="btn" data-led="green">Green</button>
      <button class="btn" data-led="red">Red</button>
      <button class="btn" data-led="off">Off</button>
    </div>
  </div>

  <div class="card wide">
    <h2>Temperature &amp; Humidity History</h2>
    <canvas class="chart" id="thChart" width="900" height="160"></canvas>
    <div class="legend">
      <span><span class="swatch" style="background:#e74c3c"></span>Temperature (&deg;C)</span>
      <span><span class="swatch" style="background:#3498db"></span>Humidity (%)</span>
    </div>
  </div>

  <div class="card wide">
    <h2>Distance History</h2>
    <canvas class="chart" id="distChart" width="900" height="160"></canvas>
    <div class="legend">
      <span><span class="swatch" style="background:#2ecc71"></span>Distance (cm)</span>
      <span><span class="swatch" style="background:#f1c40f"></span>Warning threshold</span>
      <span><span class="swatch" style="background:#e74c3c"></span>Danger threshold</span>
    </div>
    <div style="text-align:center;">
      <a class="download-link" href="/csv" download="sensor_log.csv">📥 Download CSV</a>
    </div>
  </div>

  <div class="card wide">
    <h2>Settings — Distance Thresholds (cm)</h2>
    <div class="settings-form">
      <div>
        <label>Danger (buzzer solid)</label>
        <input type="number" id="dangerInput" min="1" step="1">
      </div>
      <div>
        <label>Warning (buzzer beeps)</label>
        <input type="number" id="warningInput" min="2" step="1">
      </div>
      <button class="icon-btn" id="saveThresholds">Save</button>
    </div>
  </div>

</div>

<div class="footer-row">
  <span>HK Smart Industrial Panel &middot; v4.0</span>
</div>

<script>
const thCanvas = document.getElementById('thChart');
const distCanvas = document.getElementById('distChart');
let currentDanger = 10, currentWarning = 20;

// ---------------- Theme ----------------
function applyTheme(mode){
  document.body.classList.toggle('light', mode === 'light');
  localStorage.setItem('panelTheme', mode);
}
document.getElementById('themeToggle').addEventListener('click', () => {
  const isLight = document.body.classList.contains('light');
  applyTheme(isLight ? 'dark' : 'light');
});
applyTheme(localStorage.getItem('panelTheme') || 'dark');

// ---------------- Generic canvas line chart ----------------
function drawLineChart(canvas, series, thresholds){
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0,0,w,h);

  let allVals = [];
  series.forEach(s => allVals = allVals.concat(s.data));
  (thresholds||[]).forEach(t => allVals.push(t.value));
  if (allVals.length === 0) return;

  let minV = Math.min(...allVals), maxV = Math.max(...allVals);
  if (minV === maxV) { minV -= 1; maxV += 1; }
  const pad = (maxV - minV) * 0.1;
  minV -= pad; maxV += pad;

  const marginL = 34, marginR = 10, marginT = 10, marginB = 10;
  const plotW = w - marginL - marginR;
  const plotH = h - marginT - marginB;

  // gridlines
  ctx.strokeStyle = 'rgba(150,150,150,0.15)';
  ctx.lineWidth = 1;
  ctx.font = '10px sans-serif';
  ctx.fillStyle = 'rgba(150,150,150,0.7)';
  for(let i=0;i<=4;i++){
    const y = marginT + (plotH/4)*i;
    ctx.beginPath(); ctx.moveTo(marginL,y); ctx.lineTo(w-marginR,y); ctx.stroke();
    const val = maxV - ((maxV-minV)/4)*i;
    ctx.fillText(val.toFixed(0), 2, y+3);
  }

  // threshold lines
  (thresholds||[]).forEach(t => {
    const y = marginT + plotH - ((t.value - minV)/(maxV-minV))*plotH;
    ctx.strokeStyle = t.color;
    ctx.setLineDash([4,4]);
    ctx.beginPath(); ctx.moveTo(marginL,y); ctx.lineTo(w-marginR,y); ctx.stroke();
    ctx.setLineDash([]);
  });

  // series lines
  series.forEach(s => {
    if (s.data.length < 2) return;
    ctx.strokeStyle = s.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    s.data.forEach((v,i) => {
      const x = marginL + (plotW/(s.data.length-1))*i;
      const y = marginT + plotH - ((v - minV)/(maxV-minV))*plotH;
      if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    });
    ctx.stroke();
  });
}

async function pollHistory(){
  try{
    const res = await fetch('/history', { cache:'no-store' });
    const hist = await res.json();
    const temps = hist.map(e => e.temp);
    const hums  = hist.map(e => e.hum);
    const dists = hist.map(e => e.dist);

    drawLineChart(thCanvas, [
      { data: temps, color: '#e74c3c' },
      { data: hums,  color: '#3498db' }
    ]);
    drawLineChart(distCanvas, [
      { data: dists, color: '#2ecc71' }
    ], [
      { value: currentWarning, color: '#f1c40f' },
      { value: currentDanger,  color: '#e74c3c' }
    ]);
  }catch(e){ /* history fetch failed, ignore this cycle */ }
}

// ---------------- Live data poll ----------------
function formatUptime(sec){
  const h = String(Math.floor(sec/3600)).padStart(2,'0');
  const m = String(Math.floor((sec%3600)/60)).padStart(2,'0');
  const s = String(Math.floor(sec%60)).padStart(2,'0');
  return h+':'+m+':'+s;
}

function updateWifiBars(rssi){
  const bars = document.querySelectorAll('#wifiBars span');
  let strength = 0;
  if (rssi >= -55) strength = 4;
  else if (rssi >= -65) strength = 3;
  else if (rssi >= -75) strength = 2;
  else if (rssi >= -85) strength = 1;
  bars.forEach((b,i) => b.classList.toggle('on', i < strength));
}

async function pollData(){
  try{
    const res = await fetch('/data', { cache:'no-store' });
    const d = await res.json();
    document.getElementById('connStatus').textContent = 'live';

    document.getElementById('uptime').textContent = formatUptime(d.uptime);
    document.getElementById('rssiVal').textContent = d.rssi;
    updateWifiBars(d.rssi);

    const voltWrap = document.getElementById('voltageWrap');
    if (d.voltageEnabled){
      voltWrap.style.display = 'inline';
      document.getElementById('voltageVal').textContent = d.voltage.toFixed(2);
    } else {
      voltWrap.style.display = 'none';
    }

    currentDanger = d.dangerDistance;
    currentWarning = d.warningDistance;
    if (document.activeElement.id !== 'dangerInput')  document.getElementById('dangerInput').value  = d.dangerDistance;
    if (document.activeElement.id !== 'warningInput') document.getElementById('warningInput').value = d.warningDistance;

    const tPct = Math.max(0, Math.min(100, (d.temperature/50)*100));
    const tempGauge = document.getElementById('tempGauge');
    tempGauge.style.setProperty('--pct', tPct);
    tempGauge.style.setProperty('--gauge-color', d.temperature>40?'var(--danger)':(d.temperature>32?'var(--warn)':'var(--safe)'));
    document.getElementById('tempVal').textContent = d.temperature.toFixed(1);

    const hGauge = document.getElementById('humGauge');
    hGauge.style.setProperty('--pct', d.humidity);
    hGauge.style.setProperty('--gauge-color', 'var(--safe)');
    document.getElementById('humVal').textContent = d.humidity.toFixed(0);

    document.getElementById('distVal').textContent = d.distance < 0 ? 'ERROR' : d.distance.toFixed(1)+' cm';
    let distPct = d.distance < 0 ? 0 : Math.max(0, Math.min(100, 100-(d.distance/100)*100));
    document.getElementById('distBar').style.width = distPct + '%';

    const irIcon = document.getElementById('irIcon');
    document.getElementById('irText').textContent = d.ir ? 'DETECTED' : 'CLEAR';
    irIcon.classList.toggle('active', d.ir);

    const sysDot = document.getElementById('sysDot');
    sysDot.className = 'status-dot ' + (d.running ? 'safe' : '');
    document.getElementById('sysText').textContent = d.running ? 'RUNNING' : 'STOPPED';

    const alarmDot = document.getElementById('alarmDot');
    const alarmText = document.getElementById('alarmText');
    const banner = document.getElementById('banner');
    const fullAlarm = document.getElementById('fullAlarm');
    const buzzerIcon = document.getElementById('buzzerIcon');

    if (d.alarmLevel === 2){
      alarmDot.className = 'status-dot danger';
      alarmText.textContent = 'DANGER - BUZZER ON';
      banner.className = 'banner danger';
      banner.textContent = 'DANGER: OBJECT TOO CLOSE / IR TRIGGERED';
      fullAlarm.classList.add('show');
      buzzerIcon.className = 'buzzer-icon blaring';
    } else if (d.alarmLevel === 1){
      alarmDot.className = 'status-dot warn';
      alarmText.textContent = 'WARNING - BEEPING';
      banner.className = 'banner warn';
      banner.textContent = 'WARNING: OBJECT APPROACHING';
      fullAlarm.classList.remove('show');
      buzzerIcon.className = 'buzzer-icon beeping';
    } else {
      alarmDot.className = 'status-dot safe';
      alarmText.textContent = 'SAFE';
      banner.className = 'banner ' + (d.running ? 'safe' : '');
      banner.textContent = d.running ? 'SYSTEM RUNNING - ALL CLEAR' : 'SYSTEM STOPPED';
      fullAlarm.classList.remove('show');
      buzzerIcon.className = 'buzzer-icon';
    }

    document.querySelectorAll('[data-buzzer]').forEach(b => b.classList.toggle('active', b.dataset.buzzer === d.buzzerMode));
    document.querySelectorAll('[data-led]').forEach(b => {
      const isActive = b.dataset.led === d.ledMode;
      b.classList.toggle('active', isActive);
      b.classList.toggle('danger-active', isActive && b.dataset.led === 'red');
    });

  }catch(e){
    document.getElementById('connStatus').textContent = 'connection lost, retrying...';
  }
}

// ---------------- Controls ----------------
document.querySelectorAll('[data-buzzer]').forEach(btn => {
  btn.addEventListener('click', () => fetch('/buzzer?mode=' + btn.dataset.buzzer));
});
document.querySelectorAll('[data-led]').forEach(btn => {
  btn.addEventListener('click', () => fetch('/led?mode=' + btn.dataset.led));
});
document.getElementById('saveThresholds').addEventListener('click', () => {
  const dv = document.getElementById('dangerInput').value;
  const wv = document.getElementById('warningInput').value;
  fetch('/setThreshold?danger=' + dv + '&warning=' + wv);
});

poll_all();
function poll_all(){ pollData(); pollHistory(); }
setInterval(pollData, 1000);
setInterval(pollHistory, 5000);
</script>
</body>
</html>
)HTML";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(IR_PIN, INPUT);
#if ENABLE_VOLTAGE_MONITOR
  pinMode(VOLTAGE_PIN, INPUT);
#endif

  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();
  bootScreen();
  selfTest();

  WiFi.begin(ssid, password);
  Serial.print("Connecting");

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected");
    Serial.print("IP Address : ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi Failed!");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/csv", handleCsv);
  server.on("/setThreshold", handleSetThreshold);
  server.on("/buzzer", handleBuzzerControl);
  server.on("/led", handleLedControl);
  server.begin();
}

void loop() {
  bool button = digitalRead(BUTTON);
  if (lastButtonState == HIGH && button == LOW) pressTime = millis();
  if (lastButtonState == LOW && button == HIGH) {
    unsigned long d = millis() - pressTime;
    if (d < 1000) systemRunning = !systemRunning;
    else if (d >= 3000) systemRunning = false;
  }
  lastButtonState = button;

  temperature = dht.readTemperature();
  humidity    = dht.readHumidity();
  distanceCM  = getDistance();
  irDetected  = (digitalRead(IR_PIN) == LOW);

  evaluateAlarmLevel();
  updateBuzzer();
  updateLeds();

  if (millis() - lastLogTime >= LOG_INTERVAL_MS) {
    lastLogTime = millis();
    logReading();
  }

  // ---- OLED ----
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(systemRunning ? "RUNNING" : "STOPPED");

  display.setCursor(0, 16);
  display.print("T:"); display.print(temperature, 1); display.print("C");
  display.setCursor(68, 16);
  display.print("H:"); display.print(humidity, 0); display.print("%");

  display.setCursor(0, 32);
  display.print("D:");
  if (distanceCM < 0) display.print("ERR");
  else { display.print(distanceCM, 1); display.print("cm"); }

  display.setCursor(0, 48);
  display.print("IR:");
  display.print(irDetected ? "DETECTED" : "CLEAR");
  display.display();

  server.handleClient();
  delay(50);
}
