/*
  ESP32-S3 Air Quality Monitor — V1.7.2

  Migration from V1.4.2:
    - Web safety fix: dashboard in global PROGMEM; JSON responses bounded/streamed
    - Telegram polling interval: 120 seconds
    - Replaces SSD1306 128x64 I2C OLED with 2.8" ILI9341 240x320 SPI TFT
    - TFT is used in landscape mode: 320x240
    - SPS30, Wi-Fi, mDNS, NTP, web dashboard, Telegram,
      60-minute volatile RAM history and alerts are retained
    - VOC/NOx indices additionally persist to a 7-day LittleFS ring log

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
    - PM/CO2 history remains RAM-only. Only VOC/NOx indices are persisted.
    - V1.7.2 adds SCD4X CO2 / temperature / humidity and uses a five-screen carousel.
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
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSht4x.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

// ============================================================
// ESP32-S3 AIR QUALITY MONITOR V1.7.2
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
// GAS INDEX INTERPRETATION / LEARNING
// ------------------------------------------------------------

bool gasAlgorithmStarted = false;
unsigned long gasAlgorithmStartMillis = 0;

// Sensirion specifies settling / learning times on the order of
// <1.5 h for VOC Index specifications and <6 h for NOx Index.
// We display these as learning-state guidance, not as hard validity gates.
#define VOC_LEARNING_MS (90UL * 60UL * 1000UL)
#define NOX_LEARNING_MS (6UL * 60UL * 60UL * 1000UL)

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
uint16_t vocHistory[HISTORY_SIZE];
uint16_t noxHistory[HISTORY_SIZE];

unsigned long historyTime[HISTORY_SIZE];
uint32_t historyEpoch[HISTORY_SIZE];

int historyCount = 0;
int historyWriteIndex = 0;

// ------------------------------------------------------------
// HISTORY STORAGE POLICY
//
// PM2.5 / PM10 / CO2:
//   - volatile RAM only
//   - 120 x 30-second samples = 60 minutes
//
// VOC / NOx:
//   - same 60-minute RAM ring
//   - additionally stored as a 7-day LittleFS circular log
//   - only interpreted indices are persisted; SRAW values are not
//   - 30-second records are queued in RAM and flushed every 5 minutes
// ------------------------------------------------------------

#define GAS_PERSIST_MAGIC 0x47415332UL
#define GAS_PERSIST_VERSION 1
#define GAS_PERSIST_DAYS 7
#define GAS_PERSIST_CAPACITY (GAS_PERSIST_DAYS * 24UL * 60UL * 2UL)
#define GAS_PERSIST_FLUSH_INTERVAL (5UL * 60UL * 1000UL)
#define GAS_PERSIST_QUEUE_SIZE 16

const char* GAS_HISTORY_FILE = "/gas7d.bin";

enum GasRecordFlags : uint8_t {
  GAS_FLAG_REBOOT       = 0x01,
  GAS_FLAG_VOC_LEARNING = 0x02,
  GAS_FLAG_NOX_LEARNING = 0x04
};

struct __attribute__((packed)) GasPersistRecord {
  uint32_t epoch;
  uint16_t voc;
  uint16_t nox;
  uint8_t flags;
};

struct GasPersistHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t recordSize;
  uint32_t capacity;
  uint32_t count;
  uint32_t writeIndex;
  uint32_t checksum;
};

GasPersistHeader gasPersistHeader{};

GasPersistRecord gasPersistQueue[GAS_PERSIST_QUEUE_SIZE];
uint8_t gasPersistQueueCount = 0;

bool gasStorageReady = false;
bool gasRebootMarkerPending = true;

unsigned long lastGasPersistFlush = 0;

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

bool initGasStorage();
bool flushGasPersistence();
void updateGasPersistence();
void queueGasPersistence(
  uint32_t epoch,
  uint16_t voc,
  uint16_t nox,
  uint8_t flags
);
uint32_t gasHeaderChecksum(const GasPersistHeader& h);
uint8_t currentGasRecordFlags();

int historyIndexFromNewest(int offset);
uint16_t getVOC5MinutePeak();
uint16_t getNOx5MinutePeak();
int8_t getVOCTrend();
int8_t getNOxTrend();
const char* getVOCStatus(int32_t value);
const char* getNOxStatus(int32_t value);
const char* getTrendLabel(int8_t trend);
const char* getGasEventHint();
bool vocLearning();
bool noxLearning();
uint16_t vocLearningMinutesRemaining();
uint16_t noxLearningMinutesRemaining();


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
void handleApiGasHistory();
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
  Serial.println("                  V1.7.2");
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
  tft.print("V1.7.2");

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(80, 195);
  tft.print("Initializing...");

  // ----------------------------------------------------------
  // HISTORY
  // ----------------------------------------------------------

  // PM / CO2 history stays volatile. VOC / NOx additionally use
  // a compact 7-day LittleFS circular log.
  if (LittleFS.begin(true)) {
    gasStorageReady = initGasStorage();

    Serial.printf(
      "Gas persistence: %s\n",
      gasStorageReady
        ? "7-day LittleFS ring ready"
        : "unavailable"
    );
  } else {
    Serial.println(
      "WARNING: LittleFS mount failed. VOC/NOx persistence disabled."
    );
    gasStorageReady = false;
  }

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
  updateGasPersistence();

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
    if (!gasAlgorithmStarted) {
      gasAlgorithmStarted = true;
      gasAlgorithmStartMillis = millis();

      Serial.println(
        "SGP41 Gas Index learning timers started."
      );
    }

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

  vocHistory[historyWriteIndex] =
    sgp41HasReading
      ? static_cast<uint16_t>(
          constrain(vocIndex, 0L, 500L)
        )
      : 0;

  noxHistory[historyWriteIndex] =
    sgp41HasReading
      ? static_cast<uint16_t>(
          constrain(noxIndex, 0L, 500L)
        )
      : 0;

  historyTime[historyWriteIndex] = millis();

  time_t now;
  time(&now);

  uint32_t epoch =
    (now > 1577836800)
      ? static_cast<uint32_t>(now)
      : 0;

  historyEpoch[historyWriteIndex] = epoch;

  // Persist only mature-enough interpreted gas samples with a
  // real timestamp. Learning state is retained as record flags.
  if (
    gasStorageReady &&
    sgp41HasReading &&
    epoch > 0
  ) {
    queueGasPersistence(
      epoch,
      vocHistory[historyWriteIndex],
      noxHistory[historyWriteIndex],
      currentGasRecordFlags()
    );
  }

  // Advance circular write position. Once full, the oldest sample
  // is overwritten by the newest sample.
  historyWriteIndex =
    (historyWriteIndex + 1) % HISTORY_SIZE;

  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
}

// ============================================================
// GAS PERSISTENCE
// ============================================================

uint32_t gasHeaderChecksum(
  const GasPersistHeader& h
) {
  const uint8_t* p =
    reinterpret_cast<const uint8_t*>(&h);

  size_t n =
    sizeof(GasPersistHeader) -
    sizeof(uint32_t);

  uint32_t hash = 2166136261UL;

  for (size_t i = 0; i < n; i++) {
    hash ^= p[i];
    hash *= 16777619UL;
  }

  return hash;
}

bool initGasStorage() {
  GasPersistHeader h{};

  if (LittleFS.exists(GAS_HISTORY_FILE)) {
    File f =
      LittleFS.open(
        GAS_HISTORY_FILE,
        "r"
      );

    if (f) {
      size_t n =
        f.read(
          reinterpret_cast<uint8_t*>(&h),
          sizeof(h)
        );

      f.close();

      bool valid =
        n == sizeof(h) &&
        h.magic == GAS_PERSIST_MAGIC &&
        h.version == GAS_PERSIST_VERSION &&
        h.recordSize == sizeof(GasPersistRecord) &&
        h.capacity == GAS_PERSIST_CAPACITY &&
        h.count <= GAS_PERSIST_CAPACITY &&
        h.writeIndex < GAS_PERSIST_CAPACITY &&
        h.checksum == gasHeaderChecksum(h);

      if (valid) {
        gasPersistHeader = h;

        Serial.printf(
          "Restored gas log metadata: %lu / %lu records.\\n",
          static_cast<unsigned long>(
            gasPersistHeader.count
          ),
          static_cast<unsigned long>(
            gasPersistHeader.capacity
          )
        );

        lastGasPersistFlush = millis();
        return true;
      }

      Serial.println(
        "WARNING: Gas history metadata invalid; recreating gas log."
      );

      LittleFS.remove(GAS_HISTORY_FILE);
    }
  }

  gasPersistHeader = {};

  gasPersistHeader.magic =
    GAS_PERSIST_MAGIC;

  gasPersistHeader.version =
    GAS_PERSIST_VERSION;

  gasPersistHeader.recordSize =
    sizeof(GasPersistRecord);

  gasPersistHeader.capacity =
    GAS_PERSIST_CAPACITY;

  gasPersistHeader.count = 0;
  gasPersistHeader.writeIndex = 0;

  gasPersistHeader.checksum =
    gasHeaderChecksum(gasPersistHeader);

  File f =
    LittleFS.open(
      GAS_HISTORY_FILE,
      "w"
    );

  if (!f) {
    return false;
  }

  size_t n =
    f.write(
      reinterpret_cast<const uint8_t*>(
        &gasPersistHeader
      ),
      sizeof(gasPersistHeader)
    );

  f.flush();
  f.close();

  lastGasPersistFlush = millis();

  return n == sizeof(gasPersistHeader);
}

void queueGasPersistence(
  uint32_t epoch,
  uint16_t voc,
  uint16_t nox,
  uint8_t flags
) {
  if (!gasStorageReady) {
    return;
  }

  // Reserve a reboot marker on the first persisted record after boot.
  if (gasRebootMarkerPending) {
    flags |= GAS_FLAG_REBOOT;
    gasRebootMarkerPending = false;
  }

  if (
    gasPersistQueueCount >=
    GAS_PERSIST_QUEUE_SIZE
  ) {
    if (!flushGasPersistence()) {
      Serial.println(
        "WARNING: Gas persist queue full; dropping one gas record."
      );

      return;
    }
  }

  GasPersistRecord& r =
    gasPersistQueue[
      gasPersistQueueCount++
    ];

  r.epoch = epoch;
  r.voc = voc;
  r.nox = nox;
  r.flags = flags;
}

bool flushGasPersistence() {
  if (
    !gasStorageReady ||
    gasPersistQueueCount == 0
  ) {
    return true;
  }

  File f =
    LittleFS.open(
      GAS_HISTORY_FILE,
      "r+"
    );

  if (!f) {
    Serial.println(
      "WARNING: Could not open gas log for update."
    );

    return false;
  }

  GasPersistHeader next =
    gasPersistHeader;

  bool ok = true;

  for (
    uint8_t i = 0;
    i < gasPersistQueueCount;
    i++
  ) {
    uint32_t recordIndex =
      next.writeIndex;

    size_t offset =
      sizeof(GasPersistHeader) +
      static_cast<size_t>(recordIndex) *
      sizeof(GasPersistRecord);

    if (
      !f.seek(
        offset,
        SeekSet
      )
    ) {
      ok = false;
      break;
    }

    size_t written =
      f.write(
        reinterpret_cast<const uint8_t*>(
          &gasPersistQueue[i]
        ),
        sizeof(GasPersistRecord)
      );

    if (
      written !=
      sizeof(GasPersistRecord)
    ) {
      ok = false;
      break;
    }

    next.writeIndex =
      (next.writeIndex + 1) %
      GAS_PERSIST_CAPACITY;

    if (
      next.count <
      GAS_PERSIST_CAPACITY
    ) {
      next.count++;
    }
  }

  if (ok) {
    next.checksum =
      gasHeaderChecksum(next);

    if (!f.seek(0, SeekSet)) {
      ok = false;
    } else {
      size_t written =
        f.write(
          reinterpret_cast<const uint8_t*>(
            &next
          ),
          sizeof(next)
        );

      ok =
        written ==
        sizeof(next);
    }
  }

  f.flush();
  f.close();

  if (!ok) {
    Serial.println(
      "WARNING: VOC/NOx persistence flush failed."
    );

    return false;
  }

  gasPersistHeader = next;
  gasPersistQueueCount = 0;
  lastGasPersistFlush = millis();

  Serial.printf(
    "VOC/NOx checkpoint: %lu records stored, 0 queued.\\n",
    static_cast<unsigned long>(
      gasPersistHeader.count
    )
  );

  return true;
}

void updateGasPersistence() {
  if (
    !gasStorageReady ||
    gasPersistQueueCount == 0
  ) {
    return;
  }

  if (
    millis() - lastGasPersistFlush >=
    GAS_PERSIST_FLUSH_INTERVAL
  ) {
    flushGasPersistence();
  }
}

// ============================================================
// GAS INTERPRETATION
// ============================================================

bool vocLearning() {
  return
    !gasAlgorithmStarted ||
    millis() - gasAlgorithmStartMillis <
    VOC_LEARNING_MS;
}

bool noxLearning() {
  return
    !gasAlgorithmStarted ||
    millis() - gasAlgorithmStartMillis <
    NOX_LEARNING_MS;
}

uint16_t vocLearningMinutesRemaining() {
  if (!gasAlgorithmStarted) {
    return 90;
  }

  unsigned long elapsed =
    millis() - gasAlgorithmStartMillis;

  if (elapsed >= VOC_LEARNING_MS) {
    return 0;
  }

  return static_cast<uint16_t>(
    (VOC_LEARNING_MS - elapsed + 59999UL) /
    60000UL
  );
}

uint16_t noxLearningMinutesRemaining() {
  if (!gasAlgorithmStarted) {
    return 360;
  }

  unsigned long elapsed =
    millis() - gasAlgorithmStartMillis;

  if (elapsed >= NOX_LEARNING_MS) {
    return 0;
  }

  return static_cast<uint16_t>(
    (NOX_LEARNING_MS - elapsed + 59999UL) /
    60000UL
  );
}

uint8_t currentGasRecordFlags() {
  uint8_t flags = 0;

  if (vocLearning()) {
    flags |= GAS_FLAG_VOC_LEARNING;
  }

  if (noxLearning()) {
    flags |= GAS_FLAG_NOX_LEARNING;
  }

  return flags;
}

int historyIndexFromNewest(int offset) {
  if (
    offset < 0 ||
    offset >= historyCount
  ) {
    return -1;
  }

  return
    (
      historyWriteIndex -
      1 -
      offset +
      HISTORY_SIZE * 2
    ) %
    HISTORY_SIZE;
}

uint16_t getVOC5MinutePeak() {
  uint16_t peak =
    sgp41HasReading
      ? static_cast<uint16_t>(
          constrain(vocIndex, 0L, 500L)
        )
      : 0;

  int samples =
    min(historyCount, 10);

  for (int i = 0; i < samples; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      vocHistory[k] > peak
    ) {
      peak = vocHistory[k];
    }
  }

  return peak;
}

uint16_t getNOx5MinutePeak() {
  uint16_t peak =
    sgp41HasReading
      ? static_cast<uint16_t>(
          constrain(noxIndex, 0L, 500L)
        )
      : 0;

  int samples =
    min(historyCount, 10);

  for (int i = 0; i < samples; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      noxHistory[k] > peak
    ) {
      peak = noxHistory[k];
    }
  }

  return peak;
}

int8_t getVOCTrend() {
  if (historyCount < 6) {
    return 0;
  }

  float recent = 0;
  float prior = 0;
  int recentCount = 0;
  int priorCount = 0;

  // Last ~1 minute.
  for (int i = 0; i < 2; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      vocHistory[k] > 0
    ) {
      recent += vocHistory[k];
      recentCount++;
    }
  }

  // Roughly 1.5–3 minutes ago.
  for (int i = 3; i < 7; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      vocHistory[k] > 0
    ) {
      prior += vocHistory[k];
      priorCount++;
    }
  }

  if (
    !recentCount ||
    !priorCount
  ) {
    return 0;
  }

  float diff =
    recent / recentCount -
    prior / priorCount;

  if (diff >= 10.0f) return 1;
  if (diff <= -10.0f) return -1;

  return 0;
}

int8_t getNOxTrend() {
  if (historyCount < 6) {
    return 0;
  }

  float recent = 0;
  float prior = 0;
  int recentCount = 0;
  int priorCount = 0;

  for (int i = 0; i < 2; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      noxHistory[k] > 0
    ) {
      recent += noxHistory[k];
      recentCount++;
    }
  }

  for (int i = 3; i < 7; i++) {
    int k =
      historyIndexFromNewest(i);

    if (
      k >= 0 &&
      noxHistory[k] > 0
    ) {
      prior += noxHistory[k];
      priorCount++;
    }
  }

  if (
    !recentCount ||
    !priorCount
  ) {
    return 0;
  }

  float diff =
    recent / recentCount -
    prior / priorCount;

  if (diff >= 3.0f) return 1;
  if (diff <= -3.0f) return -1;

  return 0;
}

const char* getVOCStatus(
  int32_t value
) {
  if (value <= 100) return "AT/BELOW BASE";
  if (value <= 150) return "MILD";
  if (value <= 250) return "ELEVATED";
  if (value <= 400) return "HIGH";

  return "VERY HIGH";
}

const char* getNOxStatus(
  int32_t value
) {
  if (value <= 1) return "BASELINE";
  if (value <= 20) return "MINOR EVENT";
  if (value <= 150) return "ELEVATED";
  if (value <= 300) return "HIGH";

  return "VERY HIGH";
}

const char* getTrendLabel(
  int8_t trend
) {
  if (trend > 0) return "RISING";
  if (trend < 0) return "RECOVERING";

  return "STABLE";
}

const char* getGasEventHint() {
  if (!sgp41HasReading) {
    return "Gas sensor warming up";
  }

  if (
    noxIndex > 20 &&
    vocIndex > 150 &&
    pm2p5 >= pm25AlertThreshold
  ) {
    return "Possible cooking/smoke event";
  }

  if (
    noxIndex > 20 &&
    vocIndex > 150
  ) {
    return "Possible gas/combustion event";
  }

  if (
    vocIndex > 150 &&
    noxIndex <= 20
  ) {
    return "VOC event detected";
  }

  if (
    co2ppm >= 1000 &&
    vocIndex <= 150 &&
    noxIndex <= 20 &&
    pm2p5 < pm25AlertThreshold
  ) {
    return "Ventilation / occupancy signal";
  }

  if (
    vocIndex <= 100 &&
    noxIndex <= 1 &&
    co2ppm < 800
  ) {
    return "Air stable";
  }

  return "Monitoring indoor gases";
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
  tft.print("V1.7.2");
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
  drawHeader("GAS QUALITY - SGP41", ILI9341_PURPLE);

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

  int8_t vocTrend =
    getVOCTrend();

  int8_t noxTrend =
    getNOxTrend();

  uint16_t vocPeak =
    getVOC5MinutePeak();

  uint16_t noxPeak =
    getNOx5MinutePeak();

  // VOC card
  tft.drawRoundRect(
    10, 48, 145, 124, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(33, 60);
  tft.print("VOC INDEX");

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(5);
  tft.setCursor(36, 88);
  tft.print(vocIndex);

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(20, 132);
  tft.print(getVOCStatus(vocIndex));

  tft.setCursor(20, 149);
  tft.print(getTrendLabel(vocTrend));
  tft.print("  P5:");
  tft.print(vocPeak);

  // NOx card
  tft.drawRoundRect(
    165, 48, 145, 124, 10,
    ILI9341_DARKGREY
  );

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(190, 60);
  tft.print("NOx INDEX");

  tft.setTextColor(ILI9341_ORANGE);
  tft.setTextSize(5);
  tft.setCursor(193, 88);
  tft.print(noxIndex);

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(175, 132);
  tft.print(getNOxStatus(noxIndex));

  tft.setCursor(175, 149);
  tft.print(getTrendLabel(noxTrend));
  tft.print("  P5:");
  tft.print(noxPeak);

  // Learning state
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(12, 184);

  if (vocLearning()) {
    tft.print("VOC LEARNING ");
    tft.print(vocLearningMinutesRemaining());
    tft.print("m");
  } else {
    tft.setTextColor(ILI9341_GREEN);
    tft.print("VOC LEARNED");
  }

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(170, 184);

  if (noxLearning()) {
    tft.print("NOx LEARNING ");
    tft.print(noxLearningMinutesRemaining());
    tft.print("m");
  } else {
    tft.setTextColor(ILI9341_GREEN);
    tft.print("NOx LEARNED");
  }

  // Context hint
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(12, 205);
  tft.print(getGasEventHint());

  tft.setTextColor(
    gasStorageReady
      ? ILI9341_GREEN
      : ILI9341_RED
  );
  tft.setCursor(12, 221);
  tft.print(
    gasStorageReady
      ? "7-day gas log ON"
      : "7-day gas log OFF"
  );
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

  tft.setTextSize(1);
  tft.setCursor(250, y + 5);
  tft.setTextColor(
    gasStorageReady
      ? ILI9341_GREEN
      : ILI9341_RED
  );
  tft.print(
    gasStorageReady
      ? "G7D"
      : "G--"
  );

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
    <div class="title">SHT45 temperature</div>
    <div class="value" id="temp">--</div>
    <div class="unit">°C</div>
  </div>
  <div class="card">
    <div class="title">SHT45 humidity</div>
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
  <h2>Gas quality — SGP41</h2>
  <div class="stats">
    <div class="stat">
      <div class="title">VOC Index</div>
      <div class="statv" id="voc">--</div>
      <div class="unit" id="vocstate">--</div>
    </div>
    <div class="stat">
      <div class="title">NOx Index</div>
      <div class="statv" id="nox">--</div>
      <div class="unit" id="noxstate">--</div>
    </div>
    <div class="stat">
      <div class="title">Current interpretation</div>
      <div class="statv" id="gashint" style="font-size:16px">--</div>
    </div>
  </div>
  <div class="row"><span class="label">VOC trend / 5-min peak</span><span id="voctrend">--</span></div>
  <div class="row"><span class="label">NOx trend / 5-min peak</span><span id="noxtrend">--</span></div>
  <div class="row"><span class="label">VOC learning</span><span id="voclearn">--</span></div>
  <div class="row"><span class="label">NOx learning</span><span id="noxlearn">--</span></div>
  <div class="row"><span class="label">Persistent gas history</span><span id="gaspersist">--</span></div>
  <details style="margin-top:12px">
    <summary class="label">Diagnostics</summary>
    <div class="row"><span class="label">Raw VOC signal</span><span id="srawvoc">--</span></div>
    <div class="row"><span class="label">Raw NOx signal</span><span id="srawnox">--</span></div>
  </details>
</div>

<div class="section">
  <h2>Persistent gas history</h2>
  <div class="section-note">VOC/NOx indices only. 30-second records are batched to flash every 5 minutes; up to 7 days are retained.</div>
  <div style="display:flex;gap:8px;margin-bottom:12px;position:relative">
    <button onclick="loadGasHistory(1)" style="position:static;width:auto;height:auto;border-radius:8px;padding:7px 12px">1h</button>
    <button onclick="loadGasHistory(24)" style="position:static;width:auto;height:auto;border-radius:8px;padding:7px 12px">24h</button>
    <button onclick="loadGasHistory(168)" style="position:static;width:auto;height:auto;border-radius:8px;padding:7px 12px">7d</button>
  </div>
  <div class="title" style="margin-bottom:6px">VOC Index</div>
  <canvas id="vocHistoryChart" class="chart" style="height:230px"></canvas>
  <div class="title" style="margin:16px 0 6px">NOx Index</div>
  <canvas id="noxHistoryChart" class="chart" style="height:230px"></canvas>
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
  <div class="row"><span class="label">SHT45</span><span id="shtstatus" class="status">--</span></div>
  <div class="row"><span class="label">SGP41</span><span id="sgpstatus" class="status">--</span></div>
  <div class="row"><span class="label">SPS30 firmware</span><span id="fw">--</span></div>
  <div class="row"><span class="label">Wi-Fi</span><span id="wifi">--</span></div>
  <div class="row"><span class="label">Signal</span><span id="rssi">--</span></div>
  <div class="row"><span class="label">IP</span><span id="ip">--</span></div>
  <div class="row"><span class="label">Uptime</span><span id="up">--</span></div>
  <div class="row"><span class="label">History</span><span id="hc">--</span></div>
  <div class="row"><span class="label">History storage</span><span>RAM only</span></div>
  <div class="row"><span class="label">Monitor</span><span>V1.7.2</span></div>
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
  <div class="api-note">/api/live → every 5 s · /api/system → every 30 s · /api/history → PM/CO2 RAM history · /api/gas-history → persisted VOC/NOx (1h/24h/7d)</div>
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

    $('temp').textContent=d.sht45_has_reading
      ? d.temperature.toFixed(1)
      : '--';

    $('hum').textContent=d.sht45_has_reading
      ? d.humidity.toFixed(1)
      : '--';

    $('sz').textContent=d.typical_size.toFixed(2);

    $('voc').textContent=d.sgp41_has_reading
      ? d.voc_index
      : '--';

    $('nox').textContent=d.sgp41_has_reading
      ? d.nox_index
      : '--';

    $('vocstate').textContent=d.sgp41_has_reading
      ? d.voc_status
      : '--';

    $('noxstate').textContent=d.sgp41_has_reading
      ? d.nox_status
      : '--';

    $('gashint').textContent=d.gas_hint||'--';

    $('voctrend').textContent=d.sgp41_has_reading
      ? d.voc_trend+' / '+d.voc_peak_5m
      : '--';

    $('noxtrend').textContent=d.sgp41_has_reading
      ? d.nox_trend+' / '+d.nox_peak_5m
      : '--';

    $('voclearn').textContent=d.voc_learning
      ? d.voc_learning_minutes+' min remaining'
      : 'Learned';

    $('noxlearn').textContent=d.nox_learning
      ? d.nox_learning_minutes+' min remaining'
      : 'Learned';

    $('srawvoc').textContent=d.sgp41_online?d.sraw_voc:'--';
    $('srawnox').textContent=d.sgp41_has_reading?d.sraw_nox:'--';

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
    $('gaspersist').textContent=d.gas_persistence_ready
      ? d.gas_persist_count+' records / '+d.gas_persist_queued+' queued'
      : 'Unavailable';

    $('fw').textContent=d.sps30_firmware;
    $('spsstatus').textContent=d.sps30_online?'Online':'Offline';
    $('scdstatus').textContent=d.scd4x_online
      ?(d.scd4x_has_reading?'Online':'Warming up')
      :'Offline';

    $('shtstatus').textContent=d.sht45_online
      ?(d.sht45_has_reading?'Online':'Warming up')
      :'Offline';

    $('sgpstatus').textContent=d.sgp41_online
      ?(
          d.sgp41_has_reading
            ? 'Online'
            : (
                d.sgp41_conditioning_sec>0
                  ? 'Conditioning'
                  : 'Warming up'
              )
        )
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

const GH={
  hours:24,
  samples:[]
};

function formatGasAxisTime(epoch){
  if(!epoch)return '';
  const d=new Date(epoch*1000);

  if(GH.hours>24){
    return d.toLocaleDateString([],{
      day:'2-digit',
      month:'short'
    });
  }

  return d.toLocaleTimeString([],{
    hour:'2-digit',
    minute:'2-digit',
    hour12:false
  });
}

function drawPersistentGasChart(id,valueIndex,label){
  const {ctx,w,h}=canvasSetup(id);
  const samples=GH.samples;

  if(!samples.length){
    ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--sec');
    ctx.textAlign='center';
    ctx.fillText('No persisted gas history yet',w/2,h/2);
    return;
  }

  const L=52,R=14,T=24,B=38;
  const CW=w-L-R,CH=h-T-B;
  const style=getComputedStyle(document.documentElement);
  const sec=style.getPropertyValue('--sec');
  const border=style.getPropertyValue('--border');

  const vals=samples.map(s=>s[valueIndex]);
  const observedMax=Math.max(1,...vals);

  let yMin=0;
  let yMax=500;

  // Keep NOx readable when the room is near baseline.
  if(valueIndex===2 && observedMax<=50){
    yMax=50;
  }

  const minTs=samples[0][0];
  const maxTs=Math.max(minTs+1,samples[samples.length-1][0]);

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

  for(let i=0;i<5;i++){
    const x=L+CW*i/4;
    const ts=minTs+(maxTs-minTs)*i/4;

    ctx.textAlign='center';
    ctx.fillText(formatGasAxisTime(ts),x,h-9);
  }

  ctx.strokeStyle=valueIndex===1?'#00bcd4':'#ff9800';
  ctx.lineWidth=2;
  ctx.beginPath();

  samples.forEach((s,i)=>{
    const x=L+(s[0]-minTs)/(maxTs-minTs)*CW;
    const value=Math.max(yMin,Math.min(yMax,s[valueIndex]));
    const y=T+CH-(value-yMin)/(yMax-yMin)*CH;

    i?ctx.lineTo(x,y):ctx.moveTo(x,y);
  });

  ctx.stroke();

  // Reboot markers are flags bit 0.
  ctx.strokeStyle=border;
  for(const s of samples){
    if((s[3]&1)!==0){
      const x=L+(s[0]-minTs)/(maxTs-minTs)*CW;
      ctx.beginPath();
      ctx.moveTo(x,T);
      ctx.lineTo(x,T+CH);
      ctx.stroke();
    }
  }

  ctx.fillStyle=sec;
  ctx.textAlign='left';
  ctx.fillText(label,L,14);
}

async function loadGasHistory(hours=24){
  GH.hours=hours;

  try{
    const r=await fetch(
      '/api/gas-history?hours='+encodeURIComponent(hours),
      {cache:'no-store'}
    );

    if(!r.ok)throw new Error('HTTP '+r.status);

    const d=await r.json();
    GH.samples=Array.isArray(d.samples)?d.samples:[];

    drawPersistentGasChart('vocHistoryChart',1,'VOC Index');
    drawPersistentGasChart('noxHistoryChart',2,'NOx Index');
  }catch(e){
    console.error('Gas history API error:',e);
  }
}

fetchLive();
fetchSystem();
fetchHistory(true);
loadGasHistory(24);

setInterval(fetchLive,5000);
setInterval(fetchSystem,30000);
setInterval(()=>fetchHistory(false),30000);
setInterval(()=>loadGasHistory(GH.hours),300000);

addEventListener('resize',()=>{
  drawPM();
  drawCO2();
  drawPersistentGasChart('vocHistoryChart',1,'VOC Index');
  drawPersistentGasChart('noxHistoryChart',2,'NOx Index');
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

  server.on(
    "/api/gas-history",
    HTTP_GET,
    handleApiGasHistory
  );

  server.onNotFound(
    handleNotFound
  );

  server.begin();

  Serial.println("Web server started.");
}

// ============================================================
// WEB DASHBOARD
// Preserved from V1.4.2, version label updated to V1.7.2.
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
  char json[1800];

  time_t now;
  time(&now);

  uint32_t epoch =
    (now > 1577836800)
      ? static_cast<uint32_t>(now)
      : 0;

  int8_t vocTrend =
    getVOCTrend();

  int8_t noxTrend =
    getNOxTrend();

  int n = snprintf(
    json,
    sizeof(json),
    "{\"ts\":%lu,"
    "\"pm1\":%.2f,\"pm25\":%.2f,\"pm4\":%.2f,\"pm10\":%.2f,"
    "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,\"nc10\":%.2f,"
    "\"typical_size\":%.3f,"
    "\"co2\":%u,"
    "\"temperature\":%.2f,\"humidity\":%.2f,"
    "\"sht_temperature\":%.2f,\"sht_humidity\":%.2f,"
    "\"sht45_online\":%s,\"sht45_has_reading\":%s,"
    "\"voc_index\":%ld,\"nox_index\":%ld,"
    "\"voc_status\":\"%s\",\"nox_status\":\"%s\","
    "\"voc_trend\":\"%s\",\"nox_trend\":\"%s\","
    "\"voc_peak_5m\":%u,\"nox_peak_5m\":%u,"
    "\"voc_learning\":%s,\"nox_learning\":%s,"
    "\"voc_learning_minutes\":%u,\"nox_learning_minutes\":%u,"
    "\"gas_hint\":\"%s\","
    "\"sraw_voc\":%u,\"sraw_nox\":%u,"
    "\"sgp41_online\":%s,\"sgp41_has_reading\":%s,"
    "\"sgp41_conditioning_sec\":%u}",
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
    shtTemperature,
    shtHumidity,
    shtTemperature,
    shtHumidity,
    sht45Online ? "true" : "false",
    sht45HasReading ? "true" : "false",
    static_cast<long>(vocIndex),
    static_cast<long>(noxIndex),
    getVOCStatus(vocIndex),
    getNOxStatus(noxIndex),
    getTrendLabel(vocTrend),
    getTrendLabel(noxTrend),
    getVOC5MinutePeak(),
    getNOx5MinutePeak(),
    vocLearning() ? "true" : "false",
    noxLearning() ? "true" : "false",
    vocLearningMinutesRemaining(),
    noxLearningMinutesRemaining(),
    getGasEventHint(),
    srawVoc,
    srawNox,
    sgp41Online ? "true" : "false",
    sgp41HasReading ? "true" : "false",
    sgpConditioningSecondsRemaining
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
  char json[1500];

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
    "\"sht45_online\":%s,\"sht45_has_reading\":%s,"
    "\"sgp41_online\":%s,\"sgp41_has_reading\":%s,\"sgp41_conditioning_sec\":%u,"
    "\"history_count\":%d,\"history_capacity\":%d,\"history_interval_sec\":%lu,"
    "\"history_storage\":\"RAM only\","
    "\"gas_persistence_ready\":%s,"
    "\"gas_persist_count\":%lu,\"gas_persist_capacity\":%lu,"
    "\"gas_persist_queued\":%u,\"gas_persist_flush_sec\":%lu,"
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
    sht45Online ? "true" : "false",
    sht45HasReading ? "true" : "false",
    sgp41Online ? "true" : "false",
    sgp41HasReading ? "true" : "false",
    sgpConditioningSecondsRemaining,
    historyCount,
    HISTORY_SIZE,
    static_cast<unsigned long>(
      HISTORY_INTERVAL / 1000UL
    ),
    gasStorageReady ? "true" : "false",
    static_cast<unsigned long>(
      gasPersistHeader.count
    ),
    static_cast<unsigned long>(
      gasPersistHeader.capacity
    ),
    gasPersistQueueCount,
    static_cast<unsigned long>(
      GAS_PERSIST_FLUSH_INTERVAL / 1000UL
    ),
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
// Dashboard does not poll this endpoint in V1.7.2.
// ============================================================

void handleApiStatus() {
  char uptime[32];
  char datetime[64];
  char json[1800];

  formatUptime(uptime, sizeof(uptime));
  formatCurrentTime(datetime, sizeof(datetime));

  int n = snprintf(
    json,
    sizeof(json),
    "{\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10\":%.2f,"
    "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,\"nc10\":%.2f,"
    "\"typical_size\":%.3f,\"sps30_online\":%s,\"firmware\":\"%u.%u\","
    "\"co2_ppm\":%u,"
    "\"temperature\":%.2f,\"humidity\":%.2f,"
    "\"sht_temperature\":%.2f,\"sht_humidity\":%.2f,"
    "\"sht45_online\":%s,\"sht45_has_reading\":%s,"
    "\"scd_temperature\":%.2f,\"scd_humidity\":%.2f,"
    "\"scd4x_online\":%s,\"scd4x_has_reading\":%s,"
    "\"voc_index\":%ld,\"nox_index\":%ld,"
    "\"sraw_voc\":%u,\"sraw_nox\":%u,"
    "\"sgp41_online\":%s,\"sgp41_has_reading\":%s,\"sgp41_conditioning_sec\":%u,"
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

    // Canonical room environment.
    shtTemperature,
    shtHumidity,
    shtTemperature,
    shtHumidity,
    sht45Online ? "true" : "false",
    sht45HasReading ? "true" : "false",

    // Retain SCD4X environmental values as diagnostics only.
    scdTemperature,
    scdHumidity,
    scd4xOnline ? "true" : "false",
    scd4xHasReading ? "true" : "false",

    static_cast<long>(vocIndex),
    static_cast<long>(noxIndex),
    srawVoc,
    srawNox,
    sgp41Online ? "true" : "false",
    sgp41HasReading ? "true" : "false",
    sgpConditioningSecondsRemaining,

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
// API GAS HISTORY
//
// Persistent 7-day VOC/NOx ring.
// Query:
//   /api/gas-history?hours=1
//   /api/gas-history?hours=24
//   /api/gas-history?hours=168
//
// Output samples:
//   [epoch, VOC Index, NOx Index, flags]
//
// flags:
//   bit 0 = first persisted sample after reboot
//   bit 1 = VOC learning
//   bit 2 = NOx learning
//
// The file retains all 30-second records. The HTTP response selects
// a stride targeting <= ~900 points so a 7-day request does not block
// the ESP32 with a huge JSON transfer.
// ============================================================

void handleApiGasHistory() {
  if (!gasStorageReady) {
    server.send(
      503,
      "application/json",
      "{\"error\":\"gas persistence unavailable\"}"
    );
    return;
  }

  uint32_t hours = 24;

  if (server.hasArg("hours")) {
    long requested =
      server.arg("hours").toInt();

    if (requested > 0) {
      hours =
        static_cast<uint32_t>(
          constrain(
            requested,
            1L,
            168L
          )
        );
    }
  }

  time_t now;
  time(&now);

  uint32_t cutoff = 0;

  if (now > 1577836800) {
    uint32_t seconds =
      hours * 60UL * 60UL;

    cutoff =
      static_cast<uint32_t>(now) >
      seconds
        ? static_cast<uint32_t>(now) -
          seconds
        : 0;
  }

  uint32_t relevantEstimate =
    min(
      gasPersistHeader.count,
      hours * 120UL
    );

  uint32_t stride =
    max(
      1UL,
      (relevantEstimate + 899UL) /
      900UL
    );

  File f =
    LittleFS.open(
      GAS_HISTORY_FILE,
      "r"
    );

  if (!f) {
    server.send(
      500,
      "application/json",
      "{\"error\":\"gas history open failed\"}"
    );
    return;
  }

  server.setContentLength(
    CONTENT_LENGTH_UNKNOWN
  );

  server.send(
    200,
    "application/json",
    ""
  );

  char chunk[512];
  size_t used = 0;

  auto flushChunk = [&]() {
    if (used > 0) {
      server.sendContent(
        chunk,
        used
      );
      used = 0;
    }
  };

  auto appendText = [&](const char* s) {
    size_t len =
      strlen(s);

    if (len > sizeof(chunk)) {
      flushChunk();
      server.sendContent(s, len);
      return;
    }

    if (
      used + len >
      sizeof(chunk)
    ) {
      flushChunk();
    }

    memcpy(
      chunk + used,
      s,
      len
    );

    used += len;
  };

  char item[128];

  snprintf(
    item,
    sizeof(item),
    "{\"hours\":%lu,\"stored\":%lu,\"capacity\":%lu,"
    "\"record_interval\":30,\"stride\":%lu,\"samples\":[",
    static_cast<unsigned long>(hours),
    static_cast<unsigned long>(
      gasPersistHeader.count
    ),
    static_cast<unsigned long>(
      gasPersistHeader.capacity
    ),
    static_cast<unsigned long>(stride)
  );

  appendText(item);

  uint32_t oldest =
    gasPersistHeader.count <
    gasPersistHeader.capacity
      ? 0
      : gasPersistHeader.writeIndex;

  bool first = true;
  uint32_t matched = 0;

  for (
    uint32_t i = 0;
    i < gasPersistHeader.count;
    i++
  ) {
    uint32_t index =
      (oldest + i) %
      gasPersistHeader.capacity;

    size_t offset =
      sizeof(GasPersistHeader) +
      static_cast<size_t>(index) *
      sizeof(GasPersistRecord);

    if (!f.seek(offset, SeekSet)) {
      continue;
    }

    GasPersistRecord r{};

    if (
      f.read(
        reinterpret_cast<uint8_t*>(&r),
        sizeof(r)
      ) != sizeof(r)
    ) {
      continue;
    }

    if (
      r.epoch == 0 ||
      (
        cutoff > 0 &&
        r.epoch < cutoff
      )
    ) {
      continue;
    }

    bool reboot =
      (r.flags & GAS_FLAG_REBOOT) != 0;

    // Always emit reboot markers. Otherwise stride the output.
    if (
      !reboot &&
      (matched++ % stride) != 0
    ) {
      continue;
    }

    snprintf(
      item,
      sizeof(item),
      "%s[%lu,%u,%u,%u]",
      first ? "" : ",",
      static_cast<unsigned long>(
        r.epoch
      ),
      r.voc,
      r.nox,
      r.flags
    );

    appendText(item);
    first = false;
  }

  f.close();

  appendText(
    "],\"storage\":\"LittleFS\",\"retention_days\":7}"
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
      "🏠 Air Monitor V1.7.2\n\n"
      "/status - current air summary\n"
      "/co2 - CO2 details\n"
      "/environment - SHT45 temperature / humidity\n"
      "/gases - SGP41 VOC / NOx\n"
      "/particles - particle counts\n"
      "/stats - 60-minute PM / CO2 statistics\n"
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
      String(shtTemperature, 1) +
      " C\n"
      "Humidity: " +
      String(shtHumidity, 1) +
      " %RH\n"
      "VOC Index: " +
      String(vocIndex) +
      "\n"
      "NOx Index: " +
      String(noxIndex) +
      "\n\n" +
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
      "🌿 CO2\n\n"
      "SCD4X: " +
      String(
        scd4xOnline
          ? "Online"
          : "Offline"
      ) +
      "\nCO2: " +
      String(co2ppm) +
      " ppm\n"
      "60m Min/Avg/Max: " +
      String(getCO2Min(), 0) +
      " / " +
      String(getCO2Average(), 0) +
      " / " +
      String(getCO2Max(), 0) +
      " ppm\n\n" +
      getCurrentTime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/environment") {
    String message =
      "🌡 SHT45 ENVIRONMENT\n\n"
      "Sensor: " +
      String(
        sht45Online
          ? "Online"
          : "Offline"
      ) +
      "\nTemperature: " +
      String(shtTemperature, 1) +
      " C\n"
      "Humidity: " +
      String(shtHumidity, 1) +
      " %RH\n"
      "SGP41 compensation: " +
      String(
        sht45HasReading
          ? "SHT45 live values"
          : "Default fallback"
      ) +
      "\n\n" +
      getCurrentTime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }

  if (command == "/gases") {
    String state;

    if (!sgp41Online) {
      state = "Offline";
    } else if (sgpConditioningSecondsRemaining > 0) {
      state =
        "Conditioning (" +
        String(sgpConditioningSecondsRemaining) +
        "s)";
    } else if (!sgp41HasReading) {
      state = "Warming up";
    } else {
      state = "Online";
    }

    String message =
      "🧪 SGP41 GAS QUALITY\n\n"
      "Sensor: " +
      state +
      "\nVOC: " +
      String(vocIndex) +
      " — " +
      getVOCStatus(vocIndex) +
      " / " +
      getTrendLabel(getVOCTrend()) +
      "\nVOC 5m peak: " +
      String(getVOC5MinutePeak()) +
      "\nNOx: " +
      String(noxIndex) +
      " — " +
      getNOxStatus(noxIndex) +
      " / " +
      getTrendLabel(getNOxTrend()) +
      "\nNOx 5m peak: " +
      String(getNOx5MinutePeak()) +
      "\nVOC learning: " +
      String(
        vocLearning()
          ? String(vocLearningMinutesRemaining()) + " min left"
          : "complete"
      ) +
      "\nNOx learning: " +
      String(
        noxLearning()
          ? String(noxLearningMinutesRemaining()) + " min left"
          : "complete"
      ) +
      "\nHint: " +
      getGasEventHint() +
      "\n7-day log: " +
      String(
        gasStorageReady
          ? "On"
          : "Off"
      ) +
      " (" +
      String(gasPersistHeader.count) +
      " records)\n\n" +
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
      "Monitor: V1.7.2\n"
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
      "\nSHT45: " +
      String(
        sht45Online
          ? "Online"
          : "Offline"
      ) +
      "\nSGP41: " +
      String(
        sgp41Online
          ? (
              sgp41HasReading
                ? "Online"
                : "Warming up"
            )
          : "Offline"
      ) +
      "\nCO2: " +
      String(co2ppm) +
      " ppm"
      "\nTemperature: " +
      String(shtTemperature, 1) +
      " C"
      "\nHumidity: " +
      String(shtHumidity, 1) +
      " %RH"
      "\nVOC/NOx: " +
      String(vocIndex) +
      " / " +
      String(noxIndex) +
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

  Serial.println("Monitor: V1.7.2");
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
      "CO2: %u ppm\n",
      co2ppm
    );
  }

  Serial.printf(
    "SHT45: %s\n",
    sht45Online
      ? "Online"
      : "Offline"
  );

  if (sht45HasReading) {
    Serial.printf(
      "Environment: %.2f C | %.2f %%RH\n",
      shtTemperature,
      shtHumidity
    );
  }

  Serial.printf(
    "SGP41: %s\n",
    sgp41Online
      ? (
          sgp41HasReading
            ? "Online"
            : "Warming up"
        )
      : "Offline"
  );

  if (sgp41Online) {
    Serial.printf(
      "Gas: VOC %ld (%s/%s) | NOx %ld (%s/%s)\n",
      static_cast<long>(vocIndex),
      getVOCStatus(vocIndex),
      getTrendLabel(getVOCTrend()),
      static_cast<long>(noxIndex),
      getNOxStatus(noxIndex),
      getTrendLabel(getNOxTrend())
    );
  }

  Serial.printf(
    "Gas persistence: %s | %lu/%lu stored | %u queued\n",
    gasStorageReady
      ? "Ready"
      : "Unavailable",
    static_cast<unsigned long>(
      gasPersistHeader.count
    ),
    static_cast<unsigned long>(
      gasPersistHeader.capacity
    ),
    gasPersistQueueCount
  );

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
