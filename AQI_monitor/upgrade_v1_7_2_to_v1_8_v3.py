#!/usr/bin/env python3
# Upgrade ESP32-S3 Air Quality Monitor firmware V1.7.2 -> V1.8.
#
# V1.8 is intentionally a display-only revision:
#   - Replaces the TFT carousel with one static 320x240 dashboard.
#   - PM2.5 and CO2 are the large primary tiles.
#   - PM1, PM4, PM10, VOC and NOx are compact secondary tiles.
#   - Top bar shows weekday/date plus HH:MM; no firmware branding.
#   - Adds a full-width GOOD / ELEVATED / POOR room-status banner.
#   - Bottom row shows SHT45 temperature/humidity plus SPS30 TPS.
#   - Temperature/humidity use native TFT icons (no Unicode fonts).
#   - Footer shows airmonitor.local, Wi-Fi RSSI and update age.
#   - Green / yellow / red tiles make status visible from a distance.
#   - Existing sensor acquisition, API, web UI, history, LittleFS gas
#     persistence, Telegram and alert logic are preserved.
#
# Usage:
#   python3 upgrade_v1_7_2_to_v1_8.py air_quality_monitor_v1_7_2_gas_persistence.ino
#
# Optional explicit output:
#   python3 upgrade_v1_7_2_to_v1_8.py input.ino output.ino

from pathlib import Path
import re
import sys

DISPLAY_DECLARATIONS = r'''
void updateDisplay();
void drawDashboardFrame();
void drawDashboardDateTime(bool force);
void drawOverallStatus();
void drawSystemFooter();

enum DisplayLevel : uint8_t {
  DISPLAY_NEUTRAL = 0,
  DISPLAY_GOOD,
  DISPLAY_WARNING,
  DISPLAY_BAD
};

DisplayLevel classifyPM(float value, float alertThreshold);
DisplayLevel classifyCO2();
DisplayLevel classifyVOC();
DisplayLevel classifyNOx();
DisplayLevel overallDisplayLevel();

const char* overallStatusText(DisplayLevel level);

uint16_t displayLevelColor(DisplayLevel level);
uint16_t displayLevelTextColor(DisplayLevel level);

void drawDashboardTile(
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  const char* label,
  const char* value,
  const char* unit,
  DisplayLevel level,
  bool primary
);

void drawEnvironmentTile(
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  const char* label,
  const char* value,
  const char* unit,
  uint8_t iconType,
  DisplayLevel level
);

void drawThermometerIcon(int16_t x, int16_t y);
void drawHumidityIcon(int16_t x, int16_t y);

void drawCenteredText(
  const char* text,
  int16_t x,
  int16_t y,
  int16_t w,
  uint8_t size,
  uint16_t color
);

void showFatalError(const char* title, const char* line1, const char* line2);
'''.strip()

