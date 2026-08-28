#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cSps30.h>

// ============================================================
// V1.4
// ESP32-S3 AIR QUALITY MONITOR
//
// V1.3 OLED screens preserved exactly:
//
//   Screen 0 = Air Quality
//   Screen 1 = Clock
//   Screen 2 = Particle Counts
//
// V1.4 adds:
//
//   Screen 3 = System
//   Wi-Fi
//   NTP clock
//   airmonitor.local
//   Web dashboard
//   Dark mode
//   60-minute PM history
//   Statistics
//   JSON API
//   Telegram bot
//   Telegram commands
//   PM2.5 / PM10 alerts
//
// ============================================================


// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID     = "Pirates_IoT";
const char* WIFI_PASSWORD = "<password>";


// ============================================================
// MDNS
// ============================================================

const char* MDNS_HOSTNAME = "airmonitor";


// ============================================================
// NTP
// ============================================================

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const long GMT_OFFSET_SEC = (5 * 60 * 60) + (30 * 60);  // IST UTC+05:30
const int DAYLIGHT_OFFSET_SEC = 0;


// ============================================================
// TELEGRAM
//
// Replace these with your bot credentials.
//
// BOT_TOKEN:
// Create bot using @BotFather.
//
// CHAT_ID:
// Send /id to your bot or obtain your Telegram chat ID.
//
// ============================================================

const char* TELEGRAM_BOT_TOKEN = "<token>>";
const char* TELEGRAM_CHAT_ID    = "<id>";

WiFiClientSecure telegramClient;

UniversalTelegramBot bot(
  TELEGRAM_BOT_TOKEN,
  telegramClient
);


// ============================================================
// TELEGRAM TIMING
// ============================================================

unsigned long lastTelegramPoll = 0;

const unsigned long TELEGRAM_POLL_INTERVAL = 1500UL;


// ============================================================
// AIR QUALITY ALERTS
// ============================================================

bool alertsEnabled = true;

float pm25AlertThreshold = 35.0;
float pm10AlertThreshold = 50.0;

bool pm25AlertActive = false;
bool pm10AlertActive = false;

unsigned long lastAlertCheck = 0;

#define ALERT_CHECK_INTERVAL 10000UL

unsigned long lastPM25AlertSent = 0;
unsigned long lastPM10AlertSent = 0;

const unsigned long ALERT_COOLDOWN = 30UL * 60UL * 1000UL;


// ============================================================
// I2C
// ============================================================

#define I2C_SDA 8
#define I2C_SCL 9


// ============================================================
// OLED
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_RESET   -1
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

uint8_t fwMajor = 0;
uint8_t fwMinor = 0;

bool sps30Online = false;


// ============================================================
// WEB SERVER
// ============================================================

WebServer server(80);


// ============================================================
// CURRENT SPS30 VALUES
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
// TIMING
// ============================================================

unsigned long lastMeasurementMillis = 0;
unsigned long lastHistorySample = 0;
unsigned long lastScreenChange = 0;

#define HISTORY_INTERVAL 30000UL
#define SCREEN_ROTATION_INTERVAL 5000UL


// ============================================================
// OLED SCREENS
//
// 0 = V1.3 Air Quality
// 1 = V1.3 Clock
// 2 = V1.3 Particle Counts
// 3 = NEW System
// ============================================================

int currentScreen = 0;


// ============================================================
// HISTORY
//
// 30 second samples
// 120 samples = 60 minutes
// ============================================================

#define HISTORY_SIZE 120

float pm25History[HISTORY_SIZE];
float pm10History[HISTORY_SIZE];

unsigned long historyTime[HISTORY_SIZE];

int historyCount = 0;
int historyWriteIndex = 0;


// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

void connectWiFi();
void syncTime();

void updateSPS30();
void addHistorySample();

void updateDisplay();

void drawAirQualityScreen();
void drawClockScreen();
void drawParticleScreen();
void drawSystemScreen();

void startWebServer();

void handleRoot();
void handleApiStatus();
void handleApiHistory();
void handleNotFound();

void handleTelegram();
void checkAirQualityAlert();

void sendTelegramMessage(String message);
void handleTelegramCommand(String chatId, String command);

String getCurrentTime();
String getUptime();

void printSystemStatus();

float getPM25Min();
float getPM25Max();
float getPM25Average();

