#include <Wire.h>
#include <WiFi.h>
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

// India Standard Time (UTC +05:30)
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = 5 * 60 * 60;
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================
// ESP32-S3 I2C Pins
// ============================================================

#define I2C_SDA 8
#define I2C_SCL 9

// ============================================================
// OLED Configuration
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
// Function declarations
// ============================================================

void connectWiFi();
void syncTime();

void updateSPS30();

void updateDisplay();
void drawAirQualityScreen();
void drawClockScreen();
void drawParticleScreen();

void printTimeSerial();

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
  Serial.println("                 V1.1");
  Serial.println("========================================");
  Serial.println();

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("I2C initialized.");

  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {

    Serial.println("ERROR: SSD1306 initialization failed.");

    while (1) {
      delay(100);
    }
  }

  Serial.println("OLED initialized.");

  // Boot screen

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 8);
  display.println("AIR QUALITY MONITOR");

  display.setCursor(0, 25);
  display.println("V1.1");

  display.setCursor(0, 40);
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

  sps30.begin(Wire, 0x69);

  // ----------------------------------------------------------
  // Probe sensor
  // ----------------------------------------------------------

  uint8_t fwMajor = 0;
  uint8_t fwMinor = 0;

  int16_t error = sps30.readFirmwareVersion(
    fwMajor,
    fwMinor
  );

  if (error) {

    Serial.print("ERROR: SPS30 probe failed: ");
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

    Serial.print("ERROR: SPS30 start failed: ");
    Serial.println(error);

    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(0, 10);
    display.println("SPS30 START ERROR!");

    display.display();

    while (1) {
      delay(100);
    }
  }

  Serial.println("SPS30 measurement started.");

  // Allow fan/sensor to stabilize
  delay(1000);

  // ----------------------------------------------------------
  // Initial screen
  // ----------------------------------------------------------

  currentScreen = 0;

  lastScreenChange = millis();

  updateDisplay();

  Serial.println();
  Serial.println("========================================");
  Serial.println("Monitor started.");
  Serial.println("========================================");
  Serial.println();
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // Check SPS30
  // ----------------------------------------------------------

  updateSPS30();

  // ----------------------------------------------------------
  // Rotate display
  // ----------------------------------------------------------

  if (millis() - lastScreenChange >= SCREEN_ROTATION_INTERVAL) {

    currentScreen++;

    if (currentScreen > 2) {
      currentScreen = 0;
    }

    lastScreenChange = millis();

    updateDisplay();
  }

  // Poll frequently, but don't hammer the CPU
  delay(200);
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

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("Wi-Fi connected.");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

  } else {

    Serial.println("WARNING: Wi-Fi connection failed.");
  }
}

// ============================================================
// NTP
// ============================================================

void syncTime() {

  Serial.println("Synchronizing time...");

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

    Serial.println("Time synchronized.");

    printTimeSerial();

  } else {

    Serial.println("WARNING: NTP synchronization failed.");
  }
}

// ============================================================
// SPS30
// ============================================================

void updateSPS30() {

  uint16_t dataReady = 0;

  int16_t error = sps30.readDataReadyFlag(
    dataReady
  );

  if (error) {

    Serial.print("ERROR: Data-ready flag: ");
    Serial.println(error);

    return;
  }

  if (!dataReady) {
    return;
  }

  // ----------------------------------------------------------
  // Read measurements
  // ----------------------------------------------------------

  error = sps30.readMeasurementValuesFloat(
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

    Serial.print("ERROR: SPS30 measurement: ");
    Serial.println(error);

    return;
  }

  // ----------------------------------------------------------
  // Serial output
  // ----------------------------------------------------------

  Serial.println("----------------------------------------");

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

  // ----------------------------------------------------------
  // Refresh current OLED screen
  // ----------------------------------------------------------

  updateDisplay();
}

// ============================================================
// DISPLAY CONTROLLER
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
// SCREEN 1
//
// Main Air Quality
// ============================================================

