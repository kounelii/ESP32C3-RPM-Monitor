#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "esp_wifi.h"

#define WHEEL1_PIN 0
#define WHEEL2_PIN 1
#define LED_PIN    8

Preferences prefs;
WebServer server(80);

// Measurement
volatile unsigned long lastPulse1 = 0, lastPulse2 = 0;
volatile unsigned long delta1 = 0, delta2 = 0;
float rpm1 = 0, rpm2 = 0, rpm_diff = 0;
int rpm_diff_threshold = 50;
int rpm_update_interval = 400;
unsigned long rpm_flag_duration = 100;

// Flags
bool flag = false;
unsigned long flag_raised_time = 0;
bool led_warning_active = false;
bool led_heartbeat_active = false;
bool active_mode = true;

// Heartbeat
unsigned long last_heartbeat_time = 0;
unsigned long heartbeat_on_time = 0;

// WiFi config
String ap_ssid = "ESP-RPM";
String ap_pass = "12345678";
int ap_channel = 6;
String ap_mac = "random";
int ap_bw = 20;

// === ISR ===
void IRAM_ATTR isrWheel1() {
  unsigned long now = micros();
  if (lastPulse1 > 0) delta1 = now - lastPulse1;
  lastPulse1 = now;
}
void IRAM_ATTR isrWheel2() {
  unsigned long now = micros();
  if (lastPulse2 > 0) delta2 = now - lastPulse2;
  lastPulse2 = now;
}

void take_action() {
  Serial.println("⚠️ RPM difference sustained too long! Taking action...");
  // Add GPIO logic, alerts, etc.
}

// === HTML Pages ===
const char html_index[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>RPM Monitor</title>
<style>body{text-align:center;font-family:sans-serif}.data{font-size:2em}button{margin:10px}</style></head>
<body><h2>ESP32 RPM Monitor</h2>
<div class="data" id="rpm1">Wheel 1: ...</div>
<div class="data" id="rpm2">Wheel 2: ...</div>
<div class="data" id="diff">Difference: ...</div>
<br>
<button onclick="location.href='/config'">⚙️ Config</button>
<button onclick="location.href='/update'">⬆️ OTA</button>
<button onclick="location.href='/wifi'">📶 WiFi Setup</button>
<script>
const s=new EventSource('/events');
s.onmessage=e=>{
  let d=JSON.parse(e.data);
  document.getElementById('rpm1').textContent="Wheel 1: "+d.rpm1.toFixed(2)+" RPM";
  document.getElementById('rpm2').textContent="Wheel 2: "+d.rpm2.toFixed(2)+" RPM";
  document.getElementById('diff').textContent="Difference: "+d.diff.toFixed(2)+" RPM";
};
</script></body></html>
)rawliteral";

const char html_config[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>Config</title>
<style>body{text-align:center;font-family:sans-serif}</style></head><body>
<h2>Calibration</h2>
<form method="POST" action="/save">
  RPM Diff Threshold:<br>
  <input type="number" name="thresh" value="%THRESH%" min="0"><br><br>
  RPM Update Interval (ms):<br>
  <input type="number" name="interval" value="%INTERVAL%" min="100" max="5000"><br><br>
  Flag Duration (ms):<br>
  <input type="number" name="duration" value="%DURATION%" min="10" max="10000"><br><br>
  <button type="submit">Save</button>
</form>
<br><a href="/">← Back</a></body></html>
)rawliteral";

const char html_wifi[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>WiFi Config</title>
<style>body{text-align:center;font-family:sans-serif}</style></head><body>
<h2>WiFi Settings</h2>
<form action="/wifi-save" method="POST">
SSID:<br><input name="ssid" value="%SSID%"><br><br>
Password:<br><input name="pass" value="%PASS%"><br><br>
Channel:<br><input name="chan" type="number" min="1" max="13" value="%CHAN%"><br><br>
MAC (random or 12 hex digits):<br><input name="mac" value="%MAC%"><br><br>
Bandwidth (MHz):<br>
<select name="bw">
  <option value="20" %BW20%>20 MHz</option>
  <option value="40" %BW40%>40 MHz</option>
</select><br><br>
<button type="submit">Save</button>
</form><br><a href="/">← Back</a></body></html>
)rawliteral";

// === ROUTES ===
void pauseMeasurement() {
  active_mode = false;
  digitalWrite(LED_PIN, HIGH);
  led_heartbeat_active = false;
  led_warning_active = false;
}

void handleRoot() {
  active_mode = true;
  server.send_P(200, "text/html", html_index);
}

void handleConfig() {
  pauseMeasurement();
  String html = html_config;
  html.replace("%THRESH%", String(rpm_diff_threshold));
  html.replace("%INTERVAL%", String(rpm_update_interval));
  html.replace("%DURATION%", String(rpm_flag_duration));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("thresh")) rpm_diff_threshold = server.arg("thresh").toInt();
  if (server.hasArg("interval")) rpm_update_interval = server.arg("interval").toInt();
  if (server.hasArg("duration")) rpm_flag_duration = server.arg("duration").toInt();

  prefs.begin("calib", false);
  prefs.putInt("rpm_thresh", rpm_diff_threshold);
  prefs.putInt("update_int", rpm_update_interval);
  prefs.putInt("flag_duration", rpm_flag_duration);
  prefs.end();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWifi() {
  pauseMeasurement();
  String html = html_wifi;
  html.replace("%SSID%", ap_ssid);
  html.replace("%PASS%", ap_pass);
  html.replace("%CHAN%", String(ap_channel));
  html.replace("%MAC%", ap_mac);
  html.replace("%BW20%", ap_bw == 20 ? "selected" : "");
  html.replace("%BW40%", ap_bw == 40 ? "selected" : "");
  server.send(200, "text/html", html);
}