float getPM10Min();
float getPM10Max();
float getPM10Average();


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("      ESP32-S3 AIR QUALITY MONITOR");
  Serial.println("                 V1.4");
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


  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 5);
  display.println("AIR QUALITY MONITOR");

  display.setCursor(0, 20);
  display.println("V1.4");

  display.setCursor(0, 35);
  display.println("Initializing...");

  display.display();


  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  connectWiFi();


  // ----------------------------------------------------------
  // mDNS
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {

    if (MDNS.begin(MDNS_HOSTNAME)) {

      Serial.println();
      Serial.println("mDNS started.");

      Serial.print("Hostname: http://");
      Serial.print(MDNS_HOSTNAME);
      Serial.println(".local");

      MDNS.addService(
        "http",
        "tcp",
        80
      );

    } else {

      Serial.println(
        "WARNING: mDNS failed."
      );
    }
  }


  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
  }


  // ----------------------------------------------------------
  // Telegram TLS
  // ----------------------------------------------------------

  telegramClient.setInsecure();

  Serial.println(
    "Telegram TLS initialized."
  );


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
  // SPS30 PROBE
  // ----------------------------------------------------------

  int16_t error =
    sps30.readFirmwareVersion(
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

  Serial.println(
    "SPS30 detected!"
  );

  Serial.print("Firmware: ");
  Serial.print(fwMajor);
  Serial.print(".");
  Serial.println(fwMinor);


  // ----------------------------------------------------------
  // START MEASUREMENT
  // ----------------------------------------------------------

  error =
    sps30.startMeasurement(
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

  delay(1000);


  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  startWebServer();


  // ----------------------------------------------------------
  // INITIAL DISPLAY
  // ----------------------------------------------------------

  currentScreen = 0;

  lastScreenChange = millis();

  updateDisplay();


  // ----------------------------------------------------------
  // STATUS
  // ----------------------------------------------------------

  printSystemStatus();


  Serial.println();
  Serial.println("========================================");
  Serial.println("V1.4 MONITOR STARTED");
  Serial.println("========================================");

  Serial.print("Dashboard: http://");
  Serial.print(MDNS_HOSTNAME);
  Serial.println(".local");

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
  // WEB SERVER
  // ----------------------------------------------------------

  server.handleClient();


  // ----------------------------------------------------------
  // TELEGRAM
  // ----------------------------------------------------------

  if (
    WiFi.status() == WL_CONNECTED &&
    millis() - lastTelegramPoll >=
    TELEGRAM_POLL_INTERVAL
  ) {

    lastTelegramPoll = millis();

    handleTelegram();
  }


  // ----------------------------------------------------------
  // AIR QUALITY ALERTS
  // ----------------------------------------------------------

  if (
    millis() - lastAlertCheck >=
    ALERT_CHECK_INTERVAL
  ) {

    lastAlertCheck = millis();

    checkAirQualityAlert();
  }


  // ----------------------------------------------------------
  // OLED SCREEN ROTATION
  // ----------------------------------------------------------

  if (
    millis() - lastScreenChange >=
    SCREEN_ROTATION_INTERVAL
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

  Serial.println();

  Serial.print(
    "Connecting to Wi-Fi: "
  );

  Serial.println(
    WIFI_SSID
  );

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


  // ----------------------------------------------------------
  // HISTORY
  // ----------------------------------------------------------

  if (
    millis() - lastHistorySample >=
    HISTORY_INTERVAL
  ) {

    addHistorySample();

    lastHistorySample = millis();
  }


  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  updateDisplay();
}


// ============================================================
// HISTORY
// ============================================================

void addHistorySample() {

  pm25History[historyWriteIndex] = pm2p5;

  pm10History[historyWriteIndex] = pm10p0;

  historyTime[historyWriteIndex] = millis();


  historyWriteIndex++;

  if (
    historyWriteIndex >=
    HISTORY_SIZE
  ) {

    historyWriteIndex = 0;
  }


  if (
    historyCount <
    HISTORY_SIZE
  ) {

    historyCount++;
  }


  Serial.print(
    "History sample: "
  );

  Serial.print(
    historyCount
  );

  Serial.print(
    "/"
  );

  Serial.print(
    HISTORY_SIZE
  );

  Serial.print(
    " PM2.5="
  );

  Serial.print(
    pm2p5,
    1
  );

  Serial.print(
    " PM10="
  );

  Serial.println(
    pm10p0,
    1
  );
}


// ============================================================
// HISTORY STATISTICS
// ============================================================

float getPM25Min() {

  if (historyCount == 0) {
    return pm2p5;
  }

  float minimum = 999999.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    if (
      pm25History[i] <
      minimum
    ) {

      minimum =
        pm25History[i];
    }
  }

  return minimum;
}


float getPM25Max() {

  if (historyCount == 0) {
    return pm2p5;
  }

  float maximum = -999999.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    if (
      pm25History[i] >
      maximum
    ) {

      maximum =
        pm25History[i];
    }
  }

  return maximum;
}


float getPM25Average() {

  if (historyCount == 0) {
    return pm2p5;
  }

  float total = 0.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    total +=
      pm25History[i];
  }

  return total /
         historyCount;
}


float getPM10Min() {

  if (historyCount == 0) {
    return pm10p0;
  }

  float minimum = 999999.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    if (
      pm10History[i] <
      minimum
    ) {

      minimum =
        pm10History[i];
    }
  }

  return minimum;
}


float getPM10Max() {

  if (historyCount == 0) {
    return pm10p0;
  }

  float maximum = -999999.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    if (
      pm10History[i] >
      maximum
    ) {

      maximum =
        pm10History[i];
    }
  }

  return maximum;
}


float getPM10Average() {

  if (historyCount == 0) {
    return pm10p0;
  }

  float total = 0.0;

  for (
    int i = 0;
    i < historyCount;
    i++
  ) {

    total +=
      pm10History[i];
  }

  return total /
         historyCount;
}


// ============================================================
// TELEGRAM MESSAGE
// ============================================================

void sendTelegramMessage(String message) {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return;
  }

  if (
    String(TELEGRAM_BOT_TOKEN) ==
    "YOUR_BOT_TOKEN"
  ) {

    return;
  }

  bot.sendMessage(
    TELEGRAM_CHAT_ID,
    message,
    ""
  );
}


// ============================================================
// TELEGRAM POLLING
// ============================================================

void handleTelegram() {

  if (
    String(TELEGRAM_BOT_TOKEN) ==
    "YOUR_BOT_TOKEN"
  ) {

    return;
  }


  int messageCount =
    bot.getUpdates(
      bot.last_message_received + 1
    );


  while (
    messageCount
  ) {

    for (
      int i = 0;
      i < messageCount;
      i++
    ) {

      String chatId =
        bot.messages[i].chat_id;

      String text =
        bot.messages[i].text;


      Serial.print(
        "Telegram command: "
      );

      Serial.println(
        text
      );


      // ------------------------------------------------------
      // Security
      //
      // Only configured chat ID is allowed.
      // ------------------------------------------------------

      if (
        String(TELEGRAM_CHAT_ID) !=
        chatId
      ) {

        bot.sendMessage(
          chatId,
          "Unauthorized chat.",
          ""
        );

        continue;
      }


      handleTelegramCommand(
        chatId,
        text
      );
    }


    messageCount =
      bot.getUpdates(
        bot.last_message_received + 1
      );
  }
}


