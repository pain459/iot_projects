#!/usr/bin/env python3
# Upgrade ESP32-S3 Air Quality Monitor firmware V1.7.2 -> V1.8.
#
# V1.8 is intentionally a display-only revision:
#   - Replaces the TFT carousel with one static 320x240 dashboard.
#   - PM2.5 and CO2 are the large primary tiles.
#   - PM1, PM4, PM10, VOC and NOx are compact secondary tiles.
#   - SHT45 temperature and humidity remain visible at the bottom.
#   - Header clock is HH:MM only and redraws only when minute changes.
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
void drawDashboardClock(bool force);

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
// TFT DASHBOARD - V1.8
//
// One fixed 320x240 screen. There is no carousel.
//
// Layout:
//   Header: AIR QUALITY + HH:MM
//   Primary: PM2.5 | CO2
//   Secondary: PM1 | PM4 | PM10 | VOC | NOx
//   Bottom: Temperature | Humidity
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

// PM alert behavior is NOT changed.
//
// Red means the existing configured alert threshold is crossed.
// Yellow is only a visual "approaching threshold" state and does
// not trigger Telegram or alter PM alert state.
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

// Previous CO2 display already used:
//   <800 green
//   800-999 yellow
//   1000-1499 orange
//   >=1500 red
//
// V1.8 collapses yellow + orange into the requested yellow band.
// This changes only TFT presentation.
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

// Reuse the existing VOC interpretation:
//   <=100 baseline
//   101-250 mild/elevated
//   >250 high/very high
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

// Reuse the existing NOx interpretation:
//   <=1 baseline
//   2-150 minor/elevated
//   >150 high/very high
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
    6,
    background
  );

  tft.drawRoundRect(
    x,
    y,
    w,
    h,
    6,
    ILI9341_BLACK
  );

  drawCenteredText(
    label,
    x,
    y + 5,
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
        ? 4
        : 3;
  } else {
    valueSize =
      valueLength <= 4
        ? 2
        : 1;
  }

  drawCenteredText(
    value,
    x,
    primary ? y + 29 : y + 23,
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
      y + h - 12,
      w,
      1,
      foreground
    );
  }
}

void drawDashboardFrame() {
  tft.fillScreen(ILI9341_BLACK);

  tft.fillRect(
    0,
    0,
    320,
    30,
    ILI9341_NAVY
  );

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print("AIR QUALITY");

  tft.setTextSize(1);
  tft.setCursor(156, 11);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.print("V1.8");

  tft.drawFastHLine(
    0,
    108,
    320,
    ILI9341_DARKGREY
  );

  tft.drawFastHLine(
    0,
    168,
    320,
    ILI9341_DARKGREY
  );

  lastDisplayedMinute = -2;
  drawDashboardClock(true);
}

void drawDashboardClock(
  bool force
) {
  struct tm t;

  if (!getLocalTime(&t, 50)) {
    if (
      force ||
      lastDisplayedMinute != -1
    ) {
      tft.fillRect(
        252,
        0,
        68,
        30,
        ILI9341_NAVY
      );

      tft.setTextColor(ILI9341_YELLOW);
      tft.setTextSize(2);
      tft.setCursor(260, 8);
      tft.print("--:--");

      lastDisplayedMinute = -1;
    }

    return;
  }

  timeSynchronized = true;

  if (
    !force &&
    lastDisplayedMinute == t.tm_min
  ) {
    return;
  }

  char clockText[6];

  strftime(
    clockText,
    sizeof(clockText),
    "%H:%M",
    &t
  );

  tft.fillRect(
    252,
    0,
    68,
    30,
    ILI9341_NAVY
  );

  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(258, 8);
  tft.print(clockText);

  lastDisplayedMinute = t.tm_min;
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

  if (lastMeasurementMillis > 0) {
    snprintf(
      pm25Text,
      sizeof(pm25Text),
      "%.1f",
      pm2p5
    );
  } else {
    snprintf(
      pm25Text,
      sizeof(pm25Text),
      "--"
    );
  }

  if (scd4xHasReading) {
    snprintf(
      co2Text,
      sizeof(co2Text),
      "%u",
      co2ppm
    );
  } else if (scd4xOnline) {
    snprintf(
      co2Text,
      sizeof(co2Text),
      "WARM"
    );
  } else {
    snprintf(
      co2Text,
      sizeof(co2Text),
      "OFF"
    );
  }

  drawDashboardTile(
    4, 34, 154, 70,
    "PM2.5",
    pm25Text,
    "ug/m3",
    lastMeasurementMillis > 0
      ? classifyPM(
          pm2p5,
          pm25AlertThreshold
        )
      : DISPLAY_WARNING,
    true
  );

  drawDashboardTile(
    162, 34, 154, 70,
    "CO2",
    co2Text,
    scd4xHasReading
      ? "ppm"
      : "",
    classifyCO2(),
    true
  );

  DisplayLevel pm25Level =
    lastMeasurementMillis > 0
      ? classifyPM(
          pm2p5,
          pm25AlertThreshold
        )
      : DISPLAY_WARNING;

  DisplayLevel pm10Level =
    lastMeasurementMillis > 0
      ? classifyPM(
          pm10p0,
          pm10AlertThreshold
        )
      : DISPLAY_WARNING;

  if (lastMeasurementMillis > 0) {
    snprintf(
      pm1Text,
      sizeof(pm1Text),
      "%.1f",
      pm1p0
    );

    snprintf(
      pm4Text,
      sizeof(pm4Text),
      "%.1f",
      pm4p0
    );

    snprintf(
      pm10Text,
      sizeof(pm10Text),
      "%.1f",
      pm10p0
    );
  } else {
    snprintf(pm1Text, sizeof(pm1Text), "--");
    snprintf(pm4Text, sizeof(pm4Text), "--");
    snprintf(pm10Text, sizeof(pm10Text), "--");
  }

  if (sgp41HasReading) {
    snprintf(
      vocText,
      sizeof(vocText),
      "%ld",
      static_cast<long>(
        vocIndex
      )
    );

    snprintf(
      noxText,
      sizeof(noxText),
      "%ld",
      static_cast<long>(
        noxIndex
      )
    );
  } else if (sgp41Online) {
    snprintf(
      vocText,
      sizeof(vocText),
      "WARM"
    );

    snprintf(
      noxText,
      sizeof(noxText),
      "WARM"
    );
  } else {
    snprintf(
      vocText,
      sizeof(vocText),
      "OFF"
    );

    snprintf(
      noxText,
      sizeof(noxText),
      "OFF"
    );
  }

  // PM1 and PM4 inherit PM2.5 visual status. The existing firmware has no
  // independent PM1/PM4 alert thresholds.
  drawDashboardTile(
    4, 112, 60, 52,
    "PM1",
    pm1Text,
    "ug/m3",
    pm25Level,
    false
  );

  drawDashboardTile(
    67, 112, 60, 52,
    "PM4",
    pm4Text,
    "ug/m3",
    pm25Level,
    false
  );

  drawDashboardTile(
    130, 112, 60, 52,
    "PM10",
    pm10Text,
    "ug/m3",
    pm10Level,
    false
  );

  drawDashboardTile(
    193, 112, 60, 52,
    "VOC",
    vocText,
    sgp41HasReading
      ? "index"
      : "",
    classifyVOC(),
    false
  );

  drawDashboardTile(
    256, 112, 60, 52,
    "NOx",
    noxText,
    sgp41HasReading
      ? "index"
      : "",
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
    snprintf(
      tempText,
      sizeof(tempText),
      "--"
    );

    snprintf(
      humidityText,
      sizeof(humidityText),
      "--"
    );
  }

  DisplayLevel envLevel =
    (
      sht45Online &&
      sht45HasReading
    )
      ? DISPLAY_NEUTRAL
      : DISPLAY_BAD;

  drawDashboardTile(
    4, 172, 154, 52,
    "TEMPERATURE",
    tempText,
    sht45HasReading
      ? "C"
      : "",
    envLevel,
    false
  );

  drawDashboardTile(
    162, 172, 154, 52,
    "HUMIDITY",
    humidityText,
    sht45HasReading
      ? "%"
      : "",
    envLevel,
    false
  );

  tft.fillRect(
    0,
    227,
    320,
    13,
    ILI9341_BLACK
  );

  tft.setTextSize(1);

  tft.setCursor(5, 229);
  tft.setTextColor(
    sps30Online
      ? ILI9341_GREEN
      : ILI9341_RED
  );
  tft.print(
    sps30Online
      ? "SPS30 OK"
      : "SPS30 OFF"
  );

  tft.setCursor(86, 229);
  tft.setTextColor(
    scd4xOnline
      ? (
          scd4xHasReading
            ? ILI9341_GREEN
            : ILI9341_YELLOW
        )
      : ILI9341_RED
  );
  tft.print(
    !scd4xOnline
      ? "CO2 OFF"
      : (
          scd4xHasReading
            ? "CO2 OK"
            : "CO2 WARM"
        )
  );

  tft.setCursor(160, 229);
  tft.setTextColor(
    sht45Online
      ? ILI9341_GREEN
      : ILI9341_RED
  );
  tft.print(
    sht45Online
      ? "ENV OK"
      : "ENV OFF"
  );

  tft.setCursor(232, 229);
  tft.setTextColor(
    sgp41Online
      ? (
          sgp41HasReading
            ? ILI9341_GREEN
            : ILI9341_YELLOW
        )
      : ILI9341_RED
  );
  tft.print(
    !sgp41Online
      ? "GAS OFF"
      : (
          sgp41HasReading
            ? "GAS OK"
            : "GAS WARM"
        )
  );

  drawDashboardClock(false);
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

// -2 = never drawn, -1 = time unavailable, 0..59 = displayed minute.
int lastDisplayedMinute = -2;'''

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
  // Sensor values refresh every 2 seconds; the clock region itself
  // redraws only when the displayed minute changes.
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
