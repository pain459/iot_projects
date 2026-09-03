/*
  ESP32-S3 Air Quality Monitor — V1.5

  Migration from V1.4.2:
    - Replaces SSD1306 128x64 I2C OLED with 2.8" ILI9341 240x320 SPI TFT
    - TFT is used in landscape mode: 320x240
    - SPS30, Wi-Fi, mDNS, NTP, web dashboard, Telegram,
      60-minute history, LittleFS persistence and alerts are retained

  TFT wiring used by this sketch:
    TFT CS    -> GPIO 10
    TFT MOSI  -> GPIO 11
    TFT SCK   -> GPIO 12
    TFT MISO  -> GPIO 13
    TFT DC/RS -> GPIO 14
    TFT RST   -> GPIO 15

  SPS30 I2C wiring:
    SDA -> GPIO 8
    SCL -> GPIO 9

  Libraries:
    - Adafruit GFX Library
    - Adafruit ILI9341
    - Sensirion I2C SPS30
    - UniversalTelegramBot
    - ArduinoJson

  Notes:
    - TFT VCC/backlight wiring is handled in hardware.
    - This V1.5 keeps the four-screen carousel at 5 seconds.
    - The TFT is redrawn on screen changes rather than every sensor sample,
      avoiding full-screen SPI flicker while measurements continue normally.
*/

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SensirionI2cSps30.h>

// ============================================================
// ESP32-S3 AIR QUALITY MONITOR V1.5
// ============================================================

// ------------------------------------------------------------
// WIFI / NETWORK
// ------------------------------------------------------------

const char* WIFI_SSID = "Pirates_IoT";
const char* WIFI_PASSWORD = "<password>";

const char* MDNS_HOSTNAME = "airmonitor";

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = (5 * 60 * 60) + (30 * 60);
const int DAYLIGHT_OFFSET_SEC = 0;

// ------------------------------------------------------------
// TELEGRAM
// Replace both values. Leave defaults to disable Telegram.
// ------------------------------------------------------------

const char* TELEGRAM_BOT_TOKEN = "<token>";
const char* TELEGRAM_CHAT_ID = "<id>>";

WiFiClientSecure telegramClient;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, telegramClient);

// ------------------------------------------------------------
// WEB SERVER / SPS30
// ------------------------------------------------------------

WebServer server(80);
SensirionI2cSps30 sps30;

// ------------------------------------------------------------
// I2C — SPS30
// ------------------------------------------------------------

#define I2C_SDA 8
#define I2C_SCL 9

// ------------------------------------------------------------
// SPI TFT — ILI9341
// ------------------------------------------------------------

#define TFT_CS    10
#define TFT_MOSI  11
#define TFT_SCK   12
#define TFT_MISO  13
#define TFT_DC    14
#define TFT_RST   15

Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, TFT_RST);

// ------------------------------------------------------------
// CURRENT SPS30 READINGS
// ------------------------------------------------------------

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

uint8_t fwMajor = 0;
uint8_t fwMinor = 0;

bool sps30Online = false;

// ------------------------------------------------------------
// TIMING
// ------------------------------------------------------------

unsigned long lastMeasurementMillis = 0;
unsigned long lastHistorySample = 0;
unsigned long lastScreenChange = 0;

#define HISTORY_INTERVAL 30000UL
#define SCREEN_ROTATION_INTERVAL 5000UL

int currentScreen = 0;

// ------------------------------------------------------------
// HISTORY
// 30-second samples, 120 samples = 60 minutes
// ------------------------------------------------------------

#define HISTORY_SIZE 120

float pm25History[HISTORY_SIZE];
float pm10History[HISTORY_SIZE];

unsigned long historyTime[HISTORY_SIZE];
uint32_t historyEpoch[HISTORY_SIZE];

int historyCount = 0;
int historyWriteIndex = 0;

// ------------------------------------------------------------
// PERSISTENT HISTORY
// ------------------------------------------------------------

const char* HISTORY_FILE = "/history.dat";
const char* HISTORY_TMP  = "/history.tmp";

#define HISTORY_MAGIC 0x41495131UL
#define HISTORY_VERSION 1
#define HISTORY_SAVE_INTERVAL (5UL * 60UL * 1000UL)

unsigned long lastHistorySave = 0;
bool historyDirty = false;

struct HistoryFile {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint16_t count;
  uint16_t writeIndex;

  float pm25[HISTORY_SIZE];
  float pm10[HISTORY_SIZE];
  uint32_t epoch[HISTORY_SIZE];

  uint32_t checksum;
};

// ------------------------------------------------------------
// TELEGRAM / ALERTS
// ------------------------------------------------------------

unsigned long lastTelegramPoll = 0;
#define TELEGRAM_POLL_INTERVAL 5000UL

#define NTP_RETRY_INTERVAL 60000UL
unsigned long lastNtpRetry = 0;
bool timeSynchronized = false;

bool alertsEnabled = true;

float pm25AlertThreshold = 35.0;
float pm10AlertThreshold = 50.0;

bool pm25AlertActive = false;
bool pm10AlertActive = false;

unsigned long lastAlertCheck = 0;
#define ALERT_CHECK_INTERVAL 10000UL

unsigned long lastPM25AlertSent = 0;
unsigned long lastPM10AlertSent = 0;

#define ALERT_COOLDOWN (30UL * 60UL * 1000UL)

// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

bool telegramConfigured();

void connectWiFi();
void syncTime();

void updateSPS30();
void addHistorySample();

void maybeSaveHistory();
bool loadHistory();
bool saveHistory();
uint32_t checksumHistory(const HistoryFile& d);

void updateDisplay();
void drawAirQualityScreen();
void drawClockScreen();
void drawParticleScreen();
void drawSystemScreen();

void drawHeader(const char* title, uint16_t accent);
void drawMetricCard(int16_t x, int16_t y, int16_t w, int16_t h,
                    const char* label, float value, const char* unit,
                    uint16_t color);
void showFatalError(const char* title, const char* line1, const char* line2);

void startWebServer();
void handleRoot();
void handleApiStatus();
void handleApiHistory();
void handleNotFound();

void handleTelegram();
void handleTelegramCommand(String chatId, String command);
void checkAirQualityAlert();
void sendTelegramMessage(const String& message);

String getCurrentTime();
String getUptime();
String historyLabel(int index);