void drawAirQualityScreen() {

  display.clearDisplay();

  // Header

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

  // PM2.5

  display.setTextSize(2);
  display.setCursor(0, 16);

  display.print("PM2.5 ");

  display.print(pm2p5, 1);

  // PM1.0

  display.setTextSize(1);
  display.setCursor(0, 40);

  display.print("PM1.0 : ");
  display.print(pm1p0, 1);
  display.println(" ug/m3");

  // PM10

  display.setCursor(0, 52);

  display.print("PM10  : ");
  display.print(pm10p0, 1);
  display.println(" ug/m3");
}

// ============================================================
// SCREEN 2
//
// Clock + PM
// ============================================================

void drawClockScreen() {

  display.clearDisplay();

  struct tm timeinfo;

  // ----------------------------------------------------------
  // Date
  // ----------------------------------------------------------

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

    display.print(dateBuffer);

    // --------------------------------------------------------
    // Clock
    // --------------------------------------------------------

    display.setTextSize(2);
    display.setCursor(68, 0);

    char timeBuffer[10];

    strftime(
      timeBuffer,
      sizeof(timeBuffer),
      "%H:%M",
      &timeinfo
    );

    display.print(timeBuffer);

  } else {

    display.setTextSize(1);
    display.setCursor(0, 4);

    display.print("TIME NOT SYNCED");
  }

  // ----------------------------------------------------------
  // Separator
  // ----------------------------------------------------------

  display.drawLine(
    0,
    17,
    SCREEN_WIDTH - 1,
    17,
    SSD1306_WHITE
  );

  // ----------------------------------------------------------
  // PM2.5
  // ----------------------------------------------------------

  display.setTextSize(1);
  display.setCursor(0, 21);

  display.print("PM2.5 ");
  display.print(pm2p5, 1);
  display.print(" ug/m3");

  // ----------------------------------------------------------
  // PM1.0
  // ----------------------------------------------------------

  display.setCursor(0, 32);

  display.print("PM1.0 ");
  display.print(pm1p0, 1);
  display.print(" ug/m3");

  // ----------------------------------------------------------
  // PM10
  // ----------------------------------------------------------

  display.setCursor(0, 43);

  display.print("PM10  ");
  display.print(pm10p0, 1);
  display.print(" ug/m3");

  // ----------------------------------------------------------
  // Particle size
  // ----------------------------------------------------------

  display.setCursor(0, 54);

  display.print("Size  ");
  display.print(typicalSize, 2);
  display.print(" um");
}

// ============================================================
// SCREEN 3
//
// Particle Number Concentration
// ============================================================

void drawParticleScreen() {

  display.clearDisplay();

  // Header

  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("PARTICLE COUNTS");

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH - 1,
    10,
    SSD1306_WHITE
  );

  // ----------------------------------------------------------
  // NC0.5
  // ----------------------------------------------------------

  display.setCursor(0, 16);

  display.print("NC0.5 : ");
  display.println(nc0p5, 1);

  // ----------------------------------------------------------
  // NC1.0
  // ----------------------------------------------------------

  display.setCursor(0, 28);

  display.print("NC1.0 : ");
  display.println(nc1p0, 1);

  // ----------------------------------------------------------
  // NC2.5
  // ----------------------------------------------------------

  display.setCursor(0, 40);

  display.print("NC2.5 : ");
  display.println(nc2p5, 1);

  // ----------------------------------------------------------
  // NC10
  // ----------------------------------------------------------

  display.setCursor(0, 52);

  display.print("NC10  : ");
  display.println(nc10p0, 1);
}

// ============================================================
// Serial clock output
// ============================================================

void printTimeSerial() {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {

    Serial.println("Unable to get local time.");

    return;
  }

  char buffer[64];

  strftime(
    buffer,
    sizeof(buffer),
    "%A, %d %B %Y %H:%M:%S",
    &timeinfo
  );

  Serial.print("Current time: ");
  Serial.println(buffer);
}