void handleWifiSave() {
  if (server.hasArg("ssid")) ap_ssid = server.arg("ssid");
  if (server.hasArg("pass")) ap_pass = server.arg("pass");
  if (server.hasArg("chan")) ap_channel = server.arg("chan").toInt();
  if (server.hasArg("mac")) ap_mac = server.arg("mac");
  if (server.hasArg("bw")) ap_bw = server.arg("bw").toInt();

  prefs.begin("calib", false);
  prefs.putString("ap_ssid", ap_ssid);
  prefs.putString("ap_pass", ap_pass);
  prefs.putInt("ap_chan", ap_channel);
  prefs.putString("ap_mac", ap_mac);
  prefs.putInt("ap_bw", ap_bw);
  prefs.end();

  server.sendHeader("Location", "/");
  server.send(303);
  delay(500);
  ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) Update.begin();
  else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
  else if (up.status == UPLOAD_FILE_END) {
    if (Update.end()) Serial.println("✅ OTA Complete");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(WHEEL1_PIN, INPUT_PULLUP);
  pinMode(WHEEL2_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WHEEL1_PIN), isrWheel1, RISING);
  attachInterrupt(digitalPinToInterrupt(WHEEL2_PIN), isrWheel2, RISING);

  prefs.begin("calib", true);
  ap_ssid = prefs.getString("ap_ssid", "ESP-RPM");
  ap_pass = prefs.getString("ap_pass", "12345678");
  ap_channel = prefs.getInt("ap_chan", 6);
  ap_mac = prefs.getString("ap_mac", "random");
  ap_bw = prefs.getInt("ap_bw", 20);
  rpm_diff_threshold = prefs.getInt("rpm_thresh", 50);
  rpm_update_interval = prefs.getInt("update_int", 400);
  rpm_flag_duration = prefs.getInt("flag_duration", 100);
  prefs.end();

  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP);
  IPAddress local_ip(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str(), ap_channel);
  esp_wifi_set_bandwidth(WIFI_IF_AP, ap_bw == 40 ? WIFI_BW_HT40 : WIFI_BW_HT20);

  if (ap_mac != "random" && ap_mac.length() == 12) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
      mac[i] = strtoul(ap_mac.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    esp_wifi_set_mac(WIFI_IF_AP, mac);
  }

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi", handleWifi);
  server.on("/wifi-save", HTTP_POST, handleWifiSave);
  server.on("/update", HTTP_GET, []() {
    pauseMeasurement();
    server.send(200, "text/html",
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><input type='submit' value='Update'></form><br><a href='/'>← Back</a>");
  });
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    ESP.restart();
  }, handleUpdateUpload);
  server.on("/events", []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/event-stream");
  });

  server.begin();
  Serial.println("🚀 System Ready. Connect to: http://192.168.4.1");
}

void loop() {
  server.handleClient();
  if (!active_mode) return;

  rpm1 = delta1 > 0 ? 60000000.0 / delta1 : 0;
  rpm2 = delta2 > 0 ? 60000000.0 / delta2 : 0;
  rpm_diff = fabs(rpm1 - rpm2);

  if (rpm_diff > rpm_diff_threshold) {
    if (!flag) {
      flag = true;
      flag_raised_time = millis();
    }
    digitalWrite(LED_PIN, LOW);
    led_warning_active = true;
  } else {
    flag = false;
    flag_raised_time = 0;
    digitalWrite(LED_PIN, HIGH);
    led_warning_active = false;
  }

  if (flag && millis() - flag_raised_time >= rpm_flag_duration) {
    take_action();
    flag_raised_time = millis();
  }

  unsigned long now = millis();
  if (!led_warning_active) {
    if (now - last_heartbeat_time >= 3000) {
      digitalWrite(LED_PIN, LOW);
      heartbeat_on_time = now;
      last_heartbeat_time = now;
      led_heartbeat_active = true;
    }
    if (led_heartbeat_active && now - heartbeat_on_time >= 40) {
      digitalWrite(LED_PIN, HIGH);
      led_heartbeat_active = false;
    }
  }

  static unsigned long lastUpdate = 0;
  if (now - lastUpdate >= rpm_update_interval) {
    Serial.printf("RPM1: %.2f | RPM2: %.2f | Δ: %.2f\n", rpm1, rpm2, rpm_diff);
    server.sendContent("data: {\"rpm1\":" + String(rpm1) +
                       ", \"rpm2\":" + String(rpm2) +
                       ", \"diff\":" + String(rpm_diff) + "}\n\n");
    lastUpdate = now;
  }
}
