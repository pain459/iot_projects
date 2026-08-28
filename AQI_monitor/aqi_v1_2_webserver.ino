#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cSps30.h>

// ============================================================
// Wi-Fi Configuration
// ============================================================

const char* WIFI_SSID     = "Pirates_IoT";
const char* WIFI_PASSWORD = "<password>";

// ============================================================
// NTP / Time Configuration
// ============================================================

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

// India Standard Time: UTC +05:30
const long GMT_OFFSET_SEC = 5 * 60 * 60;
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================
// ESP32-S3 I2C Pins
// ============================================================

#define I2C_SDA 8
#define I2C_SCL 9

// ============================================================
// OLED
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// ============================================================
// SPS30
// ============================================================

SensirionI2cSps30 sps30;

// SPS30 firmware
uint8_t fwMajor = 0;
uint8_t fwMinor = 0;

// ============================================================
// Web Server
// ============================================================

WebServer server(80);

// ============================================================
// Display
// ============================================================

#define SCREEN_ROTATION_INTERVAL 5000

unsigned long lastScreenChange = 0;

int currentScreen = 0;

// ============================================================
// SPS30 Measurements
// ============================================================

float pm1p0 = 0.0;
float pm2p5 = 0.0;
float pm4p0 = 0.0;
float pm10p0 = 0.0;

float nc0p5 = 0.0;
float nc1p0 = 0.0;
float nc2p5 = 0.0;
float nc4p0 = 0.0;
float nc10p0 = 0.0;

float typicalSize = 0.0;

// ============================================================
// Sensor state
// ============================================================

bool sps30Online = false;

unsigned long lastMeasurementMillis = 0;

// ============================================================
// Function declarations
// ============================================================

void connectWiFi();
void syncTime();

void updateSPS30();

void updateDisplay();
void drawAirQualityScreen();
void drawClockScreen();
void drawParticleScreen();

void startWebServer();

void handleRoot();
void handleApiStatus();
void handleNotFound();

String getCurrentTime();
String getUptime();

void printSystemStatus();

// ============================================================
// SETUP
// ============================================================

void setup() {

  // ----------------------------------------------------------
  // Serial
  // ----------------------------------------------------------

  Serial.begin(115200);

  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("      ESP32-S3 AIR QUALITY MONITOR");
  Serial.println("                 V1.2");
  Serial.println("========================================");
  Serial.println();

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  Serial.println("I2C initialized.");

  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDRESS
      )) {

    Serial.println(
      "ERROR: SSD1306 initialization failed."
    );

    while (1) {
      delay(100);
    }
  }

  Serial.println("OLED initialized.");

  // Boot screen

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 5);
  display.println("AIR QUALITY MONITOR");

  display.setCursor(0, 20);
  display.println("V1.2");

  display.setCursor(0, 35);
  display.println("Initializing...");

  display.display();

  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  connectWiFi();

  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
  }

  // ----------------------------------------------------------
  // SPS30
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("Initializing SPS30...");

  sps30.begin(
    Wire,
    0x69
  );

  // ----------------------------------------------------------
  // Probe SPS30
  // ----------------------------------------------------------

  int16_t error = sps30.readFirmwareVersion(
    fwMajor,
    fwMinor
  );

  if (error) {

    Serial.print(
      "ERROR: SPS30 probe failed: "
    );

    Serial.println(error);

    display.clearDisplay();

    display.setTextSize(1);

    display.setCursor(0, 5);
    display.println("SPS30 ERROR!");

    display.setCursor(0, 20);
    display.println("Sensor not found.");

    display.setCursor(0, 35);
    display.println("Check wiring.");

    display.display();

    while (1) {
      delay(100);
    }
  }

  sps30Online = true;

  Serial.println("SPS30 detected!");

  Serial.print("Firmware: ");
  Serial.print(fwMajor);
  Serial.print(".");
  Serial.println(fwMinor);

  // ----------------------------------------------------------
  // Start measurement
  // ----------------------------------------------------------

  error = sps30.startMeasurement(
    SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT
  );

  if (error) {

    Serial.print(
      "ERROR: SPS30 start failed: "
    );

    Serial.println(error);

    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(0, 10);

    display.println(
      "SPS30 START ERROR!"
    );

    display.display();

    while (1) {
      delay(100);
    }
  }

  Serial.println(
    "SPS30 measurement started."
  );

  // Allow sensor fan to stabilize
  delay(1000);

  // ----------------------------------------------------------
  // Web server
  // ----------------------------------------------------------

  startWebServer();

  // ----------------------------------------------------------
  // Initial display
  // ----------------------------------------------------------

  currentScreen = 0;

  lastScreenChange = millis();

  updateDisplay();

  // ----------------------------------------------------------
  // System status
  // ----------------------------------------------------------

  printSystemStatus();

  Serial.println();
  Serial.println("========================================");
  Serial.println("V1.2 MONITOR STARTED");
  Serial.println("========================================");
  Serial.println();
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // SPS30
  // ----------------------------------------------------------

  updateSPS30();

  // ----------------------------------------------------------
  // Web server
  // ----------------------------------------------------------

  server.handleClient();

  // ----------------------------------------------------------
  // OLED screen rotation
  // ----------------------------------------------------------

  if (
    millis() - lastScreenChange >=
    SCREEN_ROTATION_INTERVAL
  ) {

    currentScreen++;

    if (currentScreen > 2) {
      currentScreen = 0;
    }

    lastScreenChange = millis();

    updateDisplay();
  }

  delay(10);
}

