# Local Air Quality Monitor — Final Wiring Reference

**Firmware baseline:** V1.8  
**Controller:** ESP32-S3  
**Purpose:** Keep this as the breadboard reference and the electrical starting point for PCB revision 1.

---

## 1. ESP32-S3 GPIO Map

### Shared I2C bus

| Function | ESP32-S3 GPIO |
|---|---:|
| SDA | GPIO 8 |
| SCL | GPIO 9 |

All four sensing devices share this I2C bus:

- SPS30
- SCD4X
- SHT45
- SGP41

### ILI9341 SPI TFT

| TFT signal | ESP32-S3 GPIO |
|---|---:|
| CS | GPIO 10 |
| MOSI | GPIO 11 |
| SCK / CLK | GPIO 12 |
| MISO | GPIO 13 |
| DC / RS | GPIO 14 |
| RST / RESET | GPIO 15 |

The firmware uses the TFT in landscape orientation at 320 × 240.

---

## 2. I2C Addresses

| Device | I2C address | Role |
|---|---:|---|
| SHT45 | `0x44` | Authoritative temperature + humidity |
| SGP41 | `0x59` | VOC + NOx raw sensing / index algorithm |
| SCD4X | `0x62` | CO2 |
| SPS30 | `0x69` | Particulate matter + particle counts + TPS |

There are no address conflicts on the shared bus.

---

## 3. Power Nets

### +5V rail

Use for:

- **SPS30 VDD**
- ESP32-S3 board input / USB supply as appropriate for the development board
- TFT supply only if the exact TFT breakout is designed for 5 V input

The SPS30 requires approximately 5 V; do not power the SPS30 from 3.3 V.

### +3V3 rail

Use for:

- **7Semi SHT45 + SGP41 breakout**
- **SCD4X in the current breadboard design**
- I2C pull-up domain
- ESP32-S3 logic
- TFT logic signals

### GND

All components must share a common ground:

- ESP32-S3
- SPS30
- SCD4X
- SHT45 + SGP41
- ILI9341 TFT

---

## 4. SPS30 — ZHR-5 Connector

The SPS30 must be placed in I2C mode.

| SPS30 pin | Signal | Connect to |
|---:|---|---|
| 1 | VDD | +5V |
| 2 | SDA | ESP32 GPIO 8 |
| 3 | SCL | ESP32 GPIO 9 |
| 4 | SEL | GND |
| 5 | GND | Common GND |

**Important:** `SEL` must already be tied to GND when the SPS30 powers up, otherwise it selects UART mode.

I2C address: `0x69`.

---

## 5. SCD4X

Current breadboard wiring:

| SCD4X | Connect to |
|---|---|
| VDD / VIN | +3V3 |
| GND | Common GND |
| SDA | ESP32 GPIO 8 |
| SCL | ESP32 GPIO 9 |

I2C address: `0x62`.

The sensor itself supports a wider supply range, but the current working monitor intentionally keeps it on the 3.3 V domain so it shares the ESP32-safe I2C bus cleanly.

### PCB note

Budget the 3.3 V rail for the SCD4X's relatively high measurement peak current. Do not size the 3.3 V regulator only from its average current.

---

## 6. 7Semi SHT45 + SGP41 Breakout

This is one physical breakout containing both sensors.

| Breakout pin | Connect to |
|---|---|
| 3V3 / VCC | +3V3 |
| GND | Common GND |
| SDA | ESP32 GPIO 8 |
| SCL | ESP32 GPIO 9 |

Internal devices:

| Sensor | Address |
|---|---:|
| SHT45 | `0x44` |
| SGP41 | `0x59` |

The board is a **3.3 V breakout**.

### Functional ownership

- SHT45 → room temperature
- SHT45 → room humidity
- SHT45 → T/RH compensation supplied to SGP41
- SGP41 → VOC Index
- SGP41 → NOx Index

The SCD4X temperature/humidity readings remain diagnostic only.

---

## 7. ILI9341 TFT

### Signal wiring

| TFT pin | ESP32-S3 |
|---|---:|
| CS | GPIO 10 |
| MOSI | GPIO 11 |
| SCK / CLK | GPIO 12 |
| MISO | GPIO 13 |
| DC / RS | GPIO 14 |
| RST | GPIO 15 |
| GND | Common GND |

### VCC / backlight

The firmware does **not** define TFT power or backlight voltage.

Keep the current known-good breadboard connection for the existing module. Before designing the PCB, identify the exact TFT breakout/module and verify:

- VCC input voltage
- whether an onboard regulator exists
- whether logic level shifting exists
- LED/backlight voltage and current
- whether the SD-card section, if present, shares SPI

ESP32-S3 GPIO signals themselves are 3.3 V logic.

---

## 8. Shared I2C Topology

```text
                         +---------------- SPS30 0x69
                         |                  VDD = 5V
                         |
ESP32-S3 GPIO 8  SDA ----+---------------- SCD4X 0x62
                         |                  VDD = 3.3V
                         |
                         +---------------- SHT45 0x44
                         |                                           |                   7Semi breakout = 3.3V
                         +---------------- SGP41 0x59

ESP32-S3 GPIO 9  SCL ----+---------------- same four devices

GND ---------------------+---------------- all devices
```

---

## 9. Complete Signal Summary

```text
ESP32-S3
│
├── GPIO 8  ── I2C SDA
│              ├── SPS30 SDA
│              ├── SCD4X SDA
│              ├── SHT45 SDA
│              └── SGP41 SDA
│
├── GPIO 9  ── I2C SCL
│              ├── SPS30 SCL
│              ├── SCD4X SCL
│              ├── SHT45 SCL
│              └── SGP41 SCL
│
├── GPIO 10 ── TFT CS
├── GPIO 11 ── TFT MOSI
├── GPIO 12 ── TFT SCK
├── GPIO 13 ── TFT MISO
├── GPIO 14 ── TFT DC
└── GPIO 15 ── TFT RESET
```

---

## 10. PCB Revision 1 Rules

Keep PCB rev 1 electrically equivalent to the working breadboard.

### I2C

- Pull SDA and SCL up to **3.3 V**, not 5 V.
- Do not blindly add pull-ups on every module and PCB branch.
- Check the effective parallel pull-up resistance because breakout boards may already include pull-ups.
- Keep SDA/SCL reasonably short and route them together.
- Add SDA, SCL, 3V3 and GND test points.

### Power

- Provide a solid 5 V rail for SPS30.
- Provide a well-regulated 3.3 V rail for ESP32 logic, SCD4X and SHT45/SGP41.
- Account for SCD4X peak current on 3.3 V.
- Add local decoupling close to each connector/device.
- Add bulk capacitance near the SCD4X / sensor power region.
- Keep a common low-impedance ground.

### Sensor placement

- Keep SHT45 away from:
  - ESP32
  - voltage regulators
  - TFT backlight circuitry
  - other heat-producing components
- Give the SHT45 direct access to room air.
- Do not obstruct SPS30 inlet or exhaust.
- Avoid routing hot power components directly beside the environmental sensor.

### SPS30

- Hard-wire `SEL` to GND for I2C mode.
- Supply from 5 V.
- Consider a keyed connector matching the SPS30 cable.
- Add test points for 5V, GND, SDA and SCL.

### TFT

- Preserve GPIO 10–15 assignments for initial PCB bring-up.
- Finalize the TFT power/backlight circuit only after identifying the exact display module being mounted.

---

## 11. Frozen Firmware-to-Hardware Contract

```text
I2C SDA = GPIO 8
I2C SCL = GPIO 9

TFT CS   = GPIO 10
TFT MOSI = GPIO 11
TFT SCK  = GPIO 12
TFT MISO = GPIO 13
TFT DC   = GPIO 14
TFT RST  = GPIO 15

SHT45 = 0x44
SGP41 = 0x59
SCD4X = 0x62
SPS30 = 0x69

SPS30 supply = 5V
SCD4X supply = 3.3V in this design
SHT45+SGP41 breakout = 3.3V
I2C logic/pull-ups = 3.3V
```

This mapping should remain unchanged during the first PCB bring-up unless a hardware conflict forces a deliberate firmware revision.
