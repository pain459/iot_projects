# Local Air Quality Monitor — Prototype Phase Complete

## Final breadboard hardware

- ESP32-S3
- Sensirion SPS30
- Sensirion SCD4X
- 7Semi SHT45 + SGP41
- 2.8-inch ILI9341 TFT

## Final firmware reference

**V1.8** is the frozen firmware baseline for PCB bring-up.

It includes:

- static local dashboard
- PM1 / PM2.5 / PM4 / PM10
- SPS30 Typical Particle Size
- CO2
- SHT45 temperature and humidity
- VOC Index and NOx Index
- overall color status
- date/time
- Wi-Fi/freshness diagnostics
- web dashboard and JSON APIs
- Telegram integration and particulate alerts
- 60-minute RAM history
- 7-day persistent VOC/NOx history
- mDNS at `airmonitor.local`

## Frozen design decisions

1. SHT45 is authoritative for room temperature and humidity.
2. SCD4X is operationally used for CO2; its T/RH remain diagnostics only.
3. SPS30 remains the particulate sensor.
4. SGP41 remains the VOC/NOx sensor and uses SHT45 compensation.
5. ESP32-S3 remains the controller.
6. ILI9341 remains the local display.
7. Existing API contracts stay stable through initial PCB bring-up.
8. PCB revision 1 should reproduce the known-good breadboard electrically before optimizing.

## PCB phase priorities

- 5 V / 3.3 V power domains and current budget
- decoupling and bulk capacitance
- common grounding
- shared I2C routing
- TFT SPI routing
- sensor connectors
- ESP32-S3 module/programming/debug access
- test points
- mounting holes
- enclosure airflow
- keeping SHT45 away from heat sources
- unobstructed SPS30 inlet/outlet

The prototype feature phase is complete. The next workstream is hardware integration.

## Wiring reference

The frozen prototype wiring is documented separately in `air_quality_monitor_v1_8_wiring_reference.md`.

Core firmware pin contract:

```text
I2C SDA GPIO 8 | I2C SCL GPIO 9
TFT CS 10 | MOSI 11 | SCK 12 | MISO 13 | DC 14 | RST 15
```