// ============================================================
// Wi-Fi
// ============================================================

void connectWiFi() {

  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  int attempts = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    attempts < 30
  ) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "Wi-Fi connected."
    );

    Serial.print(
      "IP address: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );

  } else {

    Serial.println(
      "WARNING: Wi-Fi connection failed."
    );
  }
}

// ============================================================
// NTP
// ============================================================

void syncTime() {

  Serial.println(
    "Synchronizing time..."
  );

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    NTP_SERVER_1,
    NTP_SERVER_2
  );

  struct tm timeinfo;

  int attempts = 0;

  while (
    !getLocalTime(&timeinfo) &&
    attempts < 20
  ) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (attempts < 20) {

    Serial.println(
      "Time synchronized."
    );

  } else {

    Serial.println(
      "WARNING: NTP synchronization failed."
    );
  }
}

// ============================================================
// SPS30
// ============================================================

void updateSPS30() {

  uint16_t dataReady = 0;

  int16_t error =
    sps30.readDataReadyFlag(
      dataReady
    );

  if (error) {

    Serial.print(
      "ERROR: Data-ready flag: "
    );

    Serial.println(error);

    return;
  }

  if (!dataReady) {
    return;
  }

  // ----------------------------------------------------------
  // Read floating-point values
  // ----------------------------------------------------------

  error =
    sps30.readMeasurementValuesFloat(
      pm1p0,
      pm2p5,
      pm4p0,
      pm10p0,
      nc0p5,
      nc1p0,
      nc2p5,
      nc4p0,
      nc10p0,
      typicalSize
    );

  if (error) {

    Serial.print(
      "ERROR: SPS30 measurement: "
    );

    Serial.println(error);

    return;
  }

  lastMeasurementMillis = millis();

  // ----------------------------------------------------------
  // Serial
  // ----------------------------------------------------------

  Serial.println(
    "----------------------------------------"
  );

  Serial.print("PM1.0  : ");
  Serial.print(pm1p0, 1);
  Serial.println(" ug/m3");

  Serial.print("PM2.5  : ");
  Serial.print(pm2p5, 1);
  Serial.println(" ug/m3");

  Serial.print("PM4.0  : ");
  Serial.print(pm4p0, 1);
  Serial.println(" ug/m3");

  Serial.print("PM10   : ");
  Serial.print(pm10p0, 1);
  Serial.println(" ug/m3");

  Serial.print("NC0.5  : ");
  Serial.println(nc0p5, 1);

  Serial.print("NC1.0  : ");
  Serial.println(nc1p0, 1);

  Serial.print("NC2.5  : ");
  Serial.println(nc2p5, 1);

  Serial.print("NC4.0  : ");
  Serial.println(nc4p0, 1);

  Serial.print("NC10   : ");
  Serial.println(nc10p0, 1);

  Serial.print("Particle size: ");
  Serial.print(typicalSize, 2);
  Serial.println(" um");

  // Refresh OLED
  updateDisplay();
}

// ============================================================
// WEB SERVER
// ============================================================

void startWebServer() {

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  server.on(
    "/api/status",
    HTTP_GET,
    handleApiStatus
  );

  server.onNotFound(
    handleNotFound
  );

  server.begin();

  Serial.println(
    "Web server started."
  );

  Serial.print(
    "Dashboard: http://"
  );

  Serial.print(
    WiFi.localIP()
  );

  Serial.println("/");
}

// ============================================================
// WEB DASHBOARD
// ============================================================

void handleRoot() {

  String html = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
      content="width=device-width,
               initial-scale=1.0">

<title>Air Quality Monitor</title>

<style>

* {
  box-sizing: border-box;
}

body {

  margin: 0;

  font-family:
    system-ui,
    -apple-system,
    BlinkMacSystemFont,
    "Segoe UI",
    sans-serif;

  background: #f2f4f7;

  color: #202124;
}

.container {

  max-width: 900px;

  margin: auto;

  padding: 16px;
}

.header {

  background: white;

  border-radius: 16px;

  padding: 20px;

  margin-bottom: 16px;

  box-shadow:
    0 2px 8px rgba(0,0,0,0.08);
}

.header h1 {

  margin: 0;

  font-size: 24px;
}

.header p {

  margin: 6px 0 0;

  color: #666;
}

.main-card {

  background: white;

  border-radius: 16px;

  padding: 25px;

  text-align: center;

  margin-bottom: 16px;

  box-shadow:
    0 2px 8px rgba(0,0,0,0.08);
}

.main-label {

  font-size: 16px;

  color: #666;
}

.pm25 {

  font-size: 64px;

  font-weight: 700;

  margin: 5px 0;
}

.unit {

  color: #666;
}

.grid {

  display: grid;

  grid-template-columns:
    repeat(3, 1fr);

  gap: 12px;

  margin-bottom: 16px;
}

.card {

  background: white;

  border-radius: 14px;

  padding: 18px;

  box-shadow:
    0 2px 8px rgba(0,0,0,0.08);
}

.card-title {

  font-size: 14px;

  color: #666;

  margin-bottom: 5px;
}

.value {

  font-size: 26px;

  font-weight: 600;
}

.section {

  background: white;

  border-radius: 16px;

  padding: 20px;

  margin-bottom: 16px;

  box-shadow:
    0 2px 8px rgba(0,0,0,0.08);
}

.section h2 {

  margin-top: 0;

  font-size: 18px;
}

.row {

  display: flex;

  justify-content: space-between;

  padding: 8px 0;

  border-bottom: 1px solid #eee;
}

.row:last-child {

  border-bottom: none;
}

.label {

  color: #666;
}

.status {

  display: inline-block;

  padding: 4px 9px;

  border-radius: 20px;

  background: #e6f4ea;

  color: #137333;

  font-size: 13px;
}

@media(max-width:600px) {

  .grid {

    grid-template-columns: 1fr;
  }

  .pm25 {

    font-size: 52px;
  }
}

</style>

</head>

<body>

<div class="container">

  <div class="header">

    <h1>🏠 Air Quality Monitor</h1>

    <p id="datetime">
      Loading time...
    </p>

  </div>

  <div class="main-card">

    <div class="main-label">
      PM2.5
    </div>

    <div class="pm25"
         id="pm25">
      --
    </div>

    <div class="unit">
      µg/m³
    </div>

  </div>

  <div class="grid">

    <div class="card">

      <div class="card-title">
        PM1.0
      </div>

      <div class="value"
           id="pm10">
        --
      </div>

      <div class="unit">
        µg/m³
      </div>

    </div>

    <div class="card">

      <div class="card-title">
        PM4.0
      </div>

      <div class="value"
           id="pm40">
        --
      </div>

      <div class="unit">
        µg/m³
      </div>

    </div>

    <div class="card">

      <div class="card-title">
        PM10
      </div>

      <div class="value"
           id="pm100">
        --
      </div>

      <div class="unit">
        µg/m³
      </div>

    </div>

  </div>

  <div class="section">

    <h2>Particle Counts</h2>

    <div class="row">
      <span class="label">NC0.5</span>
      <span id="nc05">--</span>
    </div>

    <div class="row">
      <span class="label">NC1.0</span>
      <span id="nc10">--</span>
    </div>

    <div class="row">
      <span class="label">NC2.5</span>
      <span id="nc25">--</span>
    </div>

    <div class="row">
      <span class="label">NC4.0</span>
      <span id="nc40">--</span>
    </div>

    <div class="row">
      <span class="label">NC10</span>
      <span id="nc100">--</span>
    </div>

  </div>

  <div class="section">

    <h2>Sensor</h2>

    <div class="row">
      <span class="label">
        Typical particle size
      </span>

      <span id="size">--</span>
    </div>

    <div class="row">
      <span class="label">
        Firmware
      </span>

      <span id="firmware">--</span>
    </div>

    <div class="row">
      <span class="label">
        Status
      </span>

      <span id="sensorStatus"
            class="status">
        --
      </span>
    </div>

  </div>

  <div class="section">

    <h2>Device</h2>

    <div class="row">
      <span class="label">
        Wi-Fi
      </span>

      <span id="wifi">--</span>
    </div>

    <div class="row">
      <span class="label">
        Signal
      </span>

      <span id="rssi">--</span>
    </div>

    <div class="row">
      <span class="label">
        Uptime
      </span>

      <span id="uptime">--</span>
    </div>

  </div>

</div>

<script>

async function updateData() {

  try {

    const response =
      await fetch('/api/status');

    const data =
      await response.json();

    document.getElementById('pm25')
      .textContent =
      data.pm2_5.toFixed(1);

    document.getElementById('pm10')
      .textContent =
      data.pm1_0.toFixed(1);

    document.getElementById('pm40')
      .textContent =
      data.pm4_0.toFixed(1);

    document.getElementById('pm100')
      .textContent =
      data.pm10.toFixed(1);

    document.getElementById('nc05')
      .textContent =
      data.nc0_5.toFixed(1);

    document.getElementById('nc10')
      .textContent =
      data.nc1_0.toFixed(1);

    document.getElementById('nc25')
      .textContent =
      data.nc2_5.toFixed(1);

    document.getElementById('nc40')
      .textContent =
      data.nc4_0.toFixed(1);

    document.getElementById('nc100')
      .textContent =
      data.nc10.toFixed(1);

    document.getElementById('size')
      .textContent =
      data.typical_size.toFixed(2)
      + ' µm';

    document.getElementById('firmware')
      .textContent =
      data.firmware;

    document.getElementById('wifi')
      .textContent =
      data.wifi;

    document.getElementById('rssi')
      .textContent =
      data.rssi + ' dBm';

    document.getElementById('uptime')
      .textContent =
      data.uptime;

    document.getElementById('datetime')
      .textContent =
      data.datetime;

    document.getElementById('sensorStatus')
      .textContent =
      data.sps30_online
      ? 'Online'
      : 'Offline';

  }

  catch (error) {

    console.log(
      'Update failed:',
      error
    );

  }
}

// Initial update
updateData();

// Update every 2 seconds
setInterval(
  updateData,
  2000
);

</script>

</body>

</html>

)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// JSON API
// ============================================================

void handleApiStatus() {

  String json = "{";

  json += "\"pm1_0\":";
  json += String(pm1p0, 2);

  json += ",\"pm2_5\":";
  json += String(pm2p5, 2);

  json += ",\"pm4_0\":";
  json += String(pm4p0, 2);

  json += ",\"pm10\":";
  json += String(pm10p0, 2);

  json += ",\"nc0_5\":";
  json += String(nc0p5, 2);

  json += ",\"nc1_0\":";
  json += String(nc1p0, 2);

  json += ",\"nc2_5\":";
  json += String(nc2p5, 2);

  json += ",\"nc4_0\":";
  json += String(nc4p0, 2);

  json += ",\"nc10\":";
  json += String(nc10p0, 2);

  json += ",\"typical_size\":";
  json += String(typicalSize, 3);

  json += ",\"sps30_online\":";
  json += sps30Online
          ? "true"
          : "false";

  json += ",\"firmware\":\"";
  json += String(fwMajor);
  json += ".";
  json += String(fwMinor);
  json += "\"";

  json += ",\"rssi\":";
  json += String(WiFi.RSSI());

  json += ",\"wifi\":\"";

  if (WiFi.status() == WL_CONNECTED) {
    json += "Connected";
  } else {
    json += "Disconnected";
  }

  json += "\"";

  json += ",\"uptime\":\"";
  json += getUptime();
  json += "\"";

  json += ",\"datetime\":\"";
  json += getCurrentTime();
  json += "\"";

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// 404
// ============================================================

void handleNotFound() {

  server.send(
    404,
    "text/plain",
    "Not Found"
  );
}

// ============================================================
// Current date/time
// ============================================================

String getCurrentTime() {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "Time not synchronized";
  }

  char buffer[64];

  strftime(
    buffer,
    sizeof(buffer),
    "%a %d %b %Y  %H:%M:%S",
    &timeinfo
  );

  return String(buffer);
}

// ============================================================
// Uptime
// ============================================================

String getUptime() {

  unsigned long seconds =
    millis() / 1000;

  unsigned long days =
    seconds / 86400;

  seconds %= 86400;

  unsigned long hours =
    seconds / 3600;

  seconds %= 3600;

  unsigned long minutes =
    seconds / 60;

  seconds %= 60;

  char buffer[32];

  if (days > 0) {

    snprintf(
      buffer,
      sizeof(buffer),
      "%lud %02lu:%02lu:%02lu",
      days,
      hours,
      minutes,
      seconds
    );

  } else {

    snprintf(
      buffer,
      sizeof(buffer),
      "%02lu:%02lu:%02lu",
      hours,
      minutes,
      seconds
    );
  }

  return String(buffer);
}