DISPLAY_IMPLEMENTATION = r'''
// ============================================================
// TFT DASHBOARD - V1.8 FINAL BREADBOARD FIRMWARE
//
// One fixed 320x240 screen. There is no carousel.
//
// Layout:
//   Header: Fri 04 Sep                         22:46
//   Full-width room status banner
//   Primary: PM2.5 | CO2
//   Secondary: PM1 | PM4 | PM10 | VOC | NOx
//   Environment: temperature | humidity | typical particle size
//   Footer: hostname | Wi-Fi RSSI | update age
//
// SHT45 remains the authoritative room temperature/humidity source.
// SCD4X environmental readings stay diagnostic only.
//
// Status colors are presentation-only. Existing alerts, API,
// history, Telegram and sensor evaluation are not changed.
// ============================================================

uint16_t displayLevelColor(
  DisplayLevel level
) {
  switch (level) {
    case DISPLAY_GOOD:
      return ILI9341_DARKGREEN;

    case DISPLAY_WARNING:
      return ILI9341_YELLOW;

    case DISPLAY_BAD:
      return ILI9341_RED;

    case DISPLAY_NEUTRAL:
    default:
      return ILI9341_DARKGREY;
  }
}

uint16_t displayLevelTextColor(
  DisplayLevel level
) {
  return
    level == DISPLAY_WARNING
      ? ILI9341_BLACK
      : ILI9341_WHITE;
}

DisplayLevel classifyPM(
  float value,
  float alertThreshold
) {
  if (value >= alertThreshold) {
    return DISPLAY_BAD;
  }

  if (value >= alertThreshold * 0.75f) {
    return DISPLAY_WARNING;
  }

  return DISPLAY_GOOD;
}

DisplayLevel classifyCO2() {
  if (!scd4xOnline) {
    return DISPLAY_BAD;
  }

  if (!scd4xHasReading) {
    return DISPLAY_WARNING;
  }

  if (co2ppm >= 1500) {
    return DISPLAY_BAD;
  }

  if (co2ppm >= 800) {
    return DISPLAY_WARNING;
  }

  return DISPLAY_GOOD;
}

DisplayLevel classifyVOC() {
  if (!sgp41Online) {
    return DISPLAY_BAD;
  }

  if (!sgp41HasReading) {
    return DISPLAY_WARNING;
  }

  if (vocIndex > 250) {
    return DISPLAY_BAD;
  }

  if (vocIndex > 100) {
    return DISPLAY_WARNING;
  }

  return DISPLAY_GOOD;
}

DisplayLevel classifyNOx() {
  if (!sgp41Online) {
    return DISPLAY_BAD;
  }

  if (!sgp41HasReading) {
    return DISPLAY_WARNING;
  }

  if (noxIndex > 150) {
    return DISPLAY_BAD;
  }

  if (noxIndex > 1) {
    return DISPLAY_WARNING;
  }

  return DISPLAY_GOOD;
}

DisplayLevel overallDisplayLevel() {
  if (
    !sps30Online ||
    !scd4xOnline ||
    !sht45Online ||
    !sgp41Online
  ) {
    return DISPLAY_BAD;
  }

  if (
    lastMeasurementMillis == 0 ||
    !scd4xHasReading ||
    !sht45HasReading ||
    !sgp41HasReading
  ) {
    return DISPLAY_WARNING;
  }

  DisplayLevel level =
    DISPLAY_GOOD;

  DisplayLevel candidate =
    classifyPM(
      pm2p5,
      pm25AlertThreshold
    );

  if (candidate > level) {
    level = candidate;
  }

  candidate =
    classifyPM(
      pm10p0,
      pm10AlertThreshold
    );

  if (candidate > level) {
    level = candidate;
  }

  candidate = classifyCO2();

  if (candidate > level) {
    level = candidate;
  }

  candidate = classifyVOC();

  if (candidate > level) {
    level = candidate;
  }

  candidate = classifyNOx();

  if (candidate > level) {
    level = candidate;
  }

  return level;
}

const char* overallStatusText(
  DisplayLevel level
) {
  if (
    !sps30Online ||
    !scd4xOnline ||
    !sht45Online ||
    !sgp41Online
  ) {
    return "AIR QUALITY: SENSOR CHECK";
  }

  if (
    lastMeasurementMillis == 0 ||
    !scd4xHasReading ||
    !sht45HasReading ||
    !sgp41HasReading
  ) {
    return "AIR QUALITY: STARTING";
  }

  switch (level) {
    case DISPLAY_BAD:
      return "AIR QUALITY: POOR";

    case DISPLAY_WARNING:
      return "AIR QUALITY: ELEVATED";

    case DISPLAY_GOOD:
    default:
      return "AIR QUALITY: GOOD";
  }
}

void drawCenteredText(
  const char* text,
  int16_t x,
  int16_t y,
  int16_t w,
  uint8_t size,
  uint16_t color
) {
  if (!text) {
    return;
  }

  int16_t bx = 0;
  int16_t by = 0;
  uint16_t bw = 0;
  uint16_t bh = 0;

  tft.setTextSize(size);
  tft.setTextColor(color);

  tft.getTextBounds(
    text,
    0,
    0,
    &bx,
    &by,
    &bw,
    &bh
  );

  int16_t tx =
    x +
    (w - static_cast<int16_t>(bw)) / 2;

  if (tx < x + 2) {
    tx = x + 2;
  }

  tft.setCursor(
    tx,
    y
  );

  tft.print(text);
}

void drawDashboardTile(
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  const char* label,
  const char* value,
  const char* unit,
  DisplayLevel level,
  bool primary
) {
  uint16_t background =
    displayLevelColor(level);

  uint16_t foreground =
    displayLevelTextColor(level);

  tft.fillRoundRect(
    x,
    y,
    w,
    h,
    5,
    background
  );

  tft.drawRoundRect(
    x,
    y,
    w,
    h,
    5,
    ILI9341_BLACK
  );

  drawCenteredText(
    label,
    x,
    y + 4,
    w,
    primary ? 2 : 1,
    foreground
  );

  size_t valueLength =
    value
      ? strlen(value)
      : 0;

  uint8_t valueSize;

  if (primary) {
    valueSize =
      valueLength <= 4
        ? 3
        : 2;
  } else {
    valueSize =
      valueLength <= 4
        ? 2
        : 1;
  }

  drawCenteredText(
    value,
    x,
    primary ? y + 23 : y + 16,
    w,
    valueSize,
    foreground
  );

  if (
    unit &&
    unit[0] != '\0'
  ) {
    drawCenteredText(
      unit,
      x,
      y + h - 10,
      w,
      1,
      foreground
    );
  }
}

void drawThermometerIcon(
  int16_t x,
  int16_t y
) {
  tft.drawRoundRect(
    x + 3,
    y,
    6,
    13,
    3,
    ILI9341_ORANGE
  );

  tft.fillRect(
    x + 5,
    y + 4,
    2,
    10,
    ILI9341_ORANGE
  );

  tft.fillCircle(
    x + 6,
    y + 14,
    5,
    ILI9341_ORANGE
  );
}

void drawHumidityIcon(
  int16_t x,
  int16_t y
) {
  tft.fillTriangle(
    x + 6,
    y,
    x + 1,
    y + 10,
    x + 11,
    y + 10,
    ILI9341_CYAN
  );

  tft.fillCircle(
    x + 6,
    y + 10,
    5,
    ILI9341_CYAN
  );
}

void drawEnvironmentTile(
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  const char* label,
  const char* value,
  const char* unit,
  uint8_t iconType,
  DisplayLevel level
) {
  uint16_t background =
    displayLevelColor(level);

  uint16_t foreground =
    displayLevelTextColor(level);

  tft.fillRoundRect(
    x,
    y,
    w,
    h,
    5,
    background
  );

  tft.drawRoundRect(
    x,
    y,
    w,
    h,
    5,
    ILI9341_BLACK
  );

  drawCenteredText(
    label,
    x,
    y + 4,
    w,
    1,
    foreground
  );

  if (iconType == 1) {
    drawThermometerIcon(
      x + 8,
      y + 19
    );
  } else if (iconType == 2) {
    drawHumidityIcon(
      x + 8,
      y + 20
    );
  }

  int16_t valueX =
    iconType == 0
      ? x
      : x + 21;

  int16_t valueW =
    iconType == 0
      ? w
      : w - 23;

  size_t valueLength =
    value
      ? strlen(value)
      : 0;

  uint8_t valueSize =
    valueLength <= 4
      ? 2
      : 1;

  drawCenteredText(
    value,
    valueX,
    y + 20,
    valueW,
    valueSize,
    foreground
  );

  if (
    unit &&
    unit[0] != '\0'
  ) {
    tft.setTextSize(1);
    tft.setTextColor(foreground);

    tft.setCursor(
      x + w - 17,
      y + 28
    );

    tft.print(unit);
  }
}

void drawDashboardFrame() {
  tft.fillScreen(ILI9341_BLACK);

  tft.fillRect(
    0,
    0,
    320,
    26,
    ILI9341_NAVY
  );

  lastDisplayedMinute = -2;
  lastDisplayedDay = -2;

  drawDashboardDateTime(true);
}

void drawDashboardDateTime(
  bool force
) {
  struct tm t;

  if (!getLocalTime(&t, 50)) {
    if (
      force ||
      lastDisplayedMinute != -1 ||
      lastDisplayedDay != -1
    ) {
      tft.fillRect(
        0,
        0,
        320,
        26,
        ILI9341_NAVY
      );

      tft.setTextColor(ILI9341_YELLOW);
      tft.setTextSize(2);

      tft.setCursor(7, 6);
      tft.print("--- -- ---");

      tft.setCursor(258, 6);
      tft.print("--:--");

      lastDisplayedMinute = -1;
      lastDisplayedDay = -1;
    }

    return;
  }

  timeSynchronized = true;

  if (
    !force &&
    lastDisplayedMinute == t.tm_min &&
    lastDisplayedDay == t.tm_yday
  ) {
    return;
  }

  char dateText[16];
  char clockText[6];

  strftime(
    dateText,
    sizeof(dateText),
    "%a %d %b",
    &t
  );

  strftime(
    clockText,
    sizeof(clockText),
    "%H:%M",
    &t
  );

  tft.fillRect(
    0,
    0,
    320,
    26,
    ILI9341_NAVY
  );

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);

  tft.setCursor(7, 6);
  tft.print(dateText);

  tft.setCursor(258, 6);
  tft.print(clockText);

  lastDisplayedMinute = t.tm_min;
  lastDisplayedDay = t.tm_yday;
}

void drawOverallStatus() {
  DisplayLevel level =
    overallDisplayLevel();

  uint16_t background =
    displayLevelColor(level);

  uint16_t foreground =
    displayLevelTextColor(level);

  tft.fillRect(
    0,
    28,
    320,
    22,
    background
  );

  drawCenteredText(
    overallStatusText(level),
    0,
    32,
    320,
    2,
    foreground
  );
}

void drawSystemFooter() {
  tft.fillRect(
    0,
    212,
    320,
    28,
    ILI9341_BLACK
  );

  tft.setTextSize(1);

  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(4, 221);
  tft.print("airmonitor.local");

  tft.setCursor(114, 221);

  if (WiFi.status() == WL_CONNECTED) {
    long rssi =
      WiFi.RSSI();

    tft.setTextColor(
      rssi >= -67
        ? ILI9341_GREEN
        : (
            rssi >= -75
              ? ILI9341_YELLOW
              : ILI9341_RED
          )
    );

    tft.print("WiFi ");
    tft.print(rssi);
    tft.print("dBm");
  } else {
    tft.setTextColor(ILI9341_RED);
    tft.print("WiFi OFF");
  }

  tft.setCursor(232, 221);

  if (lastMeasurementMillis == 0) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.print("Updated --");
  } else {
    unsigned long ageSeconds =
      (millis() - lastMeasurementMillis) / 1000UL;

    tft.setTextColor(
      ageSeconds <= 10
        ? ILI9341_GREEN
        : (
            ageSeconds <= 30
              ? ILI9341_YELLOW
              : ILI9341_RED
          )
    );

    tft.print("Updated ");
    tft.print(ageSeconds);
    tft.print("s");
  }
}

void updateDisplay() {
  char pm25Text[12];
  char co2Text[12];

  char pm1Text[12];
  char pm4Text[12];
  char pm10Text[12];

  char vocText[12];
  char noxText[12];

  char tempText[12];
  char humidityText[12];
  char particleSizeText[12];

  drawOverallStatus();

  if (lastMeasurementMillis > 0) {
    snprintf(
      pm25Text,
      sizeof(pm25Text),
      "%.1f",
      pm2p5
    );
  } else {
    snprintf(pm25Text, sizeof(pm25Text), "--");
  }

  if (scd4xHasReading) {
    snprintf(
      co2Text,
      sizeof(co2Text),
      "%u",
      co2ppm
    );
  } else if (scd4xOnline) {
    snprintf(co2Text, sizeof(co2Text), "WARM");
  } else {
    snprintf(co2Text, sizeof(co2Text), "OFF");
  }

  drawDashboardTile(
    4, 54, 154, 61,
    "PM2.5",
    pm25Text,
    "ug/m3",
    lastMeasurementMillis > 0
      ? classifyPM(pm2p5, pm25AlertThreshold)
      : DISPLAY_WARNING,
    true
  );

  drawDashboardTile(
    162, 54, 154, 61,
    "CO2",
    co2Text,
    scd4xHasReading ? "ppm" : "",
    classifyCO2(),
    true
  );

  DisplayLevel pm25Level =
    lastMeasurementMillis > 0
      ? classifyPM(pm2p5, pm25AlertThreshold)
      : DISPLAY_WARNING;

  DisplayLevel pm10Level =
    lastMeasurementMillis > 0
      ? classifyPM(pm10p0, pm10AlertThreshold)
      : DISPLAY_WARNING;

  if (lastMeasurementMillis > 0) {
    snprintf(pm1Text, sizeof(pm1Text), "%.1f", pm1p0);
    snprintf(pm4Text, sizeof(pm4Text), "%.1f", pm4p0);
    snprintf(pm10Text, sizeof(pm10Text), "%.1f", pm10p0);
    snprintf(
      particleSizeText,
      sizeof(particleSizeText),
      "%.2f",
      typicalSize
    );
  } else {
    snprintf(pm1Text, sizeof(pm1Text), "--");
    snprintf(pm4Text, sizeof(pm4Text), "--");
    snprintf(pm10Text, sizeof(pm10Text), "--");
    snprintf(particleSizeText, sizeof(particleSizeText), "--");
  }

  if (sgp41HasReading) {
    snprintf(
      vocText,
      sizeof(vocText),
      "%ld",
      static_cast<long>(vocIndex)
    );

    snprintf(
      noxText,
      sizeof(noxText),
      "%ld",
      static_cast<long>(noxIndex)
    );
  } else if (sgp41Online) {
    snprintf(vocText, sizeof(vocText), "WARM");
    snprintf(noxText, sizeof(noxText), "WARM");
  } else {
    snprintf(vocText, sizeof(vocText), "OFF");
    snprintf(noxText, sizeof(noxText), "OFF");
  }

  drawDashboardTile(
    4, 118, 60, 44,
    "PM1",
    pm1Text,
    "ug/m3",
    pm25Level,
    false
  );

  drawDashboardTile(
    67, 118, 60, 44,
    "PM4",
    pm4Text,
    "ug/m3",
    pm25Level,
    false
  );

  drawDashboardTile(
    130, 118, 60, 44,
    "PM10",
    pm10Text,
    "ug/m3",
    pm10Level,
    false
  );

  drawDashboardTile(
    193, 118, 60, 44,
    "VOC",
    vocText,
    sgp41HasReading ? "index" : "",
    classifyVOC(),
    false
  );

  drawDashboardTile(
    256, 118, 60, 44,
    "NOx",
    noxText,
    sgp41HasReading ? "index" : "",
    classifyNOx(),
    false
  );

  if (sht45HasReading) {
    snprintf(
      tempText,
      sizeof(tempText),
      "%.1f",
      shtTemperature
    );

    snprintf(
      humidityText,
      sizeof(humidityText),
      "%.0f",
      shtHumidity
    );
  } else {
    snprintf(tempText, sizeof(tempText), "--");
    snprintf(humidityText, sizeof(humidityText), "--");
  }

  DisplayLevel envLevel =
    (
      sht45Online &&
      sht45HasReading
    )
      ? DISPLAY_NEUTRAL
      : DISPLAY_BAD;

  DisplayLevel particleSizeLevel =
    (
      sps30Online &&
      lastMeasurementMillis > 0
    )
      ? DISPLAY_NEUTRAL
      : DISPLAY_BAD;

  // iconType: 1=thermometer, 2=droplet, 0=no icon.
  drawEnvironmentTile(
    4, 165, 102, 44,
    "TEMP",
    tempText,
    sht45HasReading ? "C" : "",
    1,
    envLevel
  );

  drawEnvironmentTile(
    109, 165, 102, 44,
    "HUMIDITY",
    humidityText,
    sht45HasReading ? "%" : "",
    2,
    envLevel
  );

  drawEnvironmentTile(
    214, 165, 102, 44,
    "TPS",
    particleSizeText,
    lastMeasurementMillis > 0 ? "um" : "",
    0,
    particleSizeLevel
  );

  drawSystemFooter();
  drawDashboardDateTime(false);
}

void showFatalError(
  const char* title,
  const char* line1,
  const char* line2
) {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(
    0,
    0,
    320,
    46,
    ILI9341_RED
  );

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
'''.strip()