// ============================================================
// TELEGRAM COMMANDS
// ============================================================

void handleTelegramCommand(
  String chatId,
  String command
) {

  command.trim();

  command.toLowerCase();


  // ----------------------------------------------------------
  // /start
  // ----------------------------------------------------------

  if (
    command == "/start"
  ) {

    String message =
      "🏠 Air Quality Monitor V1.4\n\n"
      "/status - Current readings\n"
      "/pm - PM readings\n"
      "/particles - Particle counts\n"
      "/stats - 60-minute statistics\n"
      "/system - Device status\n"
      "/history - History information\n"
      "/alerts - Alert configuration\n"
      "/alertson - Enable alerts\n"
      "/alertsoff - Disable alerts\n"
      "/help - Commands";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /help
  // ----------------------------------------------------------

  if (
    command == "/help"
  ) {

    String message =
      "📋 Commands\n\n"
      "/status\n"
      "/pm\n"
      "/particles\n"
      "/stats\n"
      "/system\n"
      "/history\n"
      "/alerts\n"
      "/alertson\n"
      "/alertsoff";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /status
  // ----------------------------------------------------------

  if (
    command == "/status"
  ) {

    String message =
      "🏠 AIR QUALITY\n\n";

    message +=
      "PM1.0: " +
      String(pm1p0, 1) +
      " µg/m³\n";

    message +=
      "PM2.5: " +
      String(pm2p5, 1) +
      " µg/m³\n";

    message +=
      "PM4.0: " +
      String(pm4p0, 1) +
      " µg/m³\n";

    message +=
      "PM10: " +
      String(pm10p0, 1) +
      " µg/m³\n\n";

    message +=
      "Time: " +
      getCurrentTime();

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /pm
  // ----------------------------------------------------------

  if (
    command == "/pm"
  ) {

    String message =
      "🌫 PARTICULATE MATTER\n\n";

    message +=
      "PM1.0  " +
      String(pm1p0, 1) +
      " µg/m³\n";

    message +=
      "PM2.5  " +
      String(pm2p5, 1) +
      " µg/m³\n";

    message +=
      "PM4.0  " +
      String(pm4p0, 1) +
      " µg/m³\n";

    message +=
      "PM10   " +
      String(pm10p0, 1) +
      " µg/m³";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /particles
  // ----------------------------------------------------------

  if (
    command == "/particles"
  ) {

    String message =
      "🔬 PARTICLE COUNTS\n\n";

    message +=
      "NC0.5: " +
      String(nc0p5, 1) +
      "\n";

    message +=
      "NC1.0: " +
      String(nc1p0, 1) +
      "\n";

    message +=
      "NC2.5: " +
      String(nc2p5, 1) +
      "\n";

    message +=
      "NC4.0: " +
      String(nc4p0, 1) +
      "\n";

    message +=
      "NC10: " +
      String(nc10p0, 1) +
      "\n\n";

    message +=
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


  // ----------------------------------------------------------
  // /stats
  // ----------------------------------------------------------

  if (
    command == "/stats"
  ) {

    String message =
      "📊 LAST 60 MINUTES\n\n";

    message +=
      "PM2.5\n";

    message +=
      "Min: " +
      String(getPM25Min(), 1) +
      "\n";

    message +=
      "Avg: " +
      String(getPM25Average(), 1) +
      "\n";

    message +=
      "Max: " +
      String(getPM25Max(), 1) +
      "\n\n";

    message +=
      "PM10\n";

    message +=
      "Min: " +
      String(getPM10Min(), 1) +
      "\n";

    message +=
      "Avg: " +
      String(getPM10Average(), 1) +
      "\n";

    message +=
      "Max: " +
      String(getPM10Max(), 1);

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /system
  // ----------------------------------------------------------

  if (
    command == "/system"
  ) {

    String message =
      "⚙️ SYSTEM\n\n";

    message +=
      "Firmware: " +
      String(fwMajor) +
      "." +
      String(fwMinor) +
      "\n";

    message +=
      "Wi-Fi: " +
      String(
        WiFi.status() == WL_CONNECTED
          ? "Connected"
          : "Disconnected"
      ) +
      "\n";

    if (
      WiFi.status() ==
      WL_CONNECTED
    ) {

      message +=
        "IP: " +
        WiFi.localIP().toString() +
        "\n";

      message +=
        "RSSI: " +
        String(WiFi.RSSI()) +
        " dBm\n";
    }

    message +=
      "Uptime: " +
      getUptime() +
      "\n";

    message +=
      "Sensor: " +
      String(
        sps30Online
          ? "Online"
          : "Offline"
      ) +
      "\n";

    message +=
      "History: " +
      String(historyCount) +
      " / " +
      String(HISTORY_SIZE);

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /history
  // ----------------------------------------------------------

  if (
    command == "/history"
  ) {

    String message =
      "📈 HISTORY\n\n";

    message +=
      "Window: 60 minutes\n";

    message +=
      "Interval: 30 seconds\n";

    message +=
      "Samples: " +
      String(historyCount) +
      " / " +
      String(HISTORY_SIZE) +
      "\n\n";

    message +=
      "PM2.5 Avg: " +
      String(getPM25Average(), 1) +
      " µg/m³\n";

    message +=
      "PM10 Avg: " +
      String(getPM10Average(), 1) +
      " µg/m³";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /alerts
  // ----------------------------------------------------------

  if (
    command == "/alerts"
  ) {

    String message =
      "🚨 ALERTS\n\n";

    message +=
      "Status: ";

    message +=
      alertsEnabled
        ? "Enabled\n"
        : "Disabled\n";

    message +=
      "PM2.5 threshold: " +
      String(pm25AlertThreshold, 1) +
      " µg/m³\n";

    message +=
      "PM10 threshold: " +
      String(pm10AlertThreshold, 1) +
      " µg/m³\n";

    message +=
      "Cooldown: 30 minutes";

    bot.sendMessage(
      chatId,
      message,
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /alertson
  // ----------------------------------------------------------

  if (
    command == "/alertson"
  ) {

    alertsEnabled = true;

    bot.sendMessage(
      chatId,
      "🚨 Air-quality alerts enabled.",
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // /alertsoff
  // ----------------------------------------------------------

  if (
    command == "/alertsoff"
  ) {

    alertsEnabled = false;

    bot.sendMessage(
      chatId,
      "🔕 Air-quality alerts disabled.",
      ""
    );

    return;
  }


  // ----------------------------------------------------------
  // UNKNOWN COMMAND
  // ----------------------------------------------------------

  bot.sendMessage(
    chatId,
    "Unknown command.\nUse /help",
    ""
  );
}


// ============================================================
// AIR QUALITY ALERT CHECK
// ============================================================

void checkAirQualityAlert() {

  if (!alertsEnabled) {
    return;
  }

  if (!sps30Online) {
    return;
  }


  // ----------------------------------------------------------
  // PM2.5
  // ----------------------------------------------------------

  if (
    pm2p5 >= pm25AlertThreshold
  ) {

    if (!pm25AlertActive) {

      pm25AlertActive = true;

      if (
        lastPM25AlertSent == 0 ||
        millis() - lastPM25AlertSent >=
        ALERT_COOLDOWN
      ) {

        String message =
          "🚨 PM2.5 ALERT\n\n";

        message +=
          "Current: " +
          String(pm2p5, 1) +
          " µg/m³\n";

        message +=
          "Threshold: " +
          String(pm25AlertThreshold, 1) +
          " µg/m³\n\n";

        message +=
          "Time: " +
          getCurrentTime();

        sendTelegramMessage(
          message
        );

        lastPM25AlertSent =
          millis();
      }
    }

  } else {

    if (pm25AlertActive) {

      pm25AlertActive = false;

      sendTelegramMessage(
        "✅ PM2.5 returned below "
        "the configured threshold.\n\n"
        "Current: " +
        String(pm2p5, 1) +
        " µg/m³"
      );
    }
  }


  // ----------------------------------------------------------
  // PM10
  // ----------------------------------------------------------

  if (
    pm10p0 >= pm10AlertThreshold
  ) {

    if (!pm10AlertActive) {

      pm10AlertActive = true;

      if (
        lastPM10AlertSent == 0 ||
        millis() - lastPM10AlertSent >=
        ALERT_COOLDOWN
      ) {

        String message =
          "🚨 PM10 ALERT\n\n";

        message +=
          "Current: " +
          String(pm10p0, 1) +
          " µg/m³\n";

        message +=
          "Threshold: " +
          String(pm10AlertThreshold, 1) +
          " µg/m³\n\n";

        message +=
          "Time: " +
          getCurrentTime();

        sendTelegramMessage(
          message
        );

        lastPM10AlertSent =
          millis();
      }
    }

  } else {

    if (pm10AlertActive) {

      pm10AlertActive = false;

      sendTelegramMessage(
        "✅ PM10 returned below "
        "the configured threshold.\n\n"
        "Current: " +
        String(pm10p0, 1) +
        " µg/m³"
      );
    }
  }
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

  Serial.println(
    "Web server started."
  );
}


// ============================================================
// WEB DASHBOARD
//
// This preserves the V1.3 dashboard.
//
// ============================================================

void handleRoot() {

  String html = R"rawliteral(

<!DOCTYPE html>
<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width, initial-scale=1.0">

<title>Air Quality Monitor</title>

<style>

* {
  box-sizing: border-box;
}

:root {
  --bg:#f2f4f7;
  --card:#ffffff;
  --text:#202124;
  --secondary:#666666;
  --border:#eeeeee;
  --accent:#137333;
  --status-bg:#e6f4ea;
}

[data-theme="dark"] {
  --bg:#101214;
  --card:#1b1e22;
  --text:#f1f3f4;
  --secondary:#aeb4bb;
  --border:#30343a;
  --accent:#81c995;
  --status-bg:#183522;
}

body {
  margin:0;
  font-family:system-ui,-apple-system,BlinkMacSystemFont,
               "Segoe UI",sans-serif;
  background:var(--bg);
  color:var(--text);
}

.container {
  max-width:1000px;
  margin:auto;
  padding:16px;
}

.header {
  background:var(--card);
  border-radius:16px;
  padding:20px;
  margin-bottom:16px;
  box-shadow:0 2px 8px rgba(0,0,0,.12);
  position:relative;
}

.header h1 {
  margin:0;
  font-size:24px;
}

.header p {
  margin:6px 0 0;
  color:var(--secondary);
}

.theme-button {
  position:absolute;
  top:16px;
  right:16px;
  border:none;
  border-radius:50%;
  width:40px;
  height:40px;
  cursor:pointer;
  background:var(--bg);
  color:var(--text);
  font-size:20px;
}

.main-card {
  background:var(--card);
  border-radius:16px;
  padding:25px;
  text-align:center;
  margin-bottom:16px;
  box-shadow:0 2px 8px rgba(0,0,0,.12);
}

.main-label {
  font-size:16px;
  color:var(--secondary);
}

.pm25 {
  font-size:64px;
  font-weight:700;
  margin:5px 0;
}

.unit {
  color:var(--secondary);
}

.grid {
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:12px;
  margin-bottom:16px;
}

.card {
  background:var(--card);
  border-radius:14px;
  padding:18px;
  box-shadow:0 2px 8px rgba(0,0,0,.12);
}

.card-title {
  font-size:14px;
  color:var(--secondary);
  margin-bottom:5px;
}

.value {
  font-size:26px;
  font-weight:600;
}

.section {
  background:var(--card);
  border-radius:16px;
  padding:20px;
  margin-bottom:16px;
  box-shadow:0 2px 8px rgba(0,0,0,.12);
}

.section h2 {
  margin-top:0;
  font-size:18px;
}

.stats-grid {
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:12px;
}

.stat {
  text-align:center;
  padding:12px;
  border-radius:10px;
  background:var(--bg);
}

.stat-label {
  font-size:12px;
  color:var(--secondary);
}

.stat-value {
  font-size:22px;
  font-weight:600;
  margin-top:4px;
}

.chart-container {
  position:relative;
  width:100%;
  height:320px;
}

canvas {
  width:100%!important;
  height:100%!important;
}

.row {
  display:flex;
  justify-content:space-between;
  padding:8px 0;
  border-bottom:1px solid var(--border);
}

.row:last-child {
  border-bottom:none;
}

.label {
  color:var(--secondary);
}

.status {
  display:inline-block;
  padding:4px 9px;
  border-radius:20px;
  background:var(--status-bg);
  color:var(--accent);
  font-size:13px;
}

@media(max-width:600px) {

  .grid {
    grid-template-columns:1fr;
  }

  .stats-grid {
    grid-template-columns:1fr;
  }

  .pm25 {
    font-size:52px;
  }

  .chart-container {
    height:240px;
  }
}

</style>

</head>

<body>

<div class="container">

<div class="header">

<button
class="theme-button"
onclick="toggleTheme()"
id="themeButton">🌙</button>

<h1>🏠 Air Quality Monitor</h1>

<p id="datetime">Loading time...</p>

</div>


<div class="main-card">

<div class="main-label">PM2.5</div>

<div class="pm25" id="pm25">--</div>

<div class="unit">µg/m³</div>

</div>


<div class="grid">

<div class="card">
<div class="card-title">PM1.0</div>
<div class="value" id="pm10">--</div>
<div class="unit">µg/m³</div>
</div>

<div class="card">
<div class="card-title">PM4.0</div>
<div class="value" id="pm40">--</div>
<div class="unit">µg/m³</div>
</div>

<div class="card">
<div class="card-title">PM10</div>
<div class="value" id="pm100">--</div>
<div class="unit">µg/m³</div>
</div>

</div>


<div class="section">

<h2>PM History — Last 60 Minutes</h2>

<div class="chart-container">
<canvas id="pmChart"></canvas>
</div>

</div>


<div class="section">

<h2>PM2.5 — 60 Minute Statistics</h2>

<div class="stats-grid">

<div class="stat">
<div class="stat-label">Minimum</div>
<div class="stat-value" id="pm25Min">--</div>
</div>

<div class="stat">
<div class="stat-label">Average</div>
<div class="stat-value" id="pm25Avg">--</div>
</div>

<div class="stat">
<div class="stat-label">Maximum</div>
<div class="stat-value" id="pm25Max">--</div>
</div>

</div>

</div>


<div class="section">

<h2>PM10 — 60 Minute Statistics</h2>

<div class="stats-grid">

<div class="stat">
<div class="stat-label">Minimum</div>
<div class="stat-value" id="pm10Min">--</div>
</div>

<div class="stat">
<div class="stat-label">Average</div>
<div class="stat-value" id="pm10Avg">--</div>
</div>

<div class="stat">
<div class="stat-label">Maximum</div>
<div class="stat-value" id="pm10Max">--</div>
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
<span class="label">Typical particle size</span>
<span id="size">--</span>
</div>

<div class="row">
<span class="label">Firmware</span>
<span id="firmware">--</span>
</div>

<div class="row">
<span class="label">History samples</span>
<span id="historySamples">--</span>
</div>

<div class="row">
<span class="label">Status</span>
<span id="sensorStatus" class="status">--</span>
</div>

</div>


<div class="section">

<h2>Device</h2>

<div class="row">
<span class="label">Wi-Fi</span>
<span id="wifi">--</span>
</div>

<div class="row">
<span class="label">Signal</span>
<span id="rssi">--</span>
</div>

<div class="row">
<span class="label">Uptime</span>
<span id="uptime">--</span>
</div>

</div>


<div class="section">

<h2>Telegram Alerts</h2>

<div class="row">
<span class="label">Alerts</span>
<span id="alerts">--</span>
</div>

<div class="row">
<span class="label">PM2.5 threshold</span>
<span>35 µg/m³</span>
</div>

<div class="row">
<span class="label">PM10 threshold</span>
<span>50 µg/m³</span>
</div>

</div>

</div>


<script>

function applyTheme(theme) {

document.documentElement.setAttribute(
'data-theme',
theme
);

const button =
document.getElementById('themeButton');

button.textContent =
theme === 'dark' ? '☀️' : '🌙';

localStorage.setItem(
'theme',
theme
);

}

function toggleTheme() {

const current =
document.documentElement
.getAttribute('data-theme');

applyTheme(
current === 'dark'
? 'light'
: 'dark'
);

drawChart();

}


const savedTheme =
localStorage.getItem('theme');

if(savedTheme) {

applyTheme(savedTheme);

} else {

applyTheme(
window.matchMedia(
'(prefers-color-scheme: dark)'
).matches ? 'dark' : 'light'
);

}


let chartData = {
pm25:[],
pm10:[],
time:[]
};


function drawChart() {

const canvas =
document.getElementById('pmChart');

const ctx =
canvas.getContext('2d');

const width =
canvas.clientWidth;

const height =
canvas.clientHeight;

if(width <= 0 || height <= 0) {
return;
}

const dpr =
window.devicePixelRatio || 1;

canvas.width =
width * dpr;

canvas.height =
height * dpr;

ctx.scale(dpr,dpr);

ctx.clearRect(
0,0,width,height
);

if(chartData.pm25.length === 0) {

ctx.fillStyle =
getComputedStyle(
document.documentElement
).getPropertyValue('--secondary');

ctx.font =
'14px system-ui';

ctx.textAlign =
'center';

ctx.fillText(
'Waiting for historical data...',
width/2,
height/2
);

return;

}


const left=42;
const right=12;
const top=20;
const bottom=35;

const chartWidth =
width-left-right;

const chartHeight =
height-top-bottom;

let maxValue=10;

chartData.pm25.forEach(
v => {
if(v > maxValue) maxValue=v;
}
);

chartData.pm10.forEach(
v => {
if(v > maxValue) maxValue=v;
}
);

maxValue *= 1.15;


const styles =
getComputedStyle(
document.documentElement
);

const textColor =
styles.getPropertyValue(
'--secondary'
);

const borderColor =
styles.getPropertyValue(
'--border'
);


ctx.strokeStyle =
borderColor;

ctx.lineWidth=1;

for(let i=0;i<=4;i++) {

const y =
top +
chartHeight -
(chartHeight*i/4);

ctx.beginPath();

ctx.moveTo(left,y);

ctx.lineTo(
width-right,
y
);

ctx.stroke();

ctx.fillStyle =
textColor;

ctx.font =
'11px system-ui';

ctx.textAlign =
'right';

const value =
maxValue*i/4;

ctx.fillText(
value.toFixed(0),
left-6,
y+4
);

}


ctx.fillStyle =
textColor;

ctx.font =
'11px system-ui';

ctx.textAlign =
'center';

const labelCount=5;

for(
let i=0;
i<labelCount;
i++
) {

const index =
Math.floor(
(chartData.time.length-1)
*i/
(labelCount-1)
);

const x =
left+
chartWidth*
i/
(labelCount-1);

ctx.fillText(
chartData.time[index],
x,
height-10
);

}


function drawLine(
values,
lineColor
) {

if(values.length < 2) {
return;
}

ctx.strokeStyle =
lineColor;

ctx.lineWidth=2;

ctx.beginPath();

values.forEach(
(value,index) => {

const x =
left+
chartWidth*
index/
(values.length-1);

const y =
top+
chartHeight-
(value/maxValue)*
chartHeight;

if(index===0) {
ctx.moveTo(x,y);
} else {
ctx.lineTo(x,y);
}

}
);

ctx.stroke();

}


drawLine(
chartData.pm25,
'#4caf50'
);

drawLine(
chartData.pm10,
'#ff9800'
);


ctx.font =
'12px system-ui';

ctx.textAlign =
'left';

ctx.fillStyle =
'#4caf50';

ctx.fillText(
'● PM2.5',
left,
12
);

ctx.fillStyle =
'#ff9800';

ctx.fillText(
'● PM10',
left+75,
12
);

}


async function updateHistory() {

try {

const response =
await fetch('/api/history');

const data =
await response.json();

chartData.pm25 =
data.pm25;

chartData.pm10 =
data.pm10;

chartData.time =
data.time;

document.getElementById(
'pm25Min'
).textContent =
data.pm25_min.toFixed(1);

document.getElementById(
'pm25Avg'
).textContent =
data.pm25_avg.toFixed(1);

document.getElementById(
'pm25Max'
).textContent =
data.pm25_max.toFixed(1);

document.getElementById(
'pm10Min'
).textContent =
data.pm10_min.toFixed(1);

document.getElementById(
'pm10Avg'
).textContent =
data.pm10_avg.toFixed(1);

document.getElementById(
'pm10Max'
).textContent =
data.pm10_max.toFixed(1);

document.getElementById(
'historySamples'
).textContent =
data.count + ' / 120';

drawChart();

}

catch(error) {

console.log(
'History update failed:',
error
);

}

}


async function updateData() {

try {

const response =
await fetch('/api/status');

const data =
await response.json();

document.getElementById(
'pm25'
).textContent =
data.pm2_5.toFixed(1);

document.getElementById(
'pm10'
).textContent =
data.pm1_0.toFixed(1);

document.getElementById(
'pm40'
).textContent =
data.pm4_0.toFixed(1);

document.getElementById(
'pm100'
).textContent =
data.pm10.toFixed(1);

document.getElementById(
'nc05'
).textContent =
data.nc0_5.toFixed(1);

document.getElementById(
'nc10'
).textContent =
data.nc1_0.toFixed(1);

document.getElementById(
'nc25'
).textContent =
data.nc2_5.toFixed(1);

document.getElementById(
'nc40'
).textContent =
data.nc4_0.toFixed(1);

document.getElementById(
'nc100'
).textContent =
data.nc10.toFixed(1);

document.getElementById(
'size'
).textContent =
data.typical_size.toFixed(2)
+' µm';

document.getElementById(
'firmware'
).textContent =
data.firmware;

document.getElementById(
'wifi'
).textContent =
data.wifi;

document.getElementById(
'rssi'
).textContent =
data.rssi+' dBm';

document.getElementById(
'uptime'
).textContent =
data.uptime;

document.getElementById(
'datetime'
).textContent =
data.datetime;

document.getElementById(
'sensorStatus'
).textContent =
data.sps30_online
? 'Online'
: 'Offline';

document.getElementById(
'alerts'
).textContent =
data.alerts_enabled
? 'Enabled'
: 'Disabled';

}

catch(error) {

console.log(
'Data update failed:',
error
);

}

}


updateData();

updateHistory();

setInterval(
updateData,
2000
);

setInterval(
updateHistory,
5000
);

window.addEventListener(
'resize',
drawChart
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
// STATUS API
// ============================================================

void handleApiStatus() {

  String json = "{";

  json += "\"pm1_0\":";
  json += String(pm1p0,2);

  json += ",\"pm2_5\":";
  json += String(pm2p5,2);

  json += ",\"pm4_0\":";
  json += String(pm4p0,2);

  json += ",\"pm10\":";
  json += String(pm10p0,2);

  json += ",\"nc0_5\":";
  json += String(nc0p5,2);

  json += ",\"nc1_0\":";
  json += String(nc1p0,2);

  json += ",\"nc2_5\":";
  json += String(nc2p5,2);

  json += ",\"nc4_0\":";
  json += String(nc4p0,2);

  json += ",\"nc10\":";
  json += String(nc10p0,2);

  json += ",\"typical_size\":";
  json += String(typicalSize,3);

  json += ",\"sps30_online\":";
  json += sps30Online ? "true" : "false";

  json += ",\"alerts_enabled\":";
  json += alertsEnabled ? "true" : "false";

  json += ",\"firmware\":\"";

  json += String(fwMajor);
  json += ".";
  json += String(fwMinor);

  json += "\"";

  json += ",\"rssi\":";
  json += String(WiFi.RSSI());

  json += ",\"wifi\":\"";

  json +=
    WiFi.status() ==
    WL_CONNECTED
      ? "Connected"
      : "Disconnected";

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
// HISTORY API
// ============================================================

void handleApiHistory() {

  String json = "{";

  json += "\"count\":";
  json += String(historyCount);


  // ----------------------------------------------------------
  // PM2.5
  // ----------------------------------------------------------

  json += ",\"pm25\":[";

  for (
    int i=0;
    i<historyCount;
    i++
  ) {

    int index;

    if (
      historyCount <
      HISTORY_SIZE
    ) {

      index=i;

    } else {

      index =
        (
          historyWriteIndex+i
        ) %
        HISTORY_SIZE;
    }

    if(i>0) {
      json += ",";
    }

    json +=
      String(
        pm25History[index],
        2
      );
  }

  json += "]";


  // ----------------------------------------------------------
  // PM10
  // ----------------------------------------------------------

  json += ",\"pm10\":[";

  for (
    int i=0;
    i<historyCount;
    i++
  ) {

    int index;

    if (
      historyCount <
      HISTORY_SIZE
    ) {

      index=i;

    } else {

      index =
        (
          historyWriteIndex+i
        ) %
        HISTORY_SIZE;
    }

    if(i>0) {
      json += ",";
    }

    json +=
      String(
        pm10History[index],
        2
      );
  }

  json += "]";


  // ----------------------------------------------------------
  // Time labels
  // ----------------------------------------------------------

  json += ",\"time\":[";

  for (
    int i=0;
    i<historyCount;
    i++
  ) {

    int index;

    if (
      historyCount <
      HISTORY_SIZE
    ) {

      index=i;

    } else {

      index =
        (
          historyWriteIndex+i
        ) %
        HISTORY_SIZE;
    }

    if(i>0) {
      json += ",";
    }


    unsigned long age =
      millis() -
      historyTime[index];

    unsigned long minutes =
      age / 60000UL;

    String label;

    if(minutes==0) {

      label="Now";

    } else {

      label =
        "-" +
        String(minutes) +
        "m";
    }


    json += "\"";
    json += label;
    json += "\"";
  }

  json += "]";


  // ----------------------------------------------------------
  // Statistics
  // ----------------------------------------------------------

  json += ",\"pm25_min\":";
  json += String(
    getPM25Min(),
    2
  );

  json += ",\"pm25_avg\":";
  json += String(
    getPM25Average(),
    2
  );

  json += ",\"pm25_max\":";
  json += String(
    getPM25Max(),
    2
  );

  json += ",\"pm10_min\":";
  json += String(
    getPM10Min(),
    2
  );

  json += ",\"pm10_avg\":";
  json += String(
    getPM10Average(),
    2
  );

  json += ",\"pm10_max\":";
  json += String(
    getPM10Max(),
    2
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
// CURRENT TIME
// ============================================================

String getCurrentTime() {

  struct tm timeinfo;

  if (
    !getLocalTime(
      &timeinfo
    )
  ) {

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
// UPTIME
// ============================================================

String getUptime() {

  unsigned long seconds =
    millis()/1000;

  unsigned long days =
    seconds/86400;

  seconds %= 86400;

  unsigned long hours =
    seconds/3600;

  seconds %= 3600;

  unsigned long minutes =
    seconds/60;

  seconds %= 60;

  char buffer[32];

  if(days>0) {

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
// OLED UPDATE
// ============================================================

void updateDisplay() {

  switch(currentScreen) {

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

  display.display();
}


// ============================================================
// OLED SCREEN 0
//
// V1.3 PRESERVED
// ============================================================

void drawAirQualityScreen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0,0);

  display.println(
    "AIR QUALITY"
  );

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH-1,
    10,
    SSD1306_WHITE
  );

  display.setTextSize(2);

  display.setCursor(0,16);

  display.print(
    "PM2.5 "
  );

  display.print(
    pm2p5,
    1
  );

  display.setTextSize(1);

  display.setCursor(0,40);

  display.print(
    "PM1.0 : "
  );

  display.print(
    pm1p0,
    1
  );

  display.println(
    " ug/m3"
  );

  display.setCursor(0,52);

  display.print(
    "PM10  : "
  );

  display.print(
    pm10p0,
    1
  );

  display.println(
    " ug/m3"
  );
}


// ============================================================
// OLED SCREEN 1
//
// V1.3 PRESERVED
// ============================================================

void drawClockScreen() {

  display.clearDisplay();

  struct tm timeinfo;

  display.setTextSize(1);

  if (
    getLocalTime(
      &timeinfo
    )
  ) {

    char dateBuffer[20];

    strftime(
      dateBuffer,
      sizeof(dateBuffer),
      "%a %d %b",
      &timeinfo
    );

    display.setCursor(
      0,
      4
    );

    display.print(
      dateBuffer
    );


    display.setTextSize(2);

    display.setCursor(
      68,
      0
    );

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

    display.setCursor(
      0,
      4
    );

    display.print(
      "TIME NOT SYNCED"
    );
  }


  display.drawLine(
    0,
    17,
    SCREEN_WIDTH-1,
    17,
    SSD1306_WHITE
  );


  display.setTextSize(1);


  display.setCursor(
    0,
    21
  );

  display.print(
    "PM2.5 "
  );

  display.print(
    pm2p5,
    1
  );

  display.print(
    " ug/m3"
  );


  display.setCursor(
    0,
    32
  );

  display.print(
    "PM1.0 "
  );

  display.print(
    pm1p0,
    1
  );

  display.print(
    " ug/m3"
  );


  display.setCursor(
    0,
    43
  );

  display.print(
    "PM10  "
  );

  display.print(
    pm10p0,
    1
  );

  display.print(
    " ug/m3"
  );


  display.setCursor(
    0,
    54
  );

  display.print(
    "Size  "
  );

  display.print(
    typicalSize,
    2
  );

  display.print(
    " um"
  );
}


// ============================================================
// OLED SCREEN 2
//
// V1.3 PRESERVED
// ============================================================

void drawParticleScreen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(
    0,
    0
  );

  display.println(
    "PARTICLE COUNTS"
  );

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH-1,
    10,
    SSD1306_WHITE
  );


  display.setCursor(
    0,
    16
  );

  display.print(
    "NC0.5 : "
  );

  display.println(
    nc0p5,
    1
  );


  display.setCursor(
    0,
    28
  );

  display.print(
    "NC1.0 : "
  );

  display.println(
    nc1p0,
    1
  );


  display.setCursor(
    0,
    40
  );

  display.print(
    "NC2.5 : "
  );

  display.println(
    nc2p5,
    1
  );


  display.setCursor(
    0,
    52
  );

  display.print(
    "NC10  : "
  );

  display.println(
    nc10p0,
    1
  );
}


// ============================================================
// OLED SCREEN 3
//
// NEW IN V1.4
//
// Compact system overview.
// ============================================================

void drawSystemScreen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(
    0,
    0
  );

  display.println(
    "SYSTEM"
  );

  display.drawLine(
    0,
    10,
    SCREEN_WIDTH-1,
    10,
    SSD1306_WHITE
  );


  // ----------------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------------

  display.setCursor(
    0,
    15
  );

  display.print(
    "WiFi: "
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    display.print(
      "OK "
    );

    display.print(
      WiFi.RSSI()
    );

    display.println(
      "dBm"
    );

  } else {

    display.println(
      "OFF"
    );
  }


  // ----------------------------------------------------------
  // IP
  // ----------------------------------------------------------

  display.setCursor(
    0,
    27
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    display.print(
      WiFi.localIP()
    );

  } else {

    display.print(
      "No IP"
    );
  }


  // ----------------------------------------------------------
  // History
  // ----------------------------------------------------------

  display.setCursor(
    0,
    39
  );

  display.print(
    "History: "
  );

  display.print(
    historyCount
  );

  display.print(
    "/"
  );

  display.print(
    HISTORY_SIZE
  );


  // ----------------------------------------------------------
  // Telegram
  // ----------------------------------------------------------

  display.setCursor(
    0,
    51
  );

  display.print(
    "TG: "
  );

  if (
    String(TELEGRAM_BOT_TOKEN) !=
    "YOUR_BOT_TOKEN"
  ) {

    display.print(
      "Ready"
    );

  } else {

    display.print(
      "Not configured"
    );
  }
}


// ============================================================
// SYSTEM STATUS
// ============================================================

void printSystemStatus() {

  Serial.println();

  Serial.println(
    "SYSTEM STATUS"
  );

  Serial.println(
    "-------------"
  );


  Serial.print(
    "Wi-Fi: "
  );

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
    "mDNS: http://"
  );

  Serial.print(
    MDNS_HOSTNAME
  );

  Serial.println(
    ".local"
  );


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
    "History: "
  );

  Serial.print(
    historyCount
  );

  Serial.println(
    " / 120 samples"
  );


  Serial.print(
    "Telegram: "
  );

  Serial.println(
    String(TELEGRAM_BOT_TOKEN) !=
    "YOUR_BOT_TOKEN"
      ? "Configured"
      : "Not configured"
  );
}