float getPM25Min();
float getPM25Max();
float getPM25Average();

float getPM10Min();
float getPM10Max();
float getPM10Average();

void printSystemStatus();

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("      ESP32-S3 AIR QUALITY MONITOR");
  Serial.println("                  V1.5");
  Serial.println("========================================");
  Serial.println();

  // ----------------------------------------------------------
  // I2C — SPS30
  // ----------------------------------------------------------

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("I2C initialized.");

  // ----------------------------------------------------------
  // SPI — ILI9341 TFT
  // ----------------------------------------------------------

  // SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1);

  tft.begin();
  tft.setRotation(1);          // 320 x 240 landscape
  tft.setTextWrap(false);
  tft.fillScreen(ILI9341_BLACK);

  Serial.println("ILI9341 TFT initialized.");

  // Boot screen
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, 320, 42, ILI9341_NAVY);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(3);
  tft.setCursor(24, 66);
  tft.print("AIR QUALITY");

  tft.setCursor(74, 106);
  tft.print("MONITOR");

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(130, 154);
  tft.print("V1.5");

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(80, 195);
  tft.print("Initializing...");

  // ----------------------------------------------------------
  // LITTLEFS / HISTORY
  // ----------------------------------------------------------

  if (LittleFS.begin(true)) {
    Serial.println("LittleFS initialized.");

    if (loadHistory()) {
      Serial.printf("Restored %d history samples.\n", historyCount);
    } else {
      Serial.println("No valid persistent history found.");
    }
  } else {
    Serial.println("WARNING: LittleFS unavailable.");
  }

  // ----------------------------------------------------------
  // WIFI / MDNS / NTP
  // ----------------------------------------------------------

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: http://%s.local\n", MDNS_HOSTNAME);
    } else {
      Serial.println("WARNING: mDNS failed.");
    }

    syncTime();
  }

  // ----------------------------------------------------------
  // TELEGRAM TLS
  // ----------------------------------------------------------

  telegramClient.setInsecure();
  telegramClient.setTimeout(1500);

  // ----------------------------------------------------------
  // SPS30
  // ----------------------------------------------------------

  Serial.println("Initializing SPS30...");

  sps30.begin(Wire, 0x69);

  int16_t error = sps30.readFirmwareVersion(fwMajor, fwMinor);

  if (error) {
    Serial.printf("ERROR: SPS30 probe failed: %d\n", error);
    showFatalError("SPS30 ERROR", "Sensor not found.", "Check wiring.");
    while (1) delay(100);
  }

  sps30Online = true;

  Serial.printf("SPS30 detected. Firmware %u.%u\n", fwMajor, fwMinor);

  error = sps30.startMeasurement(
    SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT
  );

  if (error) {
    Serial.printf("ERROR: SPS30 start failed: %d\n", error);
    showFatalError("SPS30 ERROR", "Measurement start failed.", "Check sensor.");
    while (1) delay(100);
  }

  delay(1000);

  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  startWebServer();

  // ----------------------------------------------------------
  // INITIAL TFT SCREEN
  // ----------------------------------------------------------

  currentScreen = 0;
  lastScreenChange = millis();
  lastHistorySample = millis();
  lastHistorySave = millis();

  updateDisplay();

  printSystemStatus();

  Serial.printf("Dashboard (mDNS): http://%s.local\n", MDNS_HOSTNAME);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(
      "Dashboard (IP):   http://%s/\n",
      WiFi.localIP().toString().c_str()
    );
  }
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  updateSPS30();

  // Keep HTTP servicing frequent.
  server.handleClient();

  // Retry NTP in background.
  if (
    WiFi.status() == WL_CONNECTED &&
    !timeSynchronized &&
    millis() - lastNtpRetry >= NTP_RETRY_INTERVAL
  ) {
    syncTime();
  }

  // Telegram polling.
  if (
    WiFi.status() == WL_CONNECTED &&
    millis() - lastTelegramPoll >= TELEGRAM_POLL_INTERVAL
  ) {
    lastTelegramPoll = millis();

    handleTelegram();

    // Service HTTP immediately after synchronous HTTPS work.
    server.handleClient();
  }

  // Alert evaluation.
  if (
    millis() - lastAlertCheck >= ALERT_CHECK_INTERVAL
  ) {
    lastAlertCheck = millis();
    checkAirQualityAlert();
  }

  maybeSaveHistory();

  // TFT carousel.
  if (
    millis() - lastScreenChange >= SCREEN_ROTATION_INTERVAL
  ) {
    currentScreen++;

    if (currentScreen > 3) {
      currentScreen = 0;
    }

    lastScreenChange = millis();
    updateDisplay();
  }

  delay(10);
}

// ============================================================
// WIFI
// ============================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);

  for (
    int i = 0;
    i < 30 && WiFi.status() != WL_CONNECTED;
    i++
  ) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(
      "Wi-Fi connected. IP=%s RSSI=%d dBm\n",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI()
    );
  } else {
    Serial.println("WARNING: Wi-Fi connection failed.");
  }
}

// ============================================================
// NTP
// ============================================================

void syncTime() {
  Serial.println("Starting NTP sync...");

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    NTP_SERVER_1,
    NTP_SERVER_2
  );

  struct tm ti;

  timeSynchronized = getLocalTime(&ti, 10000);

  if (timeSynchronized) {
    char buffer[40];

    strftime(
      buffer,
      sizeof(buffer),
      "%a %d %b %Y %H:%M:%S",
      &ti
    );

    Serial.print("Time synchronized: ");
    Serial.println(buffer);
  } else {
    Serial.println(
      "WARNING: NTP sync failed; will retry every 60 seconds."
    );
  }

  lastNtpRetry = millis();
}

// ============================================================
// TELEGRAM CONFIG
// ============================================================

bool telegramConfigured() {
  return
    String(TELEGRAM_BOT_TOKEN) != "YOUR_BOT_TOKEN" &&
    String(TELEGRAM_CHAT_ID) != "YOUR_CHAT_ID";
}

// ============================================================
// HISTORY CHECKSUM
// ============================================================

uint32_t checksumHistory(const HistoryFile& d) {
  const uint8_t* p =
    reinterpret_cast<const uint8_t*>(&d);

  size_t n =
    sizeof(HistoryFile) - sizeof(uint32_t);

  uint32_t h = 2166136261UL;

  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }

  return h;
}

// ============================================================
// LOAD HISTORY
// ============================================================

