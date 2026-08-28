#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cSps30.h>

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
// Sensirion SPS30
// ============================================================
SensirionI2cSps30 sps30;

// ============================================================
// Setup
// ============================================================
void setup() {

  // ----------------------------------------------------------
  // Serial
  // ----------------------------------------------------------
  Serial.begin(115200);

  // Give ESP32-S3 native USB time to initialize
  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32-S3 Air Quality Monitor");
  Serial.println("================================");

  // ----------------------------------------------------------
  // Initialize I2C
  // ----------------------------------------------------------
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("I2C initialized.");

  // ----------------------------------------------------------
  // Initialize OLED
  // ----------------------------------------------------------
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {

    Serial.println("ERROR: SSD1306 initialization failed.");
    Serial.println("Check OLED wiring and I2C address.");

    while (1) {
      delay(100);
    }
  }

  Serial.println("OLED initialized.");

  // Display boot message
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 8);
  display.println("Air Quality Monitor");

  display.setCursor(0, 25);
  display.println("Initializing...");

  display.display();

  // ----------------------------------------------------------
  // Initialize SPS30
  // ----------------------------------------------------------
  sps30.begin(Wire, 0x69);

  Serial.println("Initializing SPS30...");

  // ----------------------------------------------------------
  // Probe SPS30 using firmware version
  // ----------------------------------------------------------
  uint8_t fwMajor = 0;
  uint8_t fwMinor = 0;

  int16_t error = sps30.readFirmwareVersion(
    fwMajor,
    fwMinor
  );

  if (error) {

    Serial.print("ERROR: SPS30 probe failed. Error code: ");
    Serial.println(error);

    Serial.println("Check:");
    Serial.println("  - SPS30 power");
    Serial.println("  - SDA/SCL wiring");
    Serial.println("  - SEL pin");
    Serial.println("  - I2C address");

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
  // Start continuous measurement
  //
  // IMPORTANT:
  // This is the enum name used by your installed
  // Sensirion_I2C_SPS30 library:
  //
  // SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT
  // ----------------------------------------------------------
  error = sps30.startMeasurement(
    SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT
  );

  if (error) {

    Serial.print("ERROR: Failed to start measurement. Error: ");
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

  // Give the sensor/fan some time to stabilize
  delay(1000);

  Serial.println("Starting measurements...");
  Serial.println();
}

// ============================================================
// Main Loop
// ============================================================
void loop() {

  uint16_t dataReady = 0;

  // ----------------------------------------------------------
  // Check whether new measurement data is available
  // ----------------------------------------------------------
  int16_t error = sps30.readDataReadyFlag(dataReady);

  if (error) {

    Serial.print("ERROR: Reading data-ready flag: ");
    Serial.println(error);

    delay(1000);
    return;
  }

  // ----------------------------------------------------------
  // New data available
  // ----------------------------------------------------------
  if (dataReady) {

    // --------------------------------------------------------
    // Particle Mass Concentration
    // --------------------------------------------------------
    float pm1p0 = 0.0;
    float pm2p5 = 0.0;
    float pm4p0 = 0.0;
    float pm10p0 = 0.0;

    // --------------------------------------------------------
    // Particle Number Concentration
    // --------------------------------------------------------
    float nc0p5 = 0.0;
    float nc1p0 = 0.0;
    float nc2p5 = 0.0;
    float nc4p0 = 0.0;
    float nc10p0 = 0.0;

    // --------------------------------------------------------
    // Typical particle size
    // --------------------------------------------------------
    float typicalSize = 0.0;

    // --------------------------------------------------------
    // Read measurement values
    //
    // This is the method provided by your installed library.
    // --------------------------------------------------------
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

      Serial.print("ERROR: Reading measurement: ");
      Serial.println(error);

    } else {

      // ======================================================
      // Serial Output
      // ======================================================

      Serial.println("--------------------------------");

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

      // ======================================================
      // OLED Display
      // ======================================================

      display.clearDisplay();

      // ------------------------------------------------------
      // Header
      // ------------------------------------------------------
      display.setTextSize(1);
      display.setCursor(0, 0);

      display.println("Air Quality (ug/m3)");

      display.drawLine(
        0,
        10,
        SCREEN_WIDTH - 1,
        10,
        SSD1306_WHITE
      );

      // ------------------------------------------------------
      // PM2.5 - Main Reading
      // ------------------------------------------------------
      display.setTextSize(2);
      display.setCursor(0, 17);

      display.print("PM2.5:");

      display.print(pm2p5, 1);

      // ------------------------------------------------------
      // PM1.0
      // ------------------------------------------------------
      display.setTextSize(1);
      display.setCursor(0, 40);

      display.print("PM1.0 : ");
      display.println(pm1p0, 1);

      // ------------------------------------------------------
      // PM10
      // ------------------------------------------------------
      display.setCursor(0, 52);

      display.print("PM10  : ");
      display.println(pm10p0, 1);

      // Send everything to OLED
      display.display();
    }
  }

  // ==========================================================
  // Poll interval
  // ==========================================================
  delay(2000);
}