// ============================================================
// OLED DISPLAY CONTROLLER
// ============================================================

void updateDisplay() {

  switch (currentScreen) {

    case 0:
      drawAirQualityScreen();
      break;

    case 1:
      drawClockScreen();
      break;

    case 2:
      drawParticleScreen();
      break;
  }

  display.display();
}

// ============================================================
// OLED SCREEN 1
// ============================================================

void drawAirQualityScreen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0, 0);

  display.println("AIR QUALITY");

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH - 1,
    10,
    SSD1306_WHITE
  );

  display.setTextSize(2);

  display.setCursor(0, 16);

  display.print("PM2.5 ");

  display.print(
    pm2p5,
    1
  );

  display.setTextSize(1);

  display.setCursor(0, 40);

  display.print("PM1.0 : ");

  display.print(
    pm1p0,
    1
  );

  display.println(
    " ug/m3"
  );

  display.setCursor(0, 52);

  display.print("PM10  : ");

  display.print(
    pm10p0,
    1
  );

  display.println(
    " ug/m3"
  );
}

// ============================================================
// OLED SCREEN 2
// ============================================================

void drawClockScreen() {

  display.clearDisplay();

  struct tm timeinfo;

  display.setTextSize(1);

  display.setCursor(0, 0);

  if (getLocalTime(&timeinfo)) {

    char dateBuffer[20];

    strftime(
      dateBuffer,
      sizeof(dateBuffer),
      "%a %d %b",
      &timeinfo
    );

    display.print(
      dateBuffer
    );

    display.setTextSize(2);

    display.setCursor(68, 0);

    char timeBuffer[10];

    strftime(
      timeBuffer,
      sizeof(timeBuffer),
      "%H:%M",
      &timeinfo
    );

    display.print(
      timeBuffer
    );

  } else {

    display.setTextSize(1);

    display.setCursor(0, 4);

    display.print(
      "TIME NOT SYNCED"
    );
  }

  display.drawLine(
    0,
    17,
    SCREEN_WIDTH - 1,
    17,
    SSD1306_WHITE
  );

  display.setTextSize(1);

  display.setCursor(0, 21);

  display.print("PM2.5 ");

  display.print(
    pm2p5,
    1
  );

  display.print(
    " ug/m3"
  );

  display.setCursor(0, 32);

  display.print("PM1.0 ");

  display.print(
    pm1p0,
    1
  );

  display.print(
    " ug/m3"
  );

  display.setCursor(0, 43);

  display.print("PM10  ");

  display.print(
    pm10p0,
    1
  );

  display.print(
    " ug/m3"
  );

  display.setCursor(0, 54);

  display.print("Size  ");

  display.print(
    typicalSize,
    2
  );

  display.print(
    " um"
  );
}

// ============================================================
// OLED SCREEN 3
// ============================================================

void drawParticleScreen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0, 0);

  display.println(
    "PARTICLE COUNTS"
  );

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH - 1,
    10,
    SSD1306_WHITE
  );

  display.setCursor(0, 16);

  display.print("NC0.5 : ");

  display.println(
    nc0p5,
    1
  );

  display.setCursor(0, 28);

  display.print("NC1.0 : ");

  display.println(
    nc1p0,
    1
  );

  display.setCursor(0, 40);

  display.print("NC2.5 : ");

  display.println(
    nc2p5,
    1
  );

  display.setCursor(0, 52);

  display.print("NC10  : ");

  display.println(
    nc10p0,
    1
  );
}

// ============================================================
// System status
// ============================================================

void printSystemStatus() {

  Serial.println();
  Serial.println("SYSTEM STATUS");
  Serial.println("-------------");

  Serial.print("Wi-Fi: ");

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "Connected"
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );

  } else {

    Serial.println(
      "Disconnected"
    );
  }

  Serial.print(
    "SPS30: "
  );

  Serial.println(
    sps30Online
    ? "Online"
    : "Offline"
  );

  Serial.print(
    "Firmware: "
  );

  Serial.print(
    fwMajor
  );

  Serial.print(
    "."
  );

  Serial.println(
    fwMinor
  );

  Serial.print(
    "Dashboard: http://"
  );

  Serial.print(
    WiFi.localIP()
  );

  Serial.println(
    "/"
  );

  Serial.print(
    "API: http://"
  );

  Serial.print(
    WiFi.localIP()
  );

  Serial.println(
    "/api/status"
  );
}