bool loadHistory() {
  if (!LittleFS.exists(HISTORY_FILE)) {
    return false;
  }

  File f = LittleFS.open(HISTORY_FILE, "r");

  if (!f || f.size() != sizeof(HistoryFile)) {
    if (f) f.close();
    return false;
  }

  HistoryFile d;

  size_t n =
    f.read(
      reinterpret_cast<uint8_t*>(&d),
      sizeof(d)
    );

  f.close();

  if (
    n != sizeof(d) ||
    d.magic != HISTORY_MAGIC ||
    d.version != HISTORY_VERSION ||
    d.size != HISTORY_SIZE ||
    d.count > HISTORY_SIZE ||
    d.writeIndex >= HISTORY_SIZE ||
    checksumHistory(d) != d.checksum
  ) {
    return false;
  }

  historyCount = d.count;
  historyWriteIndex = d.writeIndex;

  for (int i = 0; i < HISTORY_SIZE; i++) {
    pm25History[i] = d.pm25[i];
    pm10History[i] = d.pm10[i];
    historyEpoch[i] = d.epoch[i];

    // Used only as fallback when a restored sample has no valid epoch.
    historyTime[i] = millis();
  }

  historyDirty = false;

  return true;
}

// ============================================================
// SAVE HISTORY
// ============================================================

bool saveHistory() {
  HistoryFile d{};

  d.magic = HISTORY_MAGIC;
  d.version = HISTORY_VERSION;
  d.size = HISTORY_SIZE;
  d.count = historyCount;
  d.writeIndex = historyWriteIndex;

  for (int i = 0; i < HISTORY_SIZE; i++) {
    d.pm25[i] = pm25History[i];
    d.pm10[i] = pm10History[i];
    d.epoch[i] = historyEpoch[i];
  }

  d.checksum = checksumHistory(d);

  File f = LittleFS.open(HISTORY_TMP, "w");

  if (!f) {
    return false;
  }

  size_t n =
    f.write(
      reinterpret_cast<uint8_t*>(&d),
      sizeof(d)
    );

  f.flush();
  f.close();

  if (n != sizeof(d)) {
    LittleFS.remove(HISTORY_TMP);
    return false;
  }

  if (LittleFS.exists(HISTORY_FILE)) {
    LittleFS.remove(HISTORY_FILE);
  }

  if (!LittleFS.rename(HISTORY_TMP, HISTORY_FILE)) {
    return false;
  }

  historyDirty = false;
  lastHistorySave = millis();

  Serial.printf(
    "History checkpoint saved: %d samples.\n",
    historyCount
  );

  return true;
}

// ============================================================
// PERIODIC HISTORY CHECKPOINT
// ============================================================

void maybeSaveHistory() {
  if (
    historyDirty &&
    millis() - lastHistorySave >= HISTORY_SAVE_INTERVAL
  ) {
    saveHistory();
  }
}

// ============================================================
// SPS30 UPDATE
// ============================================================

void updateSPS30() {
  uint16_t ready = 0;

  int16_t error =
    sps30.readDataReadyFlag(ready);

  if (error || !ready) {
    return;
  }

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
    return;
  }

  lastMeasurementMillis = millis();

  if (
    millis() - lastHistorySample >= HISTORY_INTERVAL
  ) {
    addHistorySample();
    lastHistorySample = millis();
  }

  // Intentionally do not redraw the full TFT on every SPS30 sample.
  // The active screen refreshes when the carousel changes.
}

// ============================================================
// ADD HISTORY SAMPLE
// ============================================================

void addHistorySample() {
  pm25History[historyWriteIndex] = pm2p5;
  pm10History[historyWriteIndex] = pm10p0;
  historyTime[historyWriteIndex] = millis();

  time_t now;
  time(&now);

  historyEpoch[historyWriteIndex] =
    (now > 1577836800)
      ? static_cast<uint32_t>(now)
      : 0;

  historyWriteIndex =
    (historyWriteIndex + 1) % HISTORY_SIZE;

  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }

  historyDirty = true;

  // First persistent checkpoint immediately.
  // Later writes use the 5-minute interval.
  if (!LittleFS.exists(HISTORY_FILE)) {
    if (!saveHistory()) {
      Serial.println(
        "WARNING: Initial history checkpoint failed."
      );
    }
  }
}

// ============================================================
// HISTORY STATISTICS
// ============================================================

float getPM25Min() {
  if (!historyCount) return pm2p5;

  float x = 1e9;

  for (int i = 0; i < historyCount; i++) {
    x = min(x, pm25History[i]);
  }

  return x;
}

float getPM25Max() {
  if (!historyCount) return pm2p5;

  float x = -1e9;

  for (int i = 0; i < historyCount; i++) {
    x = max(x, pm25History[i]);
  }

  return x;
}

float getPM25Average() {
  if (!historyCount) return pm2p5;

  float x = 0;

  for (int i = 0; i < historyCount; i++) {
    x += pm25History[i];
  }

  return x / historyCount;
}

float getPM10Min() {
  if (!historyCount) return pm10p0;

  float x = 1e9;

  for (int i = 0; i < historyCount; i++) {
    x = min(x, pm10History[i]);
  }

  return x;
}

float getPM10Max() {
  if (!historyCount) return pm10p0;

  float x = -1e9;

  for (int i = 0; i < historyCount; i++) {
    x = max(x, pm10History[i]);
  }

  return x;
}

float getPM10Average() {
  if (!historyCount) return pm10p0;

  float x = 0;

  for (int i = 0; i < historyCount; i++) {
    x += pm10History[i];
  }

  return x / historyCount;
}

// ============================================================
// TIME / UPTIME HELPERS
// ============================================================

String getCurrentTime() {
  struct tm t;

  if (!getLocalTime(&t, 100)) {
    return "Time not synchronized";
  }

  timeSynchronized = true;

  char buffer[64];

  strftime(
    buffer,
    sizeof(buffer),
    "%a %d %b %Y  %H:%M:%S",
    &t
  );

  return String(buffer);
}

