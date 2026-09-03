/*
  ESP32-S3 Air Quality Monitor — V1.7

  Migration from V1.4.2:
    - Web safety fix: dashboard in global PROGMEM; JSON responses bounded/streamed
    - Telegram polling interval: 120 seconds
    - Replaces SSD1306 128x64 I2C OLED with 2.8" ILI9341 240x320 SPI TFT
    - TFT is used in landscape mode: 320x240
    - SPS30, Wi-Fi, mDNS, NTP, web dashboard, Telegram,
      60-minute volatile RAM history and alerts are retained

  TFT wiring used by this sketch:
    TFT CS    -> GPIO 10
    TFT MOSI  -> GPIO 11
    TFT SCK   -> GPIO 12
    TFT MISO  -> GPIO 13
    TFT DC/RS -> GPIO 14
    TFT RST   -> GPIO 15

  Shared I2C wiring:
    SDA -> GPIO 8
    SCL -> GPIO 9
    SPS30 address -> 0x69
    SCD4X address -> 0x62

  Libraries:
    - Adafruit GFX Library
    - Adafruit ILI9341
    - Sensirion I2C SPS30
    - Sensirion I2C SCD4X (Sensirion Core dependency)
    - UniversalTelegramBot
    - ArduinoJson

  Notes:
    - TFT VCC/backlight wiring is handled in hardware.
    - V1.7 adds SCD4X CO2 / temperature / humidity and uses a five-screen carousel.
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
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SensirionI2cSps30.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSht4x.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

// ============================================================
// ESP32-S3 AIR QUALITY MONITOR V1.7
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
SensirionI2cScd4x scd4x;
SensirionI2cSht4x sht4x;
SensirionI2CSgp41 sgp41;

VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;

// ------------------------------------------------------------
// I2C — SPS30 + SCD4X + SHT45 + SGP41
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
// CURRENT SCD4X READINGS
// ------------------------------------------------------------

uint16_t co2ppm = 0;
float scdTemperature = 0.0;
float scdHumidity = 0.0;

bool scd4xOnline = false;
bool scd4xHasReading = false;

uint64_t scd4xSerial = 0;

unsigned long lastScd4xPoll = 0;
#define SCD4X_POLL_INTERVAL 1000UL

// ------------------------------------------------------------
// CURRENT SHT45 + SGP41 READINGS
// SHT45 is authoritative for room temperature / humidity.
// SGP41 receives SHT45 T/RH compensation every measurement.
// ------------------------------------------------------------

float shtTemperature = 0.0;
float shtHumidity = 0.0;

bool sht45Online = false;
bool sht45HasReading = false;

uint16_t srawVoc = 0;
uint16_t srawNox = 0;

int32_t vocIndex = 0;
int32_t noxIndex = 0;

bool sgp41Online = false;
bool sgp41HasReading = false;

uint16_t sgpConditioningSecondsRemaining = 10;

unsigned long lastEnvironmentPoll = 0;

#define ENVIRONMENT_POLL_INTERVAL 1000UL

uint8_t sht45ConsecutiveErrors = 0;
uint8_t sgp41ConsecutiveErrors = 0;

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
uint16_t co2History[HISTORY_SIZE];

unsigned long historyTime[HISTORY_SIZE];
uint32_t historyEpoch[HISTORY_SIZE];

int historyCount = 0;
int historyWriteIndex = 0;

// ------------------------------------------------------------
// HISTORY STORAGE POLICY
// Volatile RAM only. No filesystem / flash writes.
// History is intentionally cleared whenever the ESP32 reboots.
// ------------------------------------------------------------

// ------------------------------------------------------------
// TELEGRAM / ALERTS
// ------------------------------------------------------------

unsigned long lastTelegramPoll = 0;
#define TELEGRAM_POLL_INTERVAL 120000UL

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
void updateSCD4X();
void updateEnvironmentSensors();
void updateHistory();
void addHistorySample();


void updateDisplay();
void drawAirQualityScreen();
void drawClockScreen();
void drawParticleScreen();
void drawCO2Screen();
void drawEnvironmentScreen();
void drawGasScreen();
void drawSystemScreen();

void drawHeader(const char* title, uint16_t accent);
void drawMetricCard(int16_t x, int16_t y, int16_t w, int16_t h,
                    const char* label, float value, const char* unit,
                    uint16_t color);
void showFatalError(const char* title, const char* line1, const char* line2);

void startWebServer();
void handleRoot();
void handleApiLive();
void handleApiSystem();
void handleApiStatus();   // legacy compatibility endpoint
void handleApiHistory();
void handleNotFound();

void handleTelegram();
void handleTelegramCommand(String chatId, String command);
void checkAirQualityAlert();
void sendTelegramMessage(const String& message);

void formatCurrentTime(char* buffer, size_t size);
void formatUptime(char* buffer, size_t size);
void formatHistoryLabel(int index, char* buffer, size_t size);

String getCurrentTime();
String getUptime();
String historyLabel(int index);

float getPM25Min();
float getPM25Max();
float getPM25Average();

float getPM10Min();
float getPM10Max();
float getPM10Average();

float getCO2Min();
float getCO2Max();
float getCO2Average();

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
  Serial.println("                  V1.7");
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
  tft.print("V1.7");

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(80, 195);
  tft.print("Initializing...");

  // ----------------------------------------------------------
  // VOLATILE RAM HISTORY
  // ----------------------------------------------------------

  // Global history arrays are zero-initialized by the runtime.
  // The ring buffer starts empty on every boot and retains only
  // the latest 120 samples (60 minutes at 30-second intervals).
  historyCount = 0;
  historyWriteIndex = 0;

  Serial.println(
    "History initialized in volatile RAM: "
    "120 samples / 60 minutes / no flash writes."
  );

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
  // SCD4X — CO2 SENSOR
  // ----------------------------------------------------------

  Serial.println("Initializing SCD4X...");

  scd4x.begin(Wire, 0x62);

  // The ESP32 may reboot while the sensor itself remains powered.
  // Force it back to idle before querying identity / restarting
  // periodic measurement. A failure here is non-fatal; the sensor
  // may already be idle.
  scd4x.stopPeriodicMeasurement();

  int16_t scdError =
    scd4x.getSerialNumber(scd4xSerial);

  if (scdError) {
    Serial.printf(
      "WARNING: SCD4X probe failed: %d. Continuing without CO2 sensor.\n",
      scdError
    );
    scd4xOnline = false;
  } else {
    Serial.printf(
      "SCD4X detected. Serial: %llu\n",
      (unsigned long long)scd4xSerial
    );

    scdError = scd4x.startPeriodicMeasurement();

    if (scdError) {
      Serial.printf(
        "WARNING: SCD4X start failed: %d. Continuing without CO2 sensor.\n",
        scdError
      );
      scd4xOnline = false;
    } else {
      scd4xOnline = true;
      Serial.println(
        "SCD4X periodic measurement started (5 second update interval)."
      );
    }
  }

  // ----------------------------------------------------------
  // SHT45 — AUTHORITATIVE TEMPERATURE / HUMIDITY
  // SGP41 — VOC / NOx WITH SHT45 COMPENSATION
  // ----------------------------------------------------------

  Serial.println("Initializing SHT45 + SGP41...");

  // SHT45 default address on this 7Semi board.
  sht4x.begin(Wire, 0x44);

  float initialTemperature = 0.0;
  float initialHumidity = 0.0;

  int16_t shtError =
    sht4x.measureHighPrecision(
      initialTemperature,
      initialHumidity
    );

  if (shtError) {
    Serial.printf(
      "WARNING: SHT45 initial read failed: %d. Will retry in loop.\n",
      shtError
    );
    sht45Online = false;
    sht45HasReading = false;
  } else {
    shtTemperature = initialTemperature;
    shtHumidity = initialHumidity;
    sht45Online = true;
    sht45HasReading = true;

    Serial.printf(
      "SHT45 online. Temperature %.2f C, RH %.2f %%\n",
      shtTemperature,
      shtHumidity
    );
  }

  // SGP41 fixed I2C address is 0x59; the Sensirion driver
  // configures that internally.
  sgp41.begin(Wire);

  vocAlgorithm.reset();
  noxAlgorithm.reset();

  sgpConditioningSecondsRemaining = 10;
  sgp41Online = false;
  sgp41HasReading = false;
  lastEnvironmentPoll = millis();

  Serial.println(
    "SGP41 initialized. 10-second NOx conditioning will run at 1 Hz."
  );

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
  updateSCD4X();
  updateEnvironmentSensors();
  updateHistory();

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

  // TFT carousel.
  if (
    millis() - lastScreenChange >= SCREEN_ROTATION_INTERVAL
  ) {
    currentScreen++;

    if (currentScreen > 6) {
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

  // Intentionally do not redraw the full TFT on every SPS30 sample.
  // The active screen refreshes when the carousel changes.
}

// ============================================================
// SCD4X UPDATE
// ============================================================

void updateSCD4X() {
  if (!scd4xOnline) {
    return;
  }

  if (
    millis() - lastScd4xPoll < SCD4X_POLL_INTERVAL
  ) {
    return;
  }

  lastScd4xPoll = millis();

  bool dataReady = false;

  int16_t error =
    scd4x.getDataReadyStatus(dataReady);

  if (error || !dataReady) {
    return;
  }

  uint16_t newCO2 = 0;
  float newTemperature = 0.0;
  float newHumidity = 0.0;

  error =
    scd4x.readMeasurement(
      newCO2,
      newTemperature,
      newHumidity
    );

  if (error) {
    Serial.printf(
      "WARNING: SCD4X read failed: %d\n",
      error
    );
    return;
  }

  // Ignore an invalid zero-ppm result.
  if (newCO2 == 0) {
    return;
  }

  co2ppm = newCO2;
  scdTemperature = newTemperature;
  scdHumidity = newHumidity;
  scd4xHasReading = true;
}

// ============================================================
// SHT45 + SGP41 UPDATE
//
// Sensirion Gas Index Algorithm expects SGP41 raw measurements
// at a steady 1 Hz by default. SHT45 is read immediately before
// SGP41 and its temperature / RH are converted to SGP41
// compensation ticks.
//
// First 10 successful SGP41 cycles use executeConditioning().
// After that measureRawSignals() provides both VOC and NOx raw
// signals, which are processed by the VOC/NOx index algorithms.
// ============================================================

void updateEnvironmentSensors() {
  if (
    millis() - lastEnvironmentPoll <
    ENVIRONMENT_POLL_INTERVAL
  ) {
    return;
  }

  // Advance by one interval instead of setting to millis().
  // This reduces long-term drift in the 1 Hz algorithm cadence.
  lastEnvironmentPoll += ENVIRONMENT_POLL_INTERVAL;

  // If the loop was blocked for a long time, do not rapidly
  // "catch up" with back-to-back gas measurements.
  if (
    millis() - lastEnvironmentPoll >
    ENVIRONMENT_POLL_INTERVAL
  ) {
    lastEnvironmentPoll = millis();
  }

  // SGP41 default compensation values from Sensirion's example.
  uint16_t compensationRh = 0x8000;
  uint16_t compensationT = 0x6666;

  float newTemperature = 0.0;
  float newHumidity = 0.0;

  int16_t shtError =
    sht4x.measureHighPrecision(
      newTemperature,
      newHumidity
    );

  if (!shtError) {
    shtTemperature = newTemperature;
    shtHumidity = newHumidity;

    sht45Online = true;
    sht45HasReading = true;
    sht45ConsecutiveErrors = 0;

    // Convert SHT45 engineering units to SGP41 compensation ticks.
    float t =
      constrain(
        shtTemperature,
        -45.0f,
        130.0f
      );

    float rh =
      constrain(
        shtHumidity,
        0.0f,
        100.0f
      );

    compensationT =
      static_cast<uint16_t>(
        (t + 45.0f) *
        65535.0f /
        175.0f
      );

    compensationRh =
      static_cast<uint16_t>(
        rh *
        65535.0f /
        100.0f
      );

  } else {
    if (sht45ConsecutiveErrors < 255) {
      sht45ConsecutiveErrors++;
    }

    if (sht45ConsecutiveErrors >= 5) {
      sht45Online = false;
    }

    // SGP41 can continue with Sensirion's default compensation.
    if (
      sht45ConsecutiveErrors == 1 ||
      sht45ConsecutiveErrors % 30 == 0
    ) {
      Serial.printf(
        "WARNING: SHT45 read failed: %d; "
        "SGP41 using default compensation.\n",
        shtError
      );
    }
  }

  int16_t sgpError = 0;

  if (sgpConditioningSecondsRemaining > 0) {
    sgpError =
      sgp41.executeConditioning(
        compensationRh,
        compensationT,
        srawVoc
      );

    if (!sgpError) {
      sgpConditioningSecondsRemaining--;
      srawNox = 0;
    }

  } else {
    sgpError =
      sgp41.measureRawSignals(
        compensationRh,
        compensationT,
        srawVoc,
        srawNox
      );
  }

  if (sgpError) {
    if (sgp41ConsecutiveErrors < 255) {
      sgp41ConsecutiveErrors++;
    }

    if (sgp41ConsecutiveErrors >= 5) {
      sgp41Online = false;
    }

    if (
      sgp41ConsecutiveErrors == 1 ||
      sgp41ConsecutiveErrors % 30 == 0
    ) {
      Serial.printf(
        "WARNING: SGP41 read failed: %d\n",
        sgpError
      );
    }

    return;
  }

  sgp41ConsecutiveErrors = 0;
  sgp41Online = true;

  // Feed both raw streams to their corresponding adaptive
  // algorithms at the same 1 Hz cadence.
  vocIndex =
    vocAlgorithm.process(srawVoc);

  noxIndex =
    noxAlgorithm.process(srawNox);

  if (sgpConditioningSecondsRemaining == 0) {
    sgp41HasReading = true;
  }
}

// ============================================================
// HISTORY TIMER
// One common timeline for all sensors. This is intentionally
// independent of SPS30/SCD4X read cadence so future sensors can
// join the same 30-second RAM ring buffer cleanly.
// ============================================================

void updateHistory() {
  // Do not create history until SPS30 has produced a valid reading.
  if (lastMeasurementMillis == 0) {
    return;
  }

  // If SCD4X initialized successfully, wait for its first reading
  // before creating slot 0. This usually takes only ~5 seconds.
  // If SCD4X is offline, PM history still works normally.
  if (
    historyCount == 0 &&
    scd4xOnline &&
    !scd4xHasReading
  ) {
    return;
  }

  if (
    historyCount == 0 ||
    millis() - lastHistorySample >= HISTORY_INTERVAL
  ) {
    addHistorySample();
    lastHistorySample = millis();
  }
}

// ============================================================
// ADD HISTORY SAMPLE
// ============================================================

void addHistorySample() {
  // Store the newest reading in the current RAM ring-buffer slot.
  pm25History[historyWriteIndex] = pm2p5;
  pm10History[historyWriteIndex] = pm10p0;
  co2History[historyWriteIndex] = scd4xHasReading ? co2ppm : 0;
  historyTime[historyWriteIndex] = millis();

  // Keep an epoch timestamp when NTP is available. This is still RAM only.
  time_t now;
  time(&now);

  historyEpoch[historyWriteIndex] =
    (now > 1577836800)
      ? static_cast<uint32_t>(now)
      : 0;

  // Advance circular write position. Once full, the oldest sample
  // is overwritten by the newest sample.
  historyWriteIndex =
    (historyWriteIndex + 1) % HISTORY_SIZE;

  if (historyCount < HISTORY_SIZE) {
    historyCount++;
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

float getCO2Min() {
  uint16_t x = 65535;
  bool found = false;

  for (int i = 0; i < historyCount; i++) {
    if (co2History[i] > 0) {
      x = min(x, co2History[i]);
      found = true;
    }
  }

  return found ? (float)x : (float)co2ppm;
}

float getCO2Max() {
  uint16_t x = 0;
  bool found = false;

  for (int i = 0; i < historyCount; i++) {
    if (co2History[i] > 0) {
      x = max(x, co2History[i]);
      found = true;
    }
  }

  return found ? (float)x : (float)co2ppm;
}

float getCO2Average() {
  uint32_t total = 0;
  uint16_t count = 0;

  for (int i = 0; i < historyCount; i++) {
    if (co2History[i] > 0) {
      total += co2History[i];
      count++;
    }
  }

  return count
    ? (float)total / count
    : (float)co2ppm;
}

// ============================================================
// TIME / UPTIME HELPERS
// Bounded char-buffer versions are used by HTTP handlers to avoid
// request-time String growth and heap fragmentation.
// ============================================================

void formatCurrentTime(char* buffer, size_t size) {
  if (!buffer || size == 0) return;

  struct tm t;

  if (!getLocalTime(&t, 100)) {
    snprintf(buffer, size, "Time not synchronized");
    return;
  }

  timeSynchronized = true;

  strftime(
    buffer,
    size,
    "%a %d %b %Y  %H:%M:%S",
    &t
  );
}

void formatUptime(char* buffer, size_t size) {
  if (!buffer || size == 0) return;

  unsigned long seconds = millis() / 1000;

  unsigned long days = seconds / 86400;
  seconds %= 86400;

  unsigned long hours = seconds / 3600;
  seconds %= 3600;

  unsigned long minutes = seconds / 60;
  seconds %= 60;

  if (days) {
    snprintf(
      buffer,
      size,
      "%lud %02lu:%02lu:%02lu",
      days,
      hours,
      minutes,
      seconds
    );
  } else {
    snprintf(
      buffer,
      size,
      "%02lu:%02lu:%02lu",
      hours,
      minutes,
      seconds
    );
  }
}

void formatHistoryLabel(int index, char* buffer, size_t size) {
  if (!buffer || size == 0) return;

  if (historyEpoch[index]) {
    time_t t = historyEpoch[index];

    struct tm ti;
    localtime_r(&t, &ti);

    strftime(
      buffer,
      size,
      "%H:%M",
      &ti
    );

    return;
  }

  unsigned long age = millis() - historyTime[index];
  unsigned long minutes = age / 60000UL;

  if (minutes) {
    snprintf(buffer, size, "-%lum", minutes);
  } else {
    snprintf(buffer, size, "Now");
  }
}

String getCurrentTime() {
  char buffer[64];
  formatCurrentTime(buffer, sizeof(buffer));
  return String(buffer);
}

String getUptime() {
  char buffer[32];
  formatUptime(buffer, sizeof(buffer));
  return String(buffer);
}

String historyLabel(int index) {
  char buffer[16];
  formatHistoryLabel(index, buffer, sizeof(buffer));
  return String(buffer);
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
  tft.print("V1.7");
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
      drawCO2Screen();
      break;

    case 4:
      drawEnvironmentScreen();
      break;

    case 5:
      drawGasScreen();
      break;

    case 6:
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
    char secondsBuffer[4];

    strftime(
      dateBuffer,
      sizeof(dateBuffer),
      "%a, %d %b %Y",
      &timeinfo
    );

    strftime(
      timeBuffer,
      sizeof(timeBuffer),
      "%H:%M",
      &timeinfo
    );

    strftime(
      secondsBuffer,
      sizeof(secondsBuffer),
      "%S",
      &timeinfo
    );

    // Compact date line
    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(18, 50);
    tft.print(dateBuffer);

    // Main clock — deliberately smaller than the original size 7.
    // Size 5 keeps the time prominent without consuming the screen.
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(5);
    tft.setCursor(18, 78);
    tft.print(timeBuffer);

    // Seconds sit beside the clock instead of enlarging the whole line.
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(180, 102);
    tft.print(":");
    tft.print(secondsBuffer);

  } else {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(18, 78);
    tft.print("TIME NOT SYNCHRONIZED");
  }

  // Explicit separation between clock and sensor summary.
  tft.drawLine(
    10,
    132,
    310,
    132,
    ILI9341_DARKGREY
  );

  // Three compact summary cards.
  // 90 px height leaves enough room for label, value and unit.
  drawMetricCard(
    10, 142, 94, 88,
    "PM2.5",
    pm2p5,
    "ug/m3",
    ILI9341_GREEN
  );

  drawMetricCard(
    113, 142, 94, 88,
    "CO2",
    scd4xHasReading ? (float)co2ppm : 0.0,
    "ppm",
    scd4xHasReading ? ILI9341_CYAN : ILI9341_DARKGREY
  );

  drawMetricCard(
    216, 142, 94, 88,
    "TEMP",
    sht45HasReading ? shtTemperature : 0.0,
    "C",
    sht45HasReading ? ILI9341_YELLOW : ILI9341_DARKGREY
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
// TFT SCREEN 3 — CO2
// SCD4X is treated as the dedicated CO2 source.
// Its temperature / humidity remain internal diagnostics only.
// ============================================================

void drawCO2Screen() {
  drawHeader("CO2", ILI9341_DARKCYAN);

  if (!scd4xOnline) {
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(3);
    tft.setCursor(28, 80);
    tft.print("SCD4X OFFLINE");

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(28, 128);
    tft.print("Check 3.3V / SDA / SCL");
    return;
  }

  if (!scd4xHasReading) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(3);
    tft.setCursor(34, 84);
    tft.print("WARMING UP...");

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(55, 130);
    tft.print("First reading ~5 sec");
    return;
  }

  uint16_t co2Color = ILI9341_GREEN;
  const char* co2State = "FRESH";

  if (co2ppm >= 1500) {
    co2Color = ILI9341_RED;
    co2State = "VENTILATE";
  } else if (co2ppm >= 1000) {
    co2Color = ILI9341_ORANGE;
    co2State = "HIGH";
  } else if (co2ppm >= 800) {
    co2Color = ILI9341_YELLOW;
    co2State = "RISING";
  }

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(18, 52);
  tft.print("CO2");

  tft.setTextColor(co2Color);
  tft.setTextSize(6);
  tft.setCursor(18, 78);
  tft.print(co2ppm);

  tft.setTextSize(2);
  tft.setCursor(205, 116);
  tft.print("ppm");

  tft.fillRoundRect(
    205,
    52,
    100,
    42,
    8,
    co2Color
  );

  tft.setTextColor(ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setCursor(219, 69);
  tft.print(co2State);

  drawMetricCard(
    10, 150, 94, 80,
    "60M MIN",
    getCO2Min(),
    "ppm",
    ILI9341_CYAN
  );

  drawMetricCard(
    113, 150, 94, 80,
    "60M AVG",
    getCO2Average(),
    "ppm",
    ILI9341_GREEN
  );

  drawMetricCard(
    216, 150, 94, 80,
    "60M MAX",
    getCO2Max(),
    "ppm",
    ILI9341_ORANGE
  );
}

// ============================================================
// TFT SCREEN 4 — ENVIRONMENT
// SHT45 is the authoritative room temperature / humidity source.
// ============================================================

void drawEnvironmentScreen() {
  drawHeader("ENVIRONMENT - SHT45", ILI9341_DARKGREEN);

  if (!sht45Online || !sht45HasReading) {
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(3);
    tft.setCursor(35, 82);
    tft.print("SHT45 OFFLINE");

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(28, 130);
    tft.print("Check 3.3V / SDA / SCL");
    return;
  }

  // Temperature half
  tft.drawRoundRect(
    10, 52, 145, 130, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(24, 66);
  tft.print("TEMPERATURE");

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(5);
  tft.setCursor(22, 105);
  tft.print(shtTemperature, 1);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(112, 145);
  tft.print("C");

  // Humidity half
  tft.drawRoundRect(
    165, 52, 145, 130, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(185, 66);
  tft.print("HUMIDITY");

  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(5);
  tft.setCursor(178, 105);
  tft.print(shtHumidity, 1);

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(273, 145);
  tft.print("%");

  // Role / compensation status
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(16, 199);
  tft.print("PRIMARY TEMP/RH");

  tft.setTextColor(
    sgp41Online
      ? ILI9341_CYAN
      : ILI9341_YELLOW
  );
  tft.setCursor(145, 199);
  tft.print(
    sgp41Online
      ? "SGP41 COMPENSATED"
      : "SGP41 STARTING"
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(16, 217);
  tft.print("Sampling: 1 Hz");
}

// ============================================================
// TFT SCREEN 5 — VOC / NOx
// ============================================================

void drawGasScreen() {
  drawHeader("VOC & NOx - SGP41", ILI9341_PURPLE);

  if (!sgp41Online) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(3);
    tft.setCursor(45, 78);
    tft.print("SGP41 STARTING");

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(42, 126);

    if (sgpConditioningSecondsRemaining > 0) {
      tft.print("NOx conditioning: ");
      tft.print(sgpConditioningSecondsRemaining);
      tft.print("s");
    } else {
      tft.print("Waiting for gas data");
    }

    return;
  }

  if (sgpConditioningSecondsRemaining > 0) {
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(3);
    tft.setCursor(28, 68);
    tft.print("NOx CONDITIONING");

    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(5);
    tft.setCursor(120, 112);
    tft.print(sgpConditioningSecondsRemaining);

    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setTextSize(2);
    tft.setCursor(94, 168);
    tft.print("seconds left");

    return;
  }

  // VOC Index
  tft.drawRoundRect(
    10, 50, 145, 120, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(33, 65);
  tft.print("VOC INDEX");

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(5);
  tft.setCursor(38, 105);
  tft.print(vocIndex);

  // NOx Index
  tft.drawRoundRect(
    165, 50, 145, 120, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(190, 65);
  tft.print("NOx INDEX");

  tft.setTextColor(ILI9341_ORANGE);
  tft.setTextSize(5);
  tft.setCursor(193, 105);
  tft.print(noxIndex);

  // Raw signal diagnostics
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);

  tft.setCursor(18, 190);
  tft.print("SRAW VOC:");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(srawVoc);

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(170, 190);
  tft.print("SRAW NOx:");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(srawNox);

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(18, 212);
  tft.print("Adaptive Gas Index Algorithm @ 1 Hz");
}

// ============================================================
// TFT SCREEN 6 — SYSTEM
// ============================================================

void drawSystemScreen() {
  drawHeader("SYSTEM", ILI9341_MAROON);

  const int xLabel = 12;
  const int xValue = 116;
  int y = 46;

  // Wi-Fi + RSSI
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Wi-Fi");

  tft.setCursor(xValue, y);
  tft.setTextColor(
    WiFi.status() == WL_CONNECTED
      ? ILI9341_GREEN
      : ILI9341_RED
  );
  tft.print(
    WiFi.status() == WL_CONNECTED
      ? "Connected"
      : "Offline"
  );

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(238, y + 5);
    tft.print(WiFi.RSSI());
    tft.print(" dBm");
  }

  // IP
  y += 28;
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("IP");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(xValue, y);

  if (WiFi.status() == WL_CONNECTED) {
    tft.print(WiFi.localIP());
  } else {
    tft.print("-");
  }

  // Sensor health: use compact labels to fit all four.
  y += 28;
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Sensors");

  tft.setTextSize(1);

  tft.setCursor(xValue, y + 5);
  tft.setTextColor(
    sps30Online ? ILI9341_GREEN : ILI9341_RED
  );
  tft.print(sps30Online ? "SPS OK" : "SPS ERR");

  tft.setCursor(166, y + 5);
  tft.setTextColor(
    scd4xOnline ? ILI9341_GREEN : ILI9341_RED
  );
  tft.print(scd4xOnline ? "CO2 OK" : "CO2 ERR");

  tft.setCursor(216, y + 5);
  tft.setTextColor(
    sht45Online ? ILI9341_GREEN : ILI9341_RED
  );
  tft.print(sht45Online ? "SHT OK" : "SHT ERR");

  tft.setCursor(266, y + 5);
  tft.setTextColor(
    sgp41Online ? ILI9341_GREEN : ILI9341_RED
  );
  tft.print(sgp41Online ? "SGP OK" : "SGP ERR");

  // Environment
  y += 28;
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Env");

  tft.setCursor(xValue, y);
  tft.setTextColor(ILI9341_WHITE);

  if (sht45HasReading) {
    tft.print(shtTemperature, 1);
    tft.print("C ");
    tft.print(shtHumidity, 0);
    tft.print("%");
  } else {
    tft.print("Waiting");
  }

  // Gas indices
  y += 28;
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Gas");

  tft.setCursor(xValue, y);
  tft.setTextColor(ILI9341_WHITE);

  if (sgp41HasReading) {
    tft.print("V");
    tft.print(vocIndex);
    tft.print(" N");
    tft.print(noxIndex);
  } else if (sgpConditioningSecondsRemaining > 0) {
    tft.print("Conditioning");
  } else {
    tft.print("Waiting");
  }

  // History
  y += 28;
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("History");

  tft.setCursor(xValue, y);
  tft.setTextColor(ILI9341_WHITE);
  tft.print(historyCount);
  tft.print("/");
  tft.print(HISTORY_SIZE);
  tft.print(" RAM");

  // Uptime / Telegram
  y += 28;
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(xLabel, y);
  tft.print("Uptime");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(xValue, y);
  tft.print(getUptime());

  tft.setTextSize(1);
  tft.setCursor(250, y + 5);
  tft.setTextColor(
    telegramConfigured()
      ? ILI9341_GREEN
      : ILI9341_YELLOW
  );
  tft.print(
    telegramConfigured()
      ? "TG OK"
      : "TG --"
  );
}

// ============================================================
// WEB DASHBOARD ASSET
// IMPORTANT: large web assets must stay at global PROGMEM scope.
// Never move this inside handleRoot().
// ============================================================

static const char INDEX_HTML[] PROGMEM = R"HTML(
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
  --sec:#667085;
  --border:#e6e8eb;
  --accent:#137333;
  --status:#e6f4ea;
  --soft:#f7f8fa
}
[data-theme=dark]{
  --bg:#101214;
  --card:#1b1e22;
  --text:#f1f3f4;
  --sec:#aeb4bb;
  --border:#30343a;
  --accent:#81c995;
  --status:#183522;
  --soft:#15181c
}
body{
  margin:0;
  font-family:system-ui,-apple-system,Segoe UI,sans-serif;
  background:var(--bg);
  color:var(--text)
}
.container{
  max-width:1050px;
  margin:auto;
  padding:16px
}
.header,.hero,.section,.card{
  background:var(--card);
  border-radius:16px;
  box-shadow:0 2px 8px #0002
}
.header{
  padding:20px;
  margin-bottom:16px;
  position:relative
}
.header h1{margin:0;font-size:24px}
.header p{margin:6px 0 0;color:var(--sec)}
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
.hero{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:1px;
  overflow:hidden;
  margin-bottom:16px;
  background:var(--border)
}
.hero-cell{
  background:var(--card);
  text-align:center;
  padding:24px
}
.hero-label{color:var(--sec);font-size:14px}
.hero-value{font-size:58px;font-weight:750;line-height:1.05;margin:7px 0}
.unit{color:var(--sec);font-size:13px}
.grid3,.stats{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:12px;
  margin-bottom:16px
}
.card,.stat{padding:18px}
.title{font-size:14px;color:var(--sec)}
.value{font-size:26px;font-weight:650;margin-top:3px}
.section{
  padding:20px;
  margin-bottom:16px
}
.section h2{margin:0 0 14px;font-size:18px}
.section-note{
  color:var(--sec);
  font-size:12px;
  margin:-7px 0 12px
}
.stat{
  text-align:center;
  background:var(--soft);
  border-radius:10px
}
.statv{font-size:22px;font-weight:650;margin-top:4px}
.chart{
  width:100%;
  height:300px;
  display:block
}
.row{
  display:flex;
  justify-content:space-between;
  gap:16px;
  padding:8px 0;
  border-bottom:1px solid var(--border)
}
.row:last-child{border:0}
.label{color:var(--sec)}
.status{
  padding:4px 9px;
  border-radius:20px;
  background:var(--status);
  color:var(--accent)
}
.api-note{
  font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
  font-size:12px;
  color:var(--sec);
  overflow-wrap:anywhere
}
@media(max-width:650px){
  .hero,.grid3,.stats{grid-template-columns:1fr}
  .hero-value{font-size:50px}
  .chart{height:250px}
}
</style>
</head>
<body>
<div class="container">

<div class="header">
  <button id="theme" onclick="toggleTheme()">🌙</button>
  <h1>🏠 Air Quality Monitor</h1>
  <p id="dt">Loading device status...</p>
</div>

<div class="hero">
  <div class="hero-cell">
    <div class="hero-label">PM2.5</div>
    <div class="hero-value" id="p25">--</div>
    <div class="unit">µg/m³</div>
  </div>
  <div class="hero-cell">
    <div class="hero-label">CO₂</div>
    <div class="hero-value" id="co2">--</div>
    <div class="unit">ppm</div>
  </div>
</div>

<div class="grid3">
  <div class="card">
    <div class="title">PM1.0</div>
    <div class="value" id="p1">--</div>
    <div class="unit">µg/m³</div>
  </div>
  <div class="card">
    <div class="title">PM10</div>
    <div class="value" id="p10">--</div>
    <div class="unit">µg/m³</div>
  </div>
  <div class="card">
    <div class="title">PM4.0</div>
    <div class="value" id="p4">--</div>
    <div class="unit">µg/m³</div>
  </div>
</div>

<div class="grid3">
  <div class="card">
    <div class="title">SCD4X temperature</div>
    <div class="value" id="temp">--</div>
    <div class="unit">°C</div>
  </div>
  <div class="card">
    <div class="title">SCD4X humidity</div>
    <div class="value" id="hum">--</div>
    <div class="unit">%RH</div>
  </div>
  <div class="card">
    <div class="title">Typical particle size</div>
    <div class="value" id="sz">--</div>
    <div class="unit">µm</div>
  </div>
</div>

<div class="section">
  <h2>Particulate history — last 60 minutes</h2>
  <div class="section-note">True timestamp spacing. PM lines use a 3-sample (~90 second) visual moving average; API data and statistics remain raw.</div>
  <canvas id="pmChart" class="chart"></canvas>
</div>

<div class="section">
  <h2>PM2.5 — 60 minute statistics</h2>
  <div class="stats">
    <div class="stat"><div class="title">Minimum</div><div class="statv" id="a1">--</div></div>
    <div class="stat"><div class="title">Average</div><div class="statv" id="a2">--</div></div>
    <div class="stat"><div class="title">Maximum</div><div class="statv" id="a3">--</div></div>
  </div>
</div>

<div class="section">
  <h2>PM10 — 60 minute statistics</h2>
  <div class="stats">
    <div class="stat"><div class="title">Minimum</div><div class="statv" id="b1">--</div></div>
    <div class="stat"><div class="title">Average</div><div class="statv" id="b2">--</div></div>
    <div class="stat"><div class="title">Maximum</div><div class="statv" id="b3">--</div></div>
  </div>
</div>

<div class="section">
  <h2>CO₂ history — last 60 minutes</h2>
  <div class="section-note">Stable 400–2000 ppm display range; automatically expands upward when necessary.</div>
  <canvas id="co2Chart" class="chart"></canvas>
</div>

<div class="section">
  <h2>CO₂ — 60 minute statistics</h2>
  <div class="stats">
    <div class="stat"><div class="title">Minimum</div><div class="statv" id="c1">--</div></div>
    <div class="stat"><div class="title">Average</div><div class="statv" id="c2">--</div></div>
    <div class="stat"><div class="title">Maximum</div><div class="statv" id="c3">--</div></div>
  </div>
</div>

<div class="section">
  <h2>Particle counts</h2>
  <div class="row"><span class="label">NC0.5</span><span id="n05">--</span></div>
  <div class="row"><span class="label">NC1.0</span><span id="n10">--</span></div>
  <div class="row"><span class="label">NC2.5</span><span id="n25">--</span></div>
  <div class="row"><span class="label">NC4.0</span><span id="n40">--</span></div>
  <div class="row"><span class="label">NC10</span><span id="n100">--</span></div>
</div>

<div class="section">
  <h2>System</h2>
  <div class="row"><span class="label">SPS30</span><span id="spsstatus" class="status">--</span></div>
  <div class="row"><span class="label">SCD4X</span><span id="scdstatus" class="status">--</span></div>
  <div class="row"><span class="label">SPS30 firmware</span><span id="fw">--</span></div>
  <div class="row"><span class="label">Wi-Fi</span><span id="wifi">--</span></div>
  <div class="row"><span class="label">Signal</span><span id="rssi">--</span></div>
  <div class="row"><span class="label">IP</span><span id="ip">--</span></div>
  <div class="row"><span class="label">Uptime</span><span id="up">--</span></div>
  <div class="row"><span class="label">History</span><span id="hc">--</span></div>
  <div class="row"><span class="label">History storage</span><span>RAM only</span></div>
  <div class="row"><span class="label">Monitor</span><span>V1.6.1</span></div>
</div>

<div class="section">
  <h2>Telegram alerts</h2>
  <div class="row"><span class="label">Telegram</span><span id="tg">--</span></div>
  <div class="row"><span class="label">Alerts</span><span id="al">--</span></div>
  <div class="row"><span class="label">PM2.5 threshold</span><span id="at25">--</span></div>
  <div class="row"><span class="label">PM10 threshold</span><span id="at10">--</span></div>
</div>

<div class="section">
  <h2>API cadence</h2>
  <div class="api-note">/api/live → every 5 s · /api/system → every 30 s · /api/history → once at load, then /api/history?after=&lt;epoch&gt; every 30 s</div>
</div>

</div>

<script>
const $=id=>document.getElementById(id);

const H={
  samples:[],
  latest:0,
  capacity:120,
  interval:30
};

function setTheme(t){
  document.documentElement.dataset.theme=t;
  localStorage.theme=t;
  $('theme').textContent=t==='dark'?'☀️':'🌙';
}

setTheme(
  localStorage.theme ||
  (matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light')
);

function toggleTheme(){
  setTheme(document.documentElement.dataset.theme==='dark'?'light':'dark');
  drawPM();
  drawCO2();
}

function formatAxisTime(epoch){
  if(!epoch)return '';
  const d=new Date(epoch*1000);
  return d.toLocaleTimeString([],{
    hour:'2-digit',
    minute:'2-digit',
    hour12:false
  });
}

function canvasSetup(id){
  const c=$(id);
  const ctx=c.getContext('2d');
  const w=c.clientWidth;
  const h=c.clientHeight;
  const d=window.devicePixelRatio||1;
  c.width=Math.max(1,Math.floor(w*d));
  c.height=Math.max(1,Math.floor(h*d));
  ctx.setTransform(d,0,0,d,0,0);
  ctx.clearRect(0,0,w,h);
  return {c,ctx,w,h};
}

function validTimedSamples(){
  return H.samples.filter(s=>s[0]>0);
}

function movingAverage(points,windowSize){
  const out=[];
  for(let i=0;i<points.length;i++){
    let total=0;
    let count=0;
    for(let j=Math.max(0,i-windowSize+1);j<=i;j++){
      total+=points[j][1];
      count++;
    }
    out.push([points[i][0],total/count]);
  }
  return out;
}

function timeDomain(samples){
  const timed=samples.filter(s=>s[0]>0);

  // Do not mix pre-NTP zero timestamps with real epoch timestamps.
  // Once every visible sample has a valid epoch, use proportional time.
  if(timed.length===samples.length && timed.length>=2){
    let min=timed[0][0];
    let max=timed[timed.length-1][0];
    if(max<=min)max=min+1;
    return [min,max,true];
  }

  return [0,Math.max(1,samples.length-1),false];
}

function xFor(sample,index,samples,left,width,domain){
  const [min,max,timed]=domain;
  if(timed && sample[0]>0){
    return left+(sample[0]-min)/(max-min)*width;
  }
  if(samples.length<=1)return left+width/2;
  return left+index/(samples.length-1)*width;
}

function drawTimeAxis(ctx,samples,left,top,width,height,bottom,domain,secColor,borderColor){
  const [min,max,timed]=domain;

  ctx.strokeStyle=borderColor;
  ctx.fillStyle=secColor;
  ctx.font='11px system-ui';

  for(let i=0;i<5;i++){
    const x=left+width*i/4;
    ctx.beginPath();
    ctx.moveTo(x,top);
    ctx.lineTo(x,top+height);
    ctx.stroke();

    ctx.textAlign='center';

    if(timed){
      const ts=min+(max-min)*i/4;
      ctx.fillText(formatAxisTime(ts),x,bottom-9);
    }else if(samples.length){
      const minsAgo=((4-i)*Math.max(0,samples.length-1)*H.interval/60/4);
      ctx.fillText(i===4?'Now':'-'+Math.round(minsAgo)+'m',x,bottom-9);
    }
  }
}

function nicePMMax(maxValue){
  let m=Math.max(10,maxValue*1.15);
  if(m<=50)return Math.ceil(m/10)*10;
  if(m<=200)return Math.ceil(m/25)*25;
  return Math.ceil(m/50)*50;
}

function drawPM(){
  const {ctx,w,h}=canvasSetup('pmChart');
  const samples=H.samples;

  if(!samples.length){
    ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--sec');
    ctx.textAlign='center';
    ctx.fillText('Waiting for particulate history...',w/2,h/2);
    return;
  }

  const L=48,R=14,T=24,B=38;
  const CW=w-L-R,CH=h-T-B;
  const style=getComputedStyle(document.documentElement);
  const sec=style.getPropertyValue('--sec');
  const border=style.getPropertyValue('--border');

  const pm25Raw=samples.map(s=>[s[0],s[1]]);
  const pm10Raw=samples.map(s=>[s[0],s[2]]);
  const pm25=movingAverage(pm25Raw,3);
  const pm10=movingAverage(pm10Raw,3);
  const maxValue=Math.max(
    0,
    ...pm25Raw.map(p=>p[1]),
    ...pm10Raw.map(p=>p[1])
  );
  const yMax=nicePMMax(maxValue);
  const domain=timeDomain(samples);

  ctx.strokeStyle=border;
  ctx.fillStyle=sec;
  ctx.font='11px system-ui';

  for(let i=0;i<=4;i++){
    const y=T+CH-CH*i/4;
    ctx.beginPath();
    ctx.moveTo(L,y);
    ctx.lineTo(w-R,y);
    ctx.stroke();
    ctx.textAlign='right';
    ctx.fillText((yMax*i/4).toFixed(0),L-7,y+4);
  }

  drawTimeAxis(ctx,samples,L,T,CW,CH,h,domain,sec,border);

  function plot(points,color){
    ctx.strokeStyle=color;
    ctx.fillStyle=color;
    ctx.lineWidth=2;

    if(points.length===1){
      const x=xFor(samples[0],0,samples,L,CW,domain);
      const y=T+CH-points[0][1]/yMax*CH;
      ctx.beginPath();
      ctx.arc(x,y,4,0,Math.PI*2);
      ctx.fill();
      return;
    }

    ctx.beginPath();
    points.forEach((p,i)=>{
      const x=xFor(samples[i],i,samples,L,CW,domain);
      const y=T+CH-p[1]/yMax*CH;
      i?ctx.lineTo(x,y):ctx.moveTo(x,y);
    });
    ctx.stroke();
  }

  plot(pm25,'#4caf50');
  plot(pm10,'#ff9800');

  ctx.font='12px system-ui';
  ctx.textAlign='left';
  ctx.fillStyle='#4caf50';
  ctx.fillText('● PM2.5',L,14);
  ctx.fillStyle='#ff9800';
  ctx.fillText('● PM10',L+78,14);
}

function drawCO2(){
  const {ctx,w,h}=canvasSetup('co2Chart');
  const samples=H.samples.filter(s=>s[3]>0);

  if(!samples.length){
    ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--sec');
    ctx.textAlign='center';
    ctx.fillText('Waiting for CO₂ history...',w/2,h/2);
    return;
  }

  const L=54,R=14,T=24,B=38;
  const CW=w-L-R,CH=h-T-B;
  const style=getComputedStyle(document.documentElement);
  const sec=style.getPropertyValue('--sec');
  const border=style.getPropertyValue('--border');

  const values=samples.map(s=>s[3]);
  const observedMax=Math.max(...values);
  const yMin=400;
  const yMax=observedMax>2000
    ? Math.ceil(observedMax/500)*500
    : 2000;
  const domain=timeDomain(samples);

  ctx.strokeStyle=border;
  ctx.fillStyle=sec;
  ctx.font='11px system-ui';

  for(let i=0;i<=4;i++){
    const y=T+CH-CH*i/4;
    const val=yMin+(yMax-yMin)*i/4;
    ctx.beginPath();
    ctx.moveTo(L,y);
    ctx.lineTo(w-R,y);
    ctx.stroke();
    ctx.textAlign='right';
    ctx.fillText(val.toFixed(0),L-7,y+4);
  }

  drawTimeAxis(ctx,samples,L,T,CW,CH,h,domain,sec,border);

  ctx.strokeStyle='#00bcd4';
  ctx.fillStyle='#00bcd4';
  ctx.lineWidth=2;

  if(samples.length===1){
    const x=L+CW/2;
    const y=T+CH-(samples[0][3]-yMin)/(yMax-yMin)*CH;
    ctx.beginPath();
    ctx.arc(x,y,4,0,Math.PI*2);
    ctx.fill();
    return;
  }

  ctx.beginPath();
  samples.forEach((s,i)=>{
    const x=xFor(s,i,samples,L,CW,domain);
    const clamped=Math.max(yMin,Math.min(yMax,s[3]));
    const y=T+CH-(clamped-yMin)/(yMax-yMin)*CH;
    i?ctx.lineTo(x,y):ctx.moveTo(x,y);
  });
  ctx.stroke();

  ctx.font='12px system-ui';
  ctx.textAlign='left';
  ctx.fillText('● CO₂',L,14);
}

function updateStats(stats){
  if(!stats)return;

  $('a1').textContent=stats.pm25[0].toFixed(1);
  $('a2').textContent=stats.pm25[1].toFixed(1);
  $('a3').textContent=stats.pm25[2].toFixed(1);

  $('b1').textContent=stats.pm10[0].toFixed(1);
  $('b2').textContent=stats.pm10[1].toFixed(1);
  $('b3').textContent=stats.pm10[2].toFixed(1);

  $('c1').textContent=stats.co2[0].toFixed(0);
  $('c2').textContent=stats.co2[1].toFixed(0);
  $('c3').textContent=stats.co2[2].toFixed(0);
}

function replaceHistory(d){
  H.samples=Array.isArray(d.samples)?d.samples:[];
  H.capacity=d.capacity||120;
  H.interval=d.interval||30;
  H.latest=d.latest||(
    H.samples.length?H.samples[H.samples.length-1][0]:0
  );

  if(H.samples.length>H.capacity){
    H.samples=H.samples.slice(-H.capacity);
  }

  updateStats(d.stats);
  drawPM();
  drawCO2();
}

function appendHistory(d){
  if(Array.isArray(d.samples) && d.samples.length){
    for(const sample of d.samples){
      const ts=sample[0];

      // Epoch timestamps are unique at a 30-second sampling cadence.
      if(ts>0 && H.samples.some(s=>s[0]===ts))continue;

      H.samples.push(sample);
    }

    if(H.samples.length>H.capacity){
      H.samples=H.samples.slice(-H.capacity);
    }
  }

  H.latest=d.latest||H.latest;
  updateStats(d.stats);
  drawPM();
  drawCO2();
}

async function fetchLive(){
  try{
    const r=await fetch('/api/live',{cache:'no-store'});
    if(!r.ok)throw new Error('HTTP '+r.status);
    const d=await r.json();

    $('p25').textContent=d.pm25.toFixed(1);
    $('p1').textContent=d.pm1.toFixed(1);
    $('p4').textContent=d.pm4.toFixed(1);
    $('p10').textContent=d.pm10.toFixed(1);

    $('co2').textContent=d.co2>0?d.co2:'--';
    $('temp').textContent=d.temperature.toFixed(1);
    $('hum').textContent=d.humidity.toFixed(1);
    $('sz').textContent=d.typical_size.toFixed(2);

    $('n05').textContent=d.nc0_5.toFixed(1);
    $('n10').textContent=d.nc1_0.toFixed(1);
    $('n25').textContent=d.nc2_5.toFixed(1);
    $('n40').textContent=d.nc4_0.toFixed(1);
    $('n100').textContent=d.nc10.toFixed(1);
  }catch(e){
    console.error('Live API error:',e);
  }
}

async function fetchSystem(){
  try{
    const r=await fetch('/api/system',{cache:'no-store'});
    if(!r.ok)throw new Error('HTTP '+r.status);
    const d=await r.json();

    $('dt').textContent=d.datetime;
    $('wifi').textContent=d.wifi;
    $('rssi').textContent=d.rssi+' dBm';
    $('ip').textContent=d.ip;
    $('up').textContent=d.uptime;
    $('hc').textContent=d.history_count+' / '+d.history_capacity;

    $('fw').textContent=d.sps30_firmware;
    $('spsstatus').textContent=d.sps30_online?'Online':'Offline';
    $('scdstatus').textContent=d.scd4x_online
      ?(d.scd4x_has_reading?'Online':'Warming up')
      :'Offline';

    $('tg').textContent=d.telegram_configured?'Configured':'Not configured';
    $('al').textContent=d.alerts_enabled?'Enabled':'Disabled';
    $('at25').textContent=d.pm25_threshold.toFixed(1)+' µg/m³';
    $('at10').textContent=d.pm10_threshold.toFixed(1)+' µg/m³';
  }catch(e){
    console.error('System API error:',e);
  }
}

async function fetchHistory(initial=false){
  try{
    let url='/api/history';

    // If NTP was unavailable and latest is 0, request the complete
    // current ring buffer rather than attempting an invalid delta.
    if(!initial && H.latest>0){
      url+='?after='+encodeURIComponent(H.latest);
    }

    const r=await fetch(url,{cache:'no-store'});
    if(!r.ok)throw new Error('HTTP '+r.status);
    const d=await r.json();

    initial?replaceHistory(d):appendHistory(d);
  }catch(e){
    console.error('History API error:',e);
  }
}

fetchLive();
fetchSystem();
fetchHistory(true);

setInterval(fetchLive,5000);
setInterval(fetchSystem,30000);
setInterval(()=>fetchHistory(false),30000);

addEventListener('resize',()=>{
  drawPM();
  drawCO2();
});
</script>
</body>
</html>
)HTML";

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
    "/api/live",
    HTTP_GET,
    handleApiLive
  );

  server.on(
    "/api/system",
    HTTP_GET,
    handleApiSystem
  );

  // Legacy combined endpoint retained for compatibility.
  // The dashboard no longer polls this endpoint.
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
// Preserved from V1.4.2, version label updated to V1.6.1.
// ============================================================

void handleRoot() {
  // INDEX_HTML lives in flash at global scope. Never instantiate the
  // dashboard inside a request handler: it is ~10 KB and can overflow
  // the ESP32 loop/request stack.
  server.send_P(
    200,
    "text/html; charset=utf-8",
    INDEX_HTML
  );
}


// ============================================================
// API LIVE
// Small high-frequency endpoint. Sensor data only.
// Dashboard cadence: every 5 seconds.
// ============================================================

void handleApiLive() {
  char json[768];

  time_t now;
  time(&now);

  uint32_t epoch =
    (now > 1577836800)
      ? static_cast<uint32_t>(now)
      : 0;

  int n = snprintf(
    json,
    sizeof(json),
    "{\"ts\":%lu,"
    "\"pm1\":%.2f,\"pm25\":%.2f,\"pm4\":%.2f,\"pm10\":%.2f,"
    "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,\"nc10\":%.2f,"
    "\"typical_size\":%.3f,"
    "\"co2\":%u,\"temperature\":%.2f,\"humidity\":%.2f}",
    static_cast<unsigned long>(epoch),
    pm1p0,
    pm2p5,
    pm4p0,
    pm10p0,
    nc0p5,
    nc1p0,
    nc2p5,
    nc4p0,
    nc10p0,
    typicalSize,
    co2ppm,
    scdTemperature,
    scdHumidity
  );

  if (n < 0 || static_cast<size_t>(n) >= sizeof(json)) {
    server.send(500, "text/plain", "Live response overflow");
    return;
  }

  server.setContentLength(
    static_cast<size_t>(n)
  );
  server.send(
    200,
    "application/json",
    ""
  );
  server.sendContent(
    json,
    static_cast<size_t>(n)
  );
}

// ============================================================
// API SYSTEM
// Slow-changing device metadata.
// Dashboard cadence: every 30 seconds.
// ============================================================

void handleApiSystem() {
  char uptime[32];
  char datetime[64];
  char ip[24];
  char json[1024];

  formatUptime(uptime, sizeof(uptime));
  formatCurrentTime(datetime, sizeof(datetime));

  IPAddress addr = WiFi.localIP();

  snprintf(
    ip,
    sizeof(ip),
    "%u.%u.%u.%u",
    addr[0],
    addr[1],
    addr[2],
    addr[3]
  );

  int n = snprintf(
    json,
    sizeof(json),
    "{\"datetime\":\"%s\",\"wifi\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"uptime\":\"%s\","
    "\"sps30_online\":%s,\"sps30_firmware\":\"%u.%u\","
    "\"scd4x_online\":%s,\"scd4x_has_reading\":%s,"
    "\"history_count\":%d,\"history_capacity\":%d,\"history_interval_sec\":%lu,"
    "\"history_storage\":\"RAM only\","
    "\"telegram_configured\":%s,\"alerts_enabled\":%s,"
    "\"pm25_threshold\":%.1f,\"pm10_threshold\":%.1f}",
    datetime,
    WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected",
    WiFi.RSSI(),
    ip,
    uptime,
    sps30Online ? "true" : "false",
    fwMajor,
    fwMinor,
    scd4xOnline ? "true" : "false",
    scd4xHasReading ? "true" : "false",
    historyCount,
    HISTORY_SIZE,
    static_cast<unsigned long>(HISTORY_INTERVAL / 1000UL),
    telegramConfigured() ? "true" : "false",
    alertsEnabled ? "true" : "false",
    pm25AlertThreshold,
    pm10AlertThreshold
  );

  if (n < 0 || static_cast<size_t>(n) >= sizeof(json)) {
    server.send(500, "text/plain", "System response overflow");
    return;
  }

  server.setContentLength(
    static_cast<size_t>(n)
  );
  server.send(
    200,
    "application/json",
    ""
  );
  server.sendContent(
    json,
    static_cast<size_t>(n)
  );
}

// ============================================================
// API STATUS — LEGACY COMPATIBILITY
// Dashboard does not poll this endpoint in V1.6.1.
// ============================================================

void handleApiStatus() {
  char uptime[32];
  char datetime[64];
  char json[1280];

  formatUptime(uptime, sizeof(uptime));
  formatCurrentTime(datetime, sizeof(datetime));

  int n = snprintf(
    json,
    sizeof(json),
    "{\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10\":%.2f,"
    "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,\"nc10\":%.2f,"
    "\"typical_size\":%.3f,\"sps30_online\":%s,\"firmware\":\"%u.%u\","
    "\"co2_ppm\":%u,\"scd_temperature\":%.2f,\"scd_humidity\":%.2f,"
    "\"scd4x_online\":%s,\"scd4x_has_reading\":%s,"
    "\"wifi\":\"%s\",\"rssi\":%d,\"uptime\":\"%s\",\"datetime\":\"%s\","
    "\"history_count\":%d,\"persistent\":false,\"history_storage\":\"RAM only\",\"alerts_enabled\":%s,"
    "\"pm25_threshold\":%.1f,\"pm10_threshold\":%.1f}",
    pm1p0,
    pm2p5,
    pm4p0,
    pm10p0,
    nc0p5,
    nc1p0,
    nc2p5,
    nc4p0,
    nc10p0,
    typicalSize,
    sps30Online ? "true" : "false",
    fwMajor,
    fwMinor,
    co2ppm,
    scdTemperature,
    scdHumidity,
    scd4xOnline ? "true" : "false",
    scd4xHasReading ? "true" : "false",
    WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected",
    WiFi.RSSI(),
    uptime,
    datetime,
    historyCount,
    alertsEnabled ? "true" : "false",
    pm25AlertThreshold,
    pm10AlertThreshold
  );

  if (n < 0 || static_cast<size_t>(n) >= sizeof(json)) {
    server.send(500, "text/plain", "Status response overflow");
    return;
  }

  // Stream a bounded buffer rather than constructing a dynamic String.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent(json, static_cast<size_t>(n));
}

// ============================================================
// API HISTORY
// ============================================================

void handleApiHistory() {
  // Optional incremental cursor:
  //   /api/history?after=<epoch>
  //
  // Samples are compact tuples:
  //   [epoch, PM2.5, PM10, CO2]
  //
  // History remains raw and volatile in RAM. Formatting and graph
  // smoothing are browser responsibilities.

  uint32_t after = 0;

  if (server.hasArg("after")) {
    after = static_cast<uint32_t>(
      strtoul(
        server.arg("after").c_str(),
        nullptr,
        10
      )
    );
  }

  uint32_t latest = 0;

  if (historyCount > 0) {
    int newest =
      (historyWriteIndex - 1 + HISTORY_SIZE) %
      HISTORY_SIZE;

    latest = historyEpoch[newest];
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char chunk[512];
  size_t used = 0;

  auto flushChunk = [&]() {
    if (used > 0) {
      server.sendContent(chunk, used);
      used = 0;
    }
  };

  auto appendText = [&](const char* s) {
    size_t len = strlen(s);

    if (len > sizeof(chunk)) {
      flushChunk();
      server.sendContent(s, len);
      return;
    }

    if (used + len > sizeof(chunk)) {
      flushChunk();
    }

    memcpy(chunk + used, s, len);
    used += len;
  };

  char item[128];

  snprintf(
    item,
    sizeof(item),
    "{\"count\":%d,\"capacity\":%d,\"interval\":%lu,"
    "\"latest\":%lu,\"samples\":[",
    historyCount,
    HISTORY_SIZE,
    static_cast<unsigned long>(
      HISTORY_INTERVAL / 1000UL
    ),
    static_cast<unsigned long>(latest)
  );

  appendText(item);

  bool first = true;

  for (int i = 0; i < historyCount; i++) {
    int k =
      (historyCount < HISTORY_SIZE)
        ? i
        : (historyWriteIndex + i) % HISTORY_SIZE;

    uint32_t ts = historyEpoch[k];

    // If the caller supplied an epoch cursor, return only newer
    // timestamped samples. A zero timestamp is used only when NTP
    // has not synchronized; those are returned on full requests.
    if (
      after > 0 &&
      (ts == 0 || ts <= after)
    ) {
      continue;
    }

    snprintf(
      item,
      sizeof(item),
      "%s[%lu,%.2f,%.2f,%u]",
      first ? "" : ",",
      static_cast<unsigned long>(ts),
      pm25History[k],
      pm10History[k],
      co2History[k]
    );

    appendText(item);
    first = false;
  }

  appendText("],\"stats\":{");

  snprintf(
    item,
    sizeof(item),
    "\"pm25\":[%.2f,%.2f,%.2f],",
    getPM25Min(),
    getPM25Average(),
    getPM25Max()
  );
  appendText(item);

  snprintf(
    item,
    sizeof(item),
    "\"pm10\":[%.2f,%.2f,%.2f],",
    getPM10Min(),
    getPM10Average(),
    getPM10Max()
  );
  appendText(item);

  snprintf(
    item,
    sizeof(item),
    "\"co2\":[%.0f,%.0f,%.0f]",
    getCO2Min(),
    getCO2Average(),
    getCO2Max()
  );
  appendText(item);

  appendText(
    "},\"storage\":\"RAM only\"}"
  );

  flushChunk();
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
      "🏠 Air Monitor V1.6.1\n\n"
      "/status - current air + CO2\n"
      "/co2 - CO2 / temperature / humidity\n"
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
      " µg/m³\n"
      "CO2: " +
      String(co2ppm) +
      " ppm\n"
      "Temperature: " +
      String(scdTemperature, 1) +
      " C\n"
      "Humidity: " +
      String(scdHumidity, 1) +
      " %RH\n\n" +
      getCurrentTime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/co2") {
    String message =
      "🌿 CO2 & ENVIRONMENT\n\n"
      "SCD4X: " +
      String(
        scd4xOnline
          ? "Online"
          : "Offline"
      ) +
      "\nCO2: " +
      String(co2ppm) +
      " ppm\n"
      "Temperature: " +
      String(scdTemperature, 1) +
      " C\n"
      "Humidity: " +
      String(scdHumidity, 1) +
      " %RH\n\n" +
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
      String(getPM10Max(), 1) +
      "\nCO2 Min/Avg/Max: " +
      String(getCO2Min(), 0) +
      " / " +
      String(getCO2Average(), 0) +
      " / " +
      String(getCO2Max(), 0) +
      " ppm";

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
      "Storage: RAM only\n"
      "Cleared on reboot: Yes";

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
      "Monitor: V1.6.1\n"
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
      "\nSCD4X: " +
      String(
        scd4xOnline
          ? "Online"
          : "Offline"
      ) +
      "\nCO2: " +
      String(co2ppm) +
      " ppm"
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

  Serial.println("Monitor: V1.6.1");
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
    "SCD4X: %s\n",
    scd4xOnline
      ? "Online"
      : "Offline"
  );

  if (scd4xHasReading) {
    Serial.printf(
      "CO2: %u ppm | Temp: %.2f C | RH: %.2f %%\n",
      co2ppm,
      scdTemperature,
      scdHumidity
    );
  }

  Serial.printf(
    "History: %d / %d\n",
    historyCount,
    HISTORY_SIZE
  );

  Serial.println(
    "History storage: RAM only (cleared on reboot)"
  );

  Serial.printf(
    "Telegram: %s\n",
    telegramConfigured()
      ? "Configured"
      : "Not configured"
  );

  Serial.println();
}