def must_replace(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"Could not find expected V1.7.2 block: {label}")
    return text.replace(old, new, 1)


def upgrade(text: str) -> str:
    if "V1.7.2" not in text:
        raise RuntimeError(
            "Input does not look like air_quality_monitor_v1_7_2_gas_persistence.ino"
        )

    # Version labels only. This does not touch SPS30's fwMajor/fwMinor values.
    text = text.replace("V1.7.2", "V1.8")

    # Update stale top comment when present.
    text = text.replace(
        "    - V1.8 adds SCD4X CO2 / temperature / humidity and uses a five-screen carousel.\n"
        "    - The TFT is redrawn on screen changes rather than every sensor sample,\n"
        "      avoiding full-screen SPI flicker while measurements continue normally.",
        "    - V1.8 replaces the TFT carousel with one fixed 320x240 dashboard.\n"
        "    - Sensor tiles refresh in place; the screen never rotates.\n"
        "    - The top-right clock is HH:MM and redraws only when the minute changes."
    )

    old_timing = '''unsigned long lastMeasurementMillis = 0;
unsigned long lastHistorySample = 0;
unsigned long lastScreenChange = 0;

#define HISTORY_INTERVAL 30000UL
#define SCREEN_ROTATION_INTERVAL 5000UL

int currentScreen = 0;'''

    new_timing = '''unsigned long lastMeasurementMillis = 0;
unsigned long lastHistorySample = 0;
unsigned long lastDisplayRefresh = 0;

#define HISTORY_INTERVAL 30000UL
#define DISPLAY_REFRESH_INTERVAL 2000UL

// Header redraw state.
// -2 = never drawn, -1 = time unavailable.
int lastDisplayedMinute = -2;
int lastDisplayedDay = -2;'''

    text = must_replace(
        text,
        old_timing,
        new_timing,
        "display timing"
    )

    decl_pattern = re.compile(
        r'''void updateDisplay\(\);\s*
void drawAirQualityScreen\(\);\s*
void drawClockScreen\(\);\s*
void drawParticleScreen\(\);\s*
void drawCO2Screen\(\);\s*
void drawEnvironmentScreen\(\);\s*
void drawGasScreen\(\);\s*
void drawSystemScreen\(\);\s*

void drawHeader\(const char\* title, uint16_t accent\);\s*
void drawMetricCard\(int16_t x, int16_t y, int16_t w, int16_t h,\s*
\s*const char\* label, float value, const char\* unit,\s*
\s*uint16_t color\);\s*
void showFatalError\(const char\* title, const char\* line1, const char\* line2\);''',
        re.MULTILINE
    )

    text, count = decl_pattern.subn(
        DISPLAY_DECLARATIONS,
        text,
        count=1
    )

    if count != 1:
        raise RuntimeError(
            "Could not replace the V1.7.2 TFT function declarations."
        )

    old_setup = '''  currentScreen = 0;
  lastScreenChange = millis();
  lastHistorySample = millis();

  updateDisplay();'''

    new_setup = '''  lastDisplayRefresh = 0;
  lastHistorySample = millis();

  drawDashboardFrame();
  updateDisplay();
  lastDisplayRefresh = millis();'''

    text = must_replace(
        text,
        old_setup,
        new_setup,
        "initial TFT setup"
    )

    old_loop = '''  // TFT carousel.
  if (
    millis() - lastScreenChange >= SCREEN_ROTATION_INTERVAL
  ) {
    currentScreen++;

    if (currentScreen > 6) {
      currentScreen = 0;
    }

    lastScreenChange = millis();
    updateDisplay();
  }'''

    new_loop = '''  // Static TFT dashboard.
  // Sensor values refresh every 2 seconds; the date/time header itself
  // redraws only when its minute or day changes.
  if (
    millis() - lastDisplayRefresh >= DISPLAY_REFRESH_INTERVAL
  ) {
    lastDisplayRefresh = millis();
    updateDisplay();
  }'''

    text = must_replace(
        text,
        old_loop,
        new_loop,
        "TFT carousel loop"
    )

    start_marker = '''// ============================================================
// TFT HELPERS
// ============================================================'''

    end_marker = '''// ============================================================
// WEB DASHBOARD ASSET
// IMPORTANT: large web assets must stay at global PROGMEM scope.
// Never move this inside handleRoot().
// ============================================================'''

    start = text.find(start_marker)
    end = text.find(end_marker)

    if start < 0 or end < 0 or end <= start:
        raise RuntimeError(
            "Could not locate the V1.7.2 TFT implementation boundaries."
        )

    text = (
        text[:start]
        + DISPLAY_IMPLEMENTATION
        + "\n\n"
        + text[end:]
    )

    text = text.replace(
        "// The active screen refreshes when the carousel changes.",
        "// The static dashboard refreshes on the display timer."
    )

    forbidden = [
        "SCREEN_ROTATION_INTERVAL",
        "currentScreen",
        "lastScreenChange",
        "drawAirQualityScreen(",
        "drawClockScreen(",
        "drawParticleScreen(",
        "drawCO2Screen(",
        "drawEnvironmentScreen(",
        "drawGasScreen(",
        "drawSystemScreen(",
    ]

    leftovers = [
        item
        for item in forbidden
        if item in text
    ]

    if leftovers:
        raise RuntimeError(
            "Carousel references remain after patch: "
            + ", ".join(leftovers)
        )

    if "V1.7.2" in text:
        raise RuntimeError(
            "A V1.7.2 version label remains after patch."
        )

    if "static const char INDEX_HTML[] PROGMEM" not in text:
        raise RuntimeError(
            "Web dashboard asset was lost during patching."
        )

    if "GAS_PERSIST_DAYS 7" not in text:
        raise RuntimeError(
            "VOC/NOx persistence definitions were lost during patching."
        )

    return text


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "Usage: python3 upgrade_v1_7_2_to_v1_8.py "
            "<v1.7.2.ino> [output_v1.8.ino]",
            file=sys.stderr
        )
        return 2

    src = Path(sys.argv[1])

    if len(sys.argv) == 3:
        dst = Path(sys.argv[2])
    else:
        dst = src.with_name(
            "air_quality_monitor_v1_8_static_dashboard.ino"
        )

    original = src.read_text(encoding="utf-8")
    upgraded = upgrade(original)

    dst.write_text(
        upgraded,
        encoding="utf-8"
    )

    print(f"Created: {dst}")
    print("V1.8 display revision applied.")
    print("Sensor/API/history/Telegram/alert sections were preserved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
