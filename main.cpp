#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>


#define WHEEL1_PIN 0
#define WHEEL2_PIN 1
#define LED_PIN    8

// AP credentials
const char* ssid = "ESP-RPM";
const char* password = "12345678";

// RPM timing vars
volatile unsigned long lastPulse1 = 0, lastPulse2 = 0;
volatile unsigned long delta1 = 0, delta2 = 0;

// Computed RPMs
float rpm1 = 0.0, rpm2 = 0.0, rpm_diff = 0.0;

// EEPROM-calibrated setting
int rpm_diff_threshold = 50;

// LED state vars
bool led_warning_active = false;
unsigned long last_heartbeat_time = 0;
unsigned long heartbeat_on_time = 0;
bool led_heartbeat_active = false;

Preferences prefs;
WebServer server(80);

// === ISR functions ===
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

// === SSE Handler ===
void handleEvents() {
  rpm1 = delta1 > 0 ? 60000000.0 / delta1 : 0;
  rpm2 = delta2 > 0 ? 60000000.0 / delta2 : 0;
  rpm_diff = fabs(rpm1 - rpm2);

  String msg = "data: {\"rpm1\":" + String(rpm1, 2) +
               ", \"rpm2\":" + String(rpm2, 2) +
               ", \"diff\":" + String(rpm_diff, 2) + "}\n\n";
  server.sendContent(msg);
}

// === HTML pages ===
const char html_index[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>RPM Monitor</title>
<style>body{text-align:center;font-family:sans-serif}.data{font-size:2em}</style></head>
<body><h2>ESP32 RPM Monitor</h2>
<div class="data" id="rpm1">Wheel 1: ...</div>
<div class="data" id="rpm2">Wheel 2: ...</div>
<div class="data" id="diff">Difference: ...</div>
<br><a href="/config">⚙️ Config</a>
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
<style>body{text-align:center;font-family:sans-serif}</style></head>
<body><h2>Calibration</h2>
<form method="POST" action="/save">
  <label>RPM Diff Threshold:</label><br>
  <input type="number" name="thresh" value="%THRESH%" min="0"><br><br>
  <button type="submit">Save</button>
</form>
<br><a href="/">← Back</a>
</body></html>
)rawliteral";

// === Web routes ===
void handleRoot() { server.send_P(200, "text/html", html_index); }

void handleConfig() {
  String html = html_config;
  html.replace("%THRESH%", String(rpm_diff_threshold));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("thresh")) {
    rpm_diff_threshold = server.arg("thresh").toInt();
    prefs.begin("calib", false);
    prefs.putInt("rpm_thresh", rpm_diff_threshold);
    prefs.end();
    Serial.printf("[CALIB] Saved threshold: %d\n", rpm_diff_threshold);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Update.begin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end()) Serial.println("Update complete");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(WHEEL1_PIN, INPUT_PULLUP);
  pinMode(WHEEL2_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // LED OFF (active LOW)

  attachInterrupt(digitalPinToInterrupt(WHEEL1_PIN), isrWheel1, RISING);
  attachInterrupt(digitalPinToInterrupt(WHEEL2_PIN), isrWheel2, RISING);

  WiFi.softAP(ssid, password);
  Serial.println("AP started. Connect to http://192.168.4.1");

  prefs.begin("calib", true);
  rpm_diff_threshold = prefs.getInt("rpm_thresh", 50);
  prefs.end();

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/events", []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/event-stream");
  });
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><input type='submit' value='Update'></form>");
  });
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    ESP.restart();
  }, handleUpdateUpload);
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // Heartbeat every 3 sec
  if (!led_warning_active) {
    if (now - last_heartbeat_time >= 3000) {
      digitalWrite(LED_PIN, LOW);
      heartbeat_on_time = now;
      led_heartbeat_active = true;
      last_heartbeat_time = now;
    }
    if (led_heartbeat_active && now - heartbeat_on_time >= 40) {
      digitalWrite(LED_PIN, HIGH);
      led_heartbeat_active = false;
    }
  }

  // Update RPM + LED every ~150ms
  static unsigned long lastUpdate = 0;
  if (now - lastUpdate >= 150) {
    rpm1 = delta1 > 0 ? 60000000.0 / delta1 : 0;
    rpm2 = delta2 > 0 ? 60000000.0 / delta2 : 0;
    rpm_diff = fabs(rpm1 - rpm2);

    if (rpm_diff > rpm_diff_threshold) {
      digitalWrite(LED_PIN, LOW); // ON
      led_warning_active = true;
    } else {
      digitalWrite(LED_PIN, HIGH); // OFF
      led_warning_active = false;
    }

    handleEvents();
    lastUpdate = now;
  }
}