String getUptime() {
  unsigned long seconds = millis() / 1000;

  unsigned long days = seconds / 86400;
  seconds %= 86400;

  unsigned long hours = seconds / 3600;
  seconds %= 3600;

  unsigned long minutes = seconds / 60;
  seconds %= 60;

  char buffer[32];

  if (days) {
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

String historyLabel(int index) {
  if (historyEpoch[index]) {
    time_t t = historyEpoch[index];

    struct tm ti;
    localtime_r(&t, &ti);

    char buffer[12];

    strftime(
      buffer,
      sizeof(buffer),
      "%H:%M",
      &ti
    );

    return String(buffer);
  }

  unsigned long age =
    millis() - historyTime[index];

  unsigned long minutes =
    age / 60000UL;

  return
    minutes
      ? "-" + String(minutes) + "m"
      : "Now";
}

// ============================================================
// TFT HELPERS
// ============================================================

void drawHeader(const char* title, uint16_t accent) {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, 320, 36, accent);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print(title);

  tft.setTextSize(1);
  tft.setCursor(285, 13);
  tft.print("V1.5");
}

void drawMetricCard(
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  const char* label,
  float value,
  const char* unit,
  uint16_t color
) {
  tft.drawRoundRect(
    x,
    y,
    w,
    h,
    8,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + 8);
  tft.print(label);

  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(x + 8, y + 27);
  tft.print(value, 1);

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + h - 13);
  tft.print(unit);
}

void showFatalError(
  const char* title,
  const char* line1,
  const char* line2
) {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, 320, 46, ILI9341_RED);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(3);
  tft.setCursor(20, 10);
  tft.print(title);

  tft.setTextSize(2);
  tft.setCursor(20, 90);
  tft.print(line1);

  tft.setCursor(20, 130);
  tft.print(line2);

  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(20, 190);
  tft.print("System halted");
}

// ============================================================
// TFT UPDATE
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

    case 3:
      drawSystemScreen();
      break;
  }
}

// ============================================================
// TFT SCREEN 0 — AIR QUALITY
// ============================================================

void drawAirQualityScreen() {
  drawHeader("AIR QUALITY", ILI9341_NAVY);

  // PM2.5 main reading
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(18, 54);
  tft.print("PM2.5");

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(6);
  tft.setCursor(18, 82);
  tft.print(pm2p5, 1);

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(210, 122);
  tft.print("ug/m3");

  // Alert-state badge. This intentionally reflects the configured
  // alert threshold rather than claiming an AQI standard.
  bool elevated = pm2p5 >= pm25AlertThreshold;

  uint16_t badgeColor =
    elevated
      ? ILI9341_RED
      : ILI9341_DARKGREEN;

  tft.fillRoundRect(
    205,
    54,
    98,
    42,
    8,
    badgeColor
  );

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(
    elevated ? 220 : 216,
    67
  );
  tft.print(
    elevated
      ? "ALERT"
      : "NORMAL"
  );

  // Bottom metric cards
  drawMetricCard(
    10, 162, 94, 68,
    "PM1.0",
    pm1p0,
    "ug/m3",
    ILI9341_CYAN
  );

  drawMetricCard(
    113, 162, 94, 68,
    "PM4.0",
    pm4p0,
    "ug/m3",
    ILI9341_YELLOW
  );

  drawMetricCard(
    216, 162, 94, 68,
    "PM10",
    pm10p0,
    "ug/m3",
    ILI9341_ORANGE
  );
}

// ============================================================
// TFT SCREEN 1 — CLOCK / SUMMARY
// ============================================================

void drawClockScreen() {
  drawHeader("CLOCK & SUMMARY", ILI9341_DARKGREEN);

  struct tm timeinfo;

  if (getLocalTime(&timeinfo, 100)) {
    timeSynchronized = true;

    char dateBuffer[32];
    char timeBuffer[10];

    strftime(
      dateBuffer,
      sizeof(dateBuffer),
      "%A, %d %b %Y",
      &timeinfo
    );

    strftime(
      timeBuffer,
      sizeof(timeBuffer),
      "%H:%M",
      &timeinfo
    );

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(28, 55);
    tft.print(dateBuffer);

    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(7);
    tft.setCursor(55, 88);
    tft.print(timeBuffer);

  } else {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(3);
    tft.setCursor(35, 90);
    tft.print("TIME NOT SYNCED");
  }

  tft.drawLine(
    15,
    170,
    305,
    170,
    ILI9341_DARKGREY
  );

  drawMetricCard(
    10, 180, 94, 50,
    "PM2.5",
    pm2p5,
    "ug/m3",
    ILI9341_GREEN
  );

  drawMetricCard(
    113, 180, 94, 50,
    "PM10",
    pm10p0,
    "ug/m3",
    ILI9341_ORANGE
  );

  drawMetricCard(
    216, 180, 94, 50,
    "SIZE",
    typicalSize,
    "um",
    ILI9341_CYAN
  );
}

// ============================================================
// TFT SCREEN 2 — PARTICLE COUNTS
// ============================================================

void drawParticleScreen() {
  drawHeader("PARTICLE COUNTS", ILI9341_DARKCYAN);

  drawMetricCard(
    10, 50, 94, 78,
    "NC0.5",
    nc0p5,
    "#/cm3",
    ILI9341_CYAN
  );

  drawMetricCard(
    113, 50, 94, 78,
    "NC1.0",
    nc1p0,
    "#/cm3",
    ILI9341_GREEN
  );

  drawMetricCard(
    216, 50, 94, 78,
    "NC2.5",
    nc2p5,
    "#/cm3",
    ILI9341_YELLOW
  );

  drawMetricCard(
    61, 145, 94, 78,
    "NC4.0",
    nc4p0,
    "#/cm3",
    ILI9341_ORANGE
  );

  drawMetricCard(
    165, 145, 94, 78,
    "NC10",
    nc10p0,
    "#/cm3",
    ILI9341_RED
  );
}

// ============================================================
// TFT SCREEN 3 — SYSTEM
// ============================================================

void drawSystemScreen() {
  drawHeader("SYSTEM", ILI9341_MAROON);

  const int xLabel = 15;
  const int xValue = 125;

  int y = 52;

  tft.setTextSize(2);

  // Wi-Fi
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Wi-Fi");

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(xValue, y);
    tft.print("Connected ");

    tft.setTextColor(ILI9341_WHITE);
    tft.print(WiFi.RSSI());
    tft.print(" dBm");
  } else {
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(xValue, y);
    tft.print("Disconnected");
  }

  // IP
  y += 30;

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("IP");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(xValue, y);

  if (WiFi.status() == WL_CONNECTED) {
    tft.print(WiFi.localIP());
  } else {
    tft.print("No IP");
  }

  // SPS30
  y += 30;

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("SPS30");

  tft.setTextColor(
    sps30Online
      ? ILI9341_GREEN
      : ILI9341_RED
  );

  tft.setCursor(xValue, y);
  tft.print(
    sps30Online
      ? "Online"
      : "Offline"
  );

  if (sps30Online) {
    tft.setTextColor(ILI9341_WHITE);
    tft.print("  FW ");
    tft.print(fwMajor);
    tft.print(".");
    tft.print(fwMinor);
  }

  // History
  y += 30;

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("History");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(xValue, y);
  tft.print(historyCount);
  tft.print("/");
  tft.print(HISTORY_SIZE);

  tft.print(
    LittleFS.exists(HISTORY_FILE)
      ? " saved"
      : " RAM"
  );

  // Telegram
  y += 30;

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Telegram");

  tft.setTextColor(
    telegramConfigured()
      ? ILI9341_GREEN
      : ILI9341_YELLOW
  );

  tft.setCursor(xValue, y);
  tft.print(
    telegramConfigured()
      ? "Ready"
      : "Not configured"
  );

  // Uptime
  y += 30;

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Uptime");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(xValue, y);
  tft.print(getUptime());
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

  server.on(
    "/api/history",
    HTTP_GET,
    handleApiHistory
  );

  server.onNotFound(
    handleNotFound
  );

  server.begin();

  Serial.println("Web server started.");
}

// ============================================================
// WEB DASHBOARD
// Preserved from V1.4.2, version label updated to V1.5.
// ============================================================

void handleRoot() {
  const char html[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Air Quality Monitor</title>
<style>
*{box-sizing:border-box}
:root{
  --bg:#f2f4f7;
  --card:#fff;
  --text:#202124;
  --sec:#666;
  --border:#eee;
  --accent:#137333;
  --status:#e6f4ea
}
[data-theme=dark]{
  --bg:#101214;
  --card:#1b1e22;
  --text:#f1f3f4;
  --sec:#aeb4bb;
  --border:#30343a;
  --accent:#81c995;
  --status:#183522
}
body{
  margin:0;
  font-family:system-ui,-apple-system,Segoe UI,sans-serif;
  background:var(--bg);
  color:var(--text)
}
.container{
  max-width:1000px;
  margin:auto;
  padding:16px
}
.header,.main-card,.section,.card{
  background:var(--card);
  border-radius:16px;
  box-shadow:0 2px 8px #0002
}
.header{
  padding:20px;
  margin-bottom:16px;
  position:relative
}
.header h1{
  margin:0;
  font-size:24px
}
.header p{
  margin:6px 0 0;
  color:var(--sec)
}
button{
  position:absolute;
  right:16px;
  top:16px;
  border:0;
  border-radius:50%;
  width:40px;
  height:40px;
  background:var(--bg);
  color:var(--text);
  font-size:20px
}
.main-card{
  text-align:center;
  padding:25px;
  margin-bottom:16px
}
.main-label{
  color:var(--sec)
}
.pm25{
  font-size:64px;
  font-weight:700
}
.unit{
  color:var(--sec)
}
.grid,.stats{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:12px;
  margin-bottom:16px
}
.card,.stat{
  padding:18px
}
.title{
  font-size:14px;
  color:var(--sec)
}
.value{
  font-size:26px;
  font-weight:600
}
.section{
  padding:20px;
  margin-bottom:16px
}
.section h2{
  margin-top:0;
  font-size:18px
}
.stat{
  text-align:center;
  background:var(--bg);
  border-radius:10px
}
.statv{
  font-size:22px;
  font-weight:600;
  margin-top:4px
}
.chart{
  height:320px
}
.row{
  display:flex;
  justify-content:space-between;
  padding:8px 0;
  border-bottom:1px solid var(--border)
}
.row:last-child{
  border:0
}
.label{
  color:var(--sec)
}
.status{
  padding:4px 9px;
  border-radius:20px;
  background:var(--status);
  color:var(--accent)
}
@media(max-width:600px){
  .grid,.stats{grid-template-columns:1fr}
  .pm25{font-size:52px}
  .chart{height:240px}
}
</style>
</head>
<body>
<div class="container">

<div class="header">
  <button id="theme" onclick="toggle()">🌙</button>
  <h1>🏠 Air Quality Monitor</h1>
  <p id="dt">Loading time...</p>
</div>

<div class="main-card">
  <div class="main-label">PM2.5</div>
  <div class="pm25" id="p25">--</div>
  <div class="unit">µg/m³</div>
</div>

<div class="grid">
  <div class="card">
    <div class="title">PM1.0</div>
    <div class="value" id="p1">--</div>
    <div class="unit">µg/m³</div>
  </div>

  <div class="card">
    <div class="title">PM4.0</div>
    <div class="value" id="p4">--</div>
    <div class="unit">µg/m³</div>
  </div>

  <div class="card">
    <div class="title">PM10</div>
    <div class="value" id="p10">--</div>
    <div class="unit">µg/m³</div>
  </div>
</div>

<div class="section">
  <h2>PM History — Last 60 Minutes</h2>
  <canvas id="c" class="chart"></canvas>
</div>

<div class="section">
  <h2>PM2.5 — 60 Minute Statistics</h2>
  <div class="stats">
    <div class="stat">
      <div class="title">Minimum</div>
      <div class="statv" id="a1">--</div>
    </div>
    <div class="stat">
      <div class="title">Average</div>
      <div class="statv" id="a2">--</div>
    </div>
    <div class="stat">
      <div class="title">Maximum</div>
      <div class="statv" id="a3">--</div>
    </div>
  </div>
</div>

<div class="section">
  <h2>PM10 — 60 Minute Statistics</h2>
  <div class="stats">
    <div class="stat">
      <div class="title">Minimum</div>
      <div class="statv" id="b1">--</div>
    </div>
    <div class="stat">
      <div class="title">Average</div>
      <div class="statv" id="b2">--</div>
    </div>
    <div class="stat">
      <div class="title">Maximum</div>
      <div class="statv" id="b3">--</div>
    </div>
  </div>
</div>

<div class="section">
  <h2>Particle Counts</h2>
  <div class="row"><span class="label">NC0.5</span><span id="n05">--</span></div>
  <div class="row"><span class="label">NC1.0</span><span id="n10">--</span></div>
  <div class="row"><span class="label">NC2.5</span><span id="n25">--</span></div>
  <div class="row"><span class="label">NC4.0</span><span id="n40">--</span></div>
  <div class="row"><span class="label">NC10</span><span id="n100">--</span></div>
</div>

<div class="section">
  <h2>Sensor</h2>
  <div class="row"><span class="label">Typical particle size</span><span id="sz">--</span></div>
  <div class="row"><span class="label">Firmware</span><span id="fw">--</span></div>
  <div class="row"><span class="label">History samples</span><span id="hc">--</span></div>
  <div class="row"><span class="label">Persistent history</span><span id="persist">--</span></div>
  <div class="row"><span class="label">Status</span><span id="ss" class="status">--</span></div>
</div>

<div class="section">
  <h2>Device</h2>
  <div class="row"><span class="label">Wi-Fi</span><span id="wifi">--</span></div>
  <div class="row"><span class="label">Signal</span><span id="rssi">--</span></div>
  <div class="row"><span class="label">Uptime</span><span id="up">--</span></div>
  <div class="row"><span class="label">Monitor</span><span>V1.5</span></div>
</div>

<div class="section">
  <h2>Telegram Alerts</h2>
  <div class="row"><span class="label">Alerts</span><span id="al">--</span></div>
  <div class="row"><span class="label">PM2.5 threshold</span><span id="at25">--</span></div>
  <div class="row"><span class="label">PM10 threshold</span><span id="at10">--</span></div>
</div>

</div>

<script>
const $=x=>document.getElementById(x);

let D={
  p25:[],
  p10:[],
  t:[]
};

function theme(t){
  document.documentElement.dataset.theme=t;
  localStorage.theme=t;
  $('theme').textContent=t==='dark'?'☀️':'🌙';
}

theme(
  localStorage.theme ||
  (
    matchMedia('(prefers-color-scheme:dark)').matches
      ? 'dark'
      : 'light'
  )
);

function toggle(){
  theme(
    document.documentElement.dataset.theme==='dark'
      ? 'light'
      : 'dark'
  );
  draw();
}

function draw(){
  let c=$('c');
  let x=c.getContext('2d');
  let w=c.clientWidth;
  let h=c.clientHeight;
  let d=devicePixelRatio||1;

  c.width=w*d;
  c.height=h*d;

  x.scale(d,d);
  x.clearRect(0,0,w,h);

  if(!D.p25.length){
    x.fillStyle=getComputedStyle(
      document.documentElement
    ).getPropertyValue('--sec');

    x.textAlign='center';
    x.fillText(
      'Waiting for historical data...',
      w/2,
      h/2
    );
    return;
  }

  let L=42;
  let R=12;
  let T=20;
  let B=35;

  let CW=w-L-R;
  let CH=h-T-B;

  let m=10;

  D.p25.concat(D.p10).forEach(
    v=>m=Math.max(m,v)
  );

  m*=1.15;

  let st=getComputedStyle(
    document.documentElement
  );

  x.strokeStyle=
    st.getPropertyValue('--border');

  x.fillStyle=
    st.getPropertyValue('--sec');

  x.font='11px system-ui';

  for(let i=0;i<=4;i++){
    let y=T+CH-CH*i/4;

    x.beginPath();
    x.moveTo(L,y);
    x.lineTo(w-R,y);
    x.stroke();

    x.textAlign='right';

    x.fillText(
      (m*i/4).toFixed(0),
      L-6,
      y+4
    );
  }

  x.textAlign='center';

  for(let i=0;i<5;i++){
    let j=Math.floor(
      (D.t.length-1)*i/4
    );

    x.fillText(
      D.t[j],
      L+CW*i/4,
      h-10
    );
  }

  function line(a,col){
    if(a.length<2)return;

    x.strokeStyle=col;
    x.lineWidth=2;

    x.beginPath();

    a.forEach(
      (v,i)=>{
        let xx=
          L+CW*i/(a.length-1);

        let yy=
          T+CH-v/m*CH;

        i
          ? x.lineTo(xx,yy)
          : x.moveTo(xx,yy);
      }
    );

    x.stroke();
  }

  line(D.p25,'#4caf50');
  line(D.p10,'#ff9800');

  x.font='12px system-ui';
  x.textAlign='left';

  x.fillStyle='#4caf50';
  x.fillText('● PM2.5',L,12);

  x.fillStyle='#ff9800';
  x.fillText('● PM10',L+75,12);
}

async function data(){
  try{
    let d=await(
      await fetch('/api/status')
    ).json();

    $('p25').textContent=d.pm2_5.toFixed(1);
    $('p1').textContent=d.pm1_0.toFixed(1);
    $('p4').textContent=d.pm4_0.toFixed(1);
    $('p10').textContent=d.pm10.toFixed(1);

    $('n05').textContent=d.nc0_5.toFixed(1);
    $('n10').textContent=d.nc1_0.toFixed(1);
    $('n25').textContent=d.nc2_5.toFixed(1);
    $('n40').textContent=d.nc4_0.toFixed(1);
    $('n100').textContent=d.nc10.toFixed(1);

    $('sz').textContent=
      d.typical_size.toFixed(2)+' µm';

    $('fw').textContent=d.firmware;
    $('hc').textContent=d.history_count+' / 120';
    $('persist').textContent=d.persistent?'Yes':'No';
    $('ss').textContent=d.sps30_online?'Online':'Offline';

    $('wifi').textContent=d.wifi;
    $('rssi').textContent=d.rssi+' dBm';
    $('up').textContent=d.uptime;
    $('dt').textContent=d.datetime;

    $('al').textContent=
      d.alerts_enabled
        ? 'Enabled'
        : 'Disabled';

    $('at25').textContent=
      d.pm25_threshold.toFixed(1)+' µg/m³';

    $('at10').textContent=
      d.pm10_threshold.toFixed(1)+' µg/m³';

  }catch(e){}
}

async function hist(){
  try{
    let d=await(
      await fetch('/api/history')
    ).json();

    D.p25=d.pm25;
    D.p10=d.pm10;
    D.t=d.time;

    $('a1').textContent=d.pm25_min.toFixed(1);
    $('a2').textContent=d.pm25_avg.toFixed(1);
    $('a3').textContent=d.pm25_max.toFixed(1);

    $('b1').textContent=d.pm10_min.toFixed(1);
    $('b2').textContent=d.pm10_avg.toFixed(1);
    $('b3').textContent=d.pm10_max.toFixed(1);

    draw();

  }catch(e){}
}

data();
hist();

setInterval(data,2000);
setInterval(hist,5000);

addEventListener('resize',draw);
</script>

</body>
</html>
)HTML";

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// API STATUS
// ============================================================

void handleApiStatus() {
  String json = "{";

  json += "\"pm1_0\":" + String(pm1p0, 2);
  json += ",\"pm2_5\":" + String(pm2p5, 2);
  json += ",\"pm4_0\":" + String(pm4p0, 2);
  json += ",\"pm10\":" + String(pm10p0, 2);

  json += ",\"nc0_5\":" + String(nc0p5, 2);
  json += ",\"nc1_0\":" + String(nc1p0, 2);
  json += ",\"nc2_5\":" + String(nc2p5, 2);
  json += ",\"nc4_0\":" + String(nc4p0, 2);
  json += ",\"nc10\":" + String(nc10p0, 2);

  json +=
    ",\"typical_size\":" +
    String(typicalSize, 3);

  json +=
    ",\"sps30_online\":" +
    String(
      sps30Online
        ? "true"
        : "false"
    );

  json +=
    ",\"firmware\":\"" +
    String(fwMajor) +
    "." +
    String(fwMinor) +
    "\"";

  json +=
    ",\"wifi\":\"" +
    String(
      WiFi.status() == WL_CONNECTED
        ? "Connected"
        : "Disconnected"
    ) +
    "\"";

  json +=
    ",\"rssi\":" +
    String(WiFi.RSSI());

  json +=
    ",\"uptime\":\"" +
    getUptime() +
    "\"";

  json +=
    ",\"datetime\":\"" +
    getCurrentTime() +
    "\"";

  json +=
    ",\"history_count\":" +
    String(historyCount);

  json +=
    ",\"persistent\":" +
    String(
      LittleFS.exists(HISTORY_FILE)
        ? "true"
        : "false"
    );

  json +=
    ",\"alerts_enabled\":" +
    String(
      alertsEnabled
        ? "true"
        : "false"
    );

  json +=
    ",\"pm25_threshold\":" +
    String(pm25AlertThreshold, 1);

  json +=
    ",\"pm10_threshold\":" +
    String(pm10AlertThreshold, 1);

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// API HISTORY
// ============================================================

void handleApiHistory() {
  String json =
    "{\"count\":" +
    String(historyCount) +
    ",\"pm25\":[";

  for (int i = 0; i < historyCount; i++) {
    int k =
      (historyCount < HISTORY_SIZE)
        ? i
        : (historyWriteIndex + i) % HISTORY_SIZE;

    if (i) json += ",";

    json += String(
      pm25History[k],
      2
    );
  }

  json += "],\"pm10\":[";

  for (int i = 0; i < historyCount; i++) {
    int k =
      (historyCount < HISTORY_SIZE)
        ? i
        : (historyWriteIndex + i) % HISTORY_SIZE;

    if (i) json += ",";

    json += String(
      pm10History[k],
      2
    );
  }

  json += "],\"time\":[";

  for (int i = 0; i < historyCount; i++) {
    int k =
      (historyCount < HISTORY_SIZE)
        ? i
        : (historyWriteIndex + i) % HISTORY_SIZE;

    if (i) json += ",";

    json += "\"";
    json += historyLabel(k);
    json += "\"";
  }

  json += "]";

  json +=
    ",\"pm25_min\":" +
    String(getPM25Min(), 2);

  json +=
    ",\"pm25_avg\":" +
    String(getPM25Average(), 2);

  json +=
    ",\"pm25_max\":" +
    String(getPM25Max(), 2);

  json +=
    ",\"pm10_min\":" +
    String(getPM10Min(), 2);

  json +=
    ",\"pm10_avg\":" +
    String(getPM10Average(), 2);

  json +=
    ",\"pm10_max\":" +
    String(getPM10Max(), 2);

  json +=
    ",\"persistent\":" +
    String(
      LittleFS.exists(HISTORY_FILE)
        ? "true"
        : "false"
    );

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
// TELEGRAM
// ============================================================

void sendTelegramMessage(
  const String& message
) {
  if (
    telegramConfigured() &&
    WiFi.status() == WL_CONNECTED
  ) {
    bot.sendMessage(
      TELEGRAM_CHAT_ID,
      message,
      ""
    );
  }
}

void handleTelegram() {
  if (!telegramConfigured()) {
    return;
  }

  // One Telegram request per poll only.
  // Do not drain updates in a while-loop because synchronous
  // HTTPS calls can starve the local WebServer.
  int n =
    bot.getUpdates(
      bot.last_message_received + 1
    );

  if (n <= 0) {
    return;
  }

  for (int i = 0; i < n; i++) {
    String chat =
      bot.messages[i].chat_id;

    String command =
      bot.messages[i].text;

    if (chat != String(TELEGRAM_CHAT_ID)) {
      bot.sendMessage(
        chat,
        "Unauthorized chat.",
        ""
      );

      continue;
    }

    handleTelegramCommand(
      chat,
      command
    );
  }
}

void handleTelegramCommand(
  String chatId,
  String command
) {
  command.trim();
  command.toLowerCase();

  if (
    command == "/start" ||
    command == "/help"
  ) {
    bot.sendMessage(
      chatId,
      "🏠 Air Monitor V1.5\n\n"
      "/status - current PM\n"
      "/particles - particle counts\n"
      "/stats - 60-minute statistics\n"
      "/history - history status\n"
      "/system - device status\n"
      "/alerts - alert settings\n"
      "/alertson - enable alerts\n"
      "/alertsoff - disable alerts",
      ""
    );

    return;
  }

  if (
    command == "/status" ||
    command == "/pm"
  ) {
    String message =
      "🌫 AIR QUALITY\n\n"
      "PM1.0: " +
      String(pm1p0, 1) +
      " µg/m³\n"
      "PM2.5: " +
      String(pm2p5, 1) +
      " µg/m³\n"
      "PM4.0: " +
      String(pm4p0, 1) +
      " µg/m³\n"
      "PM10: " +
      String(pm10p0, 1) +
      " µg/m³\n\n" +
      getCurrentTime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/particles") {
    String message =
      "🔬 PARTICLES\n\n"
      "NC0.5: " +
      String(nc0p5, 1) +
      "\n"
      "NC1.0: " +
      String(nc1p0, 1) +
      "\n"
      "NC2.5: " +
      String(nc2p5, 1) +
      "\n"
      "NC4.0: " +
      String(nc4p0, 1) +
      "\n"
      "NC10: " +
      String(nc10p0, 1) +
      "\n\n"
      "Typical size: " +
      String(typicalSize, 2) +
      " µm";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/stats") {
    String message =
      "📊 60-MINUTE STATS\n\n"
      "PM2.5 Min/Avg/Max: " +
      String(getPM25Min(), 1) +
      " / " +
      String(getPM25Average(), 1) +
      " / " +
      String(getPM25Max(), 1) +
      "\n"
      "PM10 Min/Avg/Max: " +
      String(getPM10Min(), 1) +
      " / " +
      String(getPM10Average(), 1) +
      " / " +
      String(getPM10Max(), 1);

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/history") {
    String message =
      "📈 HISTORY\n\n"
      "Samples: " +
      String(historyCount) +
      " / 120\n"
      "Interval: 30 seconds\n"
      "Window: 60 minutes\n"
      "Persistent checkpoint: " +
      String(
        LittleFS.exists(HISTORY_FILE)
          ? "Yes"
          : "No"
      );

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/system") {
    String message =
      "⚙️ SYSTEM\n\n"
      "Monitor: V1.5\n"
      "Display: 2.8in ILI9341 TFT\n"
      "SPS30: " +
      String(
        sps30Online
          ? "Online"
          : "Offline"
      ) +
      "\nFirmware: " +
      String(fwMajor) +
      "." +
      String(fwMinor) +
      "\nWi-Fi: " +
      String(
        WiFi.status() == WL_CONNECTED
          ? "Connected"
          : "Disconnected"
      ) +
      "\nIP: " +
      WiFi.localIP().toString() +
      "\nRSSI: " +
      String(WiFi.RSSI()) +
      " dBm\nUptime: " +
      getUptime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/alerts") {
    String message =
      "🚨 ALERTS\n\n"
      "Status: " +
      String(
        alertsEnabled
          ? "Enabled"
          : "Disabled"
      ) +
      "\nPM2.5: " +
      String(pm25AlertThreshold, 1) +
      " µg/m³\nPM10: " +
      String(pm10AlertThreshold, 1) +
      " µg/m³\nCooldown: 30 minutes";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/alertson") {
    alertsEnabled = true;

    bot.sendMessage(
      chatId,
      "🚨 Air-quality alerts enabled.",
      ""
    );

    return;
  }

  if (command == "/alertsoff") {
    alertsEnabled = false;

    bot.sendMessage(
      chatId,
      "🔕 Air-quality alerts disabled.",
      ""
    );

    return;
  }

  bot.sendMessage(
    chatId,
    "Unknown command. Use /help",
    ""
  );
}

// ============================================================
// ALERTS
// ============================================================

void checkAirQualityAlert() {
  if (
    !alertsEnabled ||
    !sps30Online ||
    !telegramConfigured()
  ) {
    return;
  }

  // PM2.5
  if (pm2p5 >= pm25AlertThreshold) {
    if (!pm25AlertActive) {
      pm25AlertActive = true;

      if (
        !lastPM25AlertSent ||
        millis() - lastPM25AlertSent >= ALERT_COOLDOWN
      ) {
        sendTelegramMessage(
          "🚨 PM2.5 ALERT\n\n"
          "Current: " +
          String(pm2p5, 1) +
          " µg/m³\n"
          "Threshold: " +
          String(pm25AlertThreshold, 1) +
          " µg/m³\n\n" +
          getCurrentTime()
        );

        lastPM25AlertSent = millis();
      }
    }
  } else if (pm25AlertActive) {
    pm25AlertActive = false;

    sendTelegramMessage(
      "✅ PM2.5 returned below threshold. Current: " +
      String(pm2p5, 1) +
      " µg/m³"
    );
  }

  // PM10
  if (pm10p0 >= pm10AlertThreshold) {
    if (!pm10AlertActive) {
      pm10AlertActive = true;

      if (
        !lastPM10AlertSent ||
        millis() - lastPM10AlertSent >= ALERT_COOLDOWN
      ) {
        sendTelegramMessage(
          "🚨 PM10 ALERT\n\n"
          "Current: " +
          String(pm10p0, 1) +
          " µg/m³\n"
          "Threshold: " +
          String(pm10AlertThreshold, 1) +
          " µg/m³\n\n" +
          getCurrentTime()
        );

        lastPM10AlertSent = millis();
      }
    }
  } else if (pm10AlertActive) {
    pm10AlertActive = false;

    sendTelegramMessage(
      "✅ PM10 returned below threshold. Current: " +
      String(pm10p0, 1) +
      " µg/m³"
    );
  }
}

// ============================================================
// SERIAL SYSTEM STATUS
// ============================================================

void printSystemStatus() {
  Serial.println();
  Serial.println("SYSTEM STATUS");
  Serial.println("-------------");

  Serial.println("Monitor: V1.5");
  Serial.println("Display: 2.8in ILI9341 SPI TFT");

  Serial.printf(
    "Wi-Fi: %s\n",
    WiFi.status() == WL_CONNECTED
      ? "Connected"
      : "Disconnected"
  );

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(
      "IP: %s\n",
      WiFi.localIP().toString().c_str()
    );

    Serial.printf(
      "RSSI: %d dBm\n",
      WiFi.RSSI()
    );
  }

  Serial.printf(
    "mDNS: http://%s.local\n",
    MDNS_HOSTNAME
  );

  Serial.printf(
    "SPS30: %s\n",
    sps30Online
      ? "Online"
      : "Offline"
  );

  Serial.printf(
    "Firmware: %u.%u\n",
    fwMajor,
    fwMinor
  );

  Serial.printf(
    "History: %d / %d\n",
    historyCount,
    HISTORY_SIZE
  );

  Serial.printf(
    "Persistent: %s\n",
    LittleFS.exists(HISTORY_FILE)
      ? "Yes"
      : "No"
  );

  Serial.printf(
    "Telegram: %s\n",
    telegramConfigured()
      ? "Configured"
      : "Not configured"
  );

  Serial.println();
}
