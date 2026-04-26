# Pin Map — Waveshare ESP32-S3-Touch-AMOLED-1.75 (standard, non-C variant)

**Target board:** Amazon B0F99V6FGF / Waveshare SKU 31262 / ESP32-S3R8 (8 MB PSRAM, 16 MB Flash).
**Sources:** the official Waveshare engineering-sample repo `pin_config.h` + the board schematic and PCB silkscreen photos provided by the user 2026-04-26. The schematic is authoritative for anything not in `pin_config.h`.

> **Update note (2026-04-26):** an earlier draft of this file said "no I/O expander on the plain 1.75." That was wrong — the schematic confirms a **TCA9554PWR** at U5 carrying interrupts and a few control signals from the PMIC, RTC, IMU, GPS, and the test-pad row. It is on the shared I2C bus.

---

## ESP32-S3 GPIO map

| GPIO | Function | Notes |
| ---: | --- | --- |
| 0 | **BOOT button** (Key1) | direct GPIO; doubles as user button at runtime |
| 1 | SD MOSI / CMD | |
| 2 | SD SCK / CLK | |
| 3 | SD MISO / DATA0 | |
| 4 | LCD QSPI SIO0 (D0) | CO5300 |
| 5 | LCD QSPI SIO1 (D1) | |
| 6 | LCD QSPI SIO2 (D2) | |
| 7 | LCD QSPI SIO3 (D3) | |
| 8 | I2S DOUT — `ASDOUT` (ESP32 → ES8311) | speaker data path |
| 9 | I2S BCLK / SCLK | shared codec ↔ mic |
| 10 | I2S DIN — `DSDIN` (ES8311/ES7210 → ESP32) | mic data path |
| 11 | Touch INT (CST9217) | active-low |
| 12 | LCD CS (CO5300) | active-low |
| **13** | **LCD TE** (tearing-effect sync) | **NEW vs prior draft** — CO5300 vsync; use for tear-free flushes |
| 14 | I2C SCL (shared bus) | touch + RTC + PMIC + codec + IMU + expander + header |
| 15 | I2C SDA (shared bus) | |
| 16 | **Free GPIO** → 8-pin header (silkscreen `IO16`) | |
| 17 | GPS UART RXD (LC76G) **or** 8-pin header `IO17` | shared via 0 Ω option resistors R15/R16 — populated for GPS by default |
| 18 | GPS UART TXD (LC76G) **or** 8-pin header `IO18` | same caveat as GPIO 17 |
| 19 | USB D− (native USB OTG) | do not reuse |
| 20 | USB D+ (native USB OTG) | do not reuse |
| **21** | **QMI8658 INT2** (direct to ESP32) | **NEW vs prior draft** — fast wake-on-motion path; INT1 is via the expander |
| 38 | LCD QSPI SCLK | |
| 39 | LCD RESET | direct GPIO |
| 40 | Touch RESET (CST9217) | direct GPIO |
| 41 | SD CS (when used in 1-bit SPI mode) | |
| 42 | I2S MCLK | |
| 43 | U0TXD → 8-pin header `TXD` | shared with USB-CDC programming serial — using H2 TXD competes with serial monitor |
| 44 | U0RXD → 8-pin header `RXD` | same caveat |
| 45 | I2S WS / LRCK | |
| 46 | Speaker amp PA_CTRL (NS4150B enable) | drive HIGH to unmute |

**Free for software use under default population:** GPIO 16 only (on the H2 header). GPIO 17/18 become free if you depopulate R15/R16 and stop using the onboard GNSS.

**Strapping pins of note:** GPIO 0 (boot mode), 3 (used by SD), 45 (used by I2S WS), 46 (used by PA). All are loaded with their final purposes; don't expect to repurpose them.

---

## Shared I2C bus (GPIO 15 SDA / GPIO 14 SCL)

| Device | I2C address | Notes |
| --- | --- | --- |
| CST9217 capacitive touch | `0x5A` | INT on GPIO 11, RST on GPIO 40 |
| ES8311 codec (speaker DAC + mic preamp) | `0x18` | `ES8311_ADDRRES_0` |
| ES7210 4-channel mic ADC + AEC | `0x40` | feeds dual MIC1/MIC2 + 1 AEC loopback channel from speaker |
| AXP2101 PMIC | `0x34` | battery, charging, PWRKEY events; IRQ via expander EXIO5 |
| QMI8658 6-axis IMU | `0x6B` (silkscreen labels `0x6B` next to U3) | INT1 via expander EXIO6, INT2 direct on GPIO 21 |
| PCF85063 RTC | `0x51` | INT (alarm) via expander EXIO3, backup-powered through AXP2101 RTCLDO |
| TCA9554PWR I/O expander | `0x20` (default A0/A1/A2 = 0/0/0) | see expander map below |
| LC76G GNSS (optional, alternate path) | `0x50/0x54` | I2C alt mode; UART is the default; only reachable when the 0 Ω jumpers select I2C |

**Recommended bus speed:** 400 kHz. (The Waveshare GPS-via-I2C example runs 100 kHz, but the on-board parts all support 400 kHz fast-mode.)

---

## TCA9554PWR I/O expander map (U5)

The expander aggregates several interrupt and control lines so they don't burn ESP32 GPIOs. Read with a single I2C transaction; we'll poll on a slow timer (~50 Hz) and on PMIC IRQ events.

| Expander pin | Net | Direction | Used for |
| --- | --- | --- | --- |
| EXIO0 | test pad `EX0` (silkscreen `TP4`) | bidir | spare; exposed on bottom test row |
| EXIO1 | test pad `EX1` (`TP5`) | bidir | spare |
| EXIO2 | test pad `EX2` (`TP6`) | bidir | spare |
| EXIO3 | RTC INT (PCF85063 alarm) | input to ESP32 | wake/alarm signaling from RTC |
| EXIO4 | SYS_OUT → PWR-on gate | output from ESP32 | drives the BSS138 transistor that controls AXP2101 PWRON; pulling this lets firmware initiate a controlled power-down |
| EXIO5 | AXP_IRQ (AXP2101 IRQ) | input to ESP32 | PMIC events: button press, charge state change, voltage thresholds |
| EXIO6 | QMI INT1 (IMU INT1) | input to ESP32 | configurable IMU event (e.g., tap, sig-motion) |
| EXIO7 | GPS_RST (LC76G reset) | output from ESP32 | drive low to reset the GNSS module |

The TCA9554's own `INT` pin (which goes low when any input pin changes) — I cannot definitively read which ESP32 GPIO it lands on from the photo. Two reasonable possibilities: (a) not wired to ESP32 (we just poll), or (b) tied to one of the test pads. **Will assume "not wired, polled" until proven otherwise.** If we see latency on PMIC events that matters, we'll revisit.

---

## Connectors and silkscreen-confirmed pinouts

### 8-pin expansion header `H2` (top, edge of board)

Silkscreen labels: `IO18 | IO17 | IO16 | RXD | TXD | 3V3 | GND | VBUS`

| Pin | Net | ESP32 GPIO | Notes |
| ---: | --- | ---: | --- |
| 1 | `IO18` | GPIO 18 | shared with onboard GNSS UART (depopulate R16 to free) |
| 2 | `IO17` | GPIO 17 | shared with onboard GNSS UART (depopulate R15 to free) |
| 3 | `IO16` | GPIO 16 | always free |
| 4 | `RXD` | GPIO 44 (U0RXD) | shared with programming serial |
| 5 | `TXD` | GPIO 43 (U0TXD) | shared with programming serial |
| 6 | `3V3` | — | regulated 3.3 V out |
| 7 | `GND` | — | |
| 8 | `VBUS` | — | 5 V from USB (only present when USB plugged in) |

### Bottom test-pad row (`TP2`–`TP6`)

Silkscreen labels (left to right): `SDA | SCL | EX0 | EX1 | EX2`

| Pad | Net | Notes |
| --- | --- | --- |
| `SDA` (TP3) | GPIO 15 | direct tap on shared I2C bus |
| `SCL` (TP2) | GPIO 14 | direct tap on shared I2C bus |
| `EX0` (TP4) | TCA9554 EXIO0 | accessed via expander I2C reads/writes |
| `EX1` (TP5) | TCA9554 EXIO1 | |
| `EX2` (TP6) | TCA9554 EXIO2 | |

`TP11` (RST) and `TP1` (GND) are visible on the silkscreen for ESP32 hardware reset access.

### Power and battery connectors

- `H3` (top, MX1.25 2-pin labeled `SPK` `+`/`−`): 8 Ω 2 W speaker. Driven by NS4150B amp gated by GPIO 46.
- `J1` (top, MX1.25 2-pin labeled `BAT` `+`/`−`): 3.7 V Li-ion battery. AXP2101 charges this from USB while the device runs — meets the project's "charge while running" requirement out of the box.

### Buttons

- `Key1` silkscreen `BOOT` → GPIO 0 (with R7/R8 10 kΩ pull-up). Available as a software button at runtime.
- `Key2` silkscreen `PWR` → AXP2101 PWRON via T1 (BSS138 transistor) gated by SYS_OUT (TCA9554 EXIO4). Press events are reported by the AXP2101 over I2C; firmware can also force a power-down by driving EXIO4 low.

---

## What this means architecturally

A few things we now have for free that I didn't think we did:

- **Tear-free animation** is on the table — LCD TE is on GPIO 13, so we can lock display flushes to vsync and avoid the AMOLED tearing band on full-screen swipe transitions.
- **Wake-on-motion** is feasible without bodging — QMI8658 INT2 lands on GPIO 21 directly, so the IMU can wake us from light sleep.
- **Controlled power-down** is straightforward — driving TCA9554 EXIO4 low cuts AXP2101 power. Useful for "low battery, halt non-essential tasks."
- **Battery / charge state** are still I2C-only (AXP2101 internal ADC). No analog ADC pin.

A few things to keep in mind:

- **GPIO 16 is the only "always-free" GPIO on the H2 header** under default board population. If we want to expose more, we have to choose between GNSS and the header pins.
- **The TCA9554 polling cadence matters for PMIC IRQ latency.** 50 Hz polling is fine for charge-state changes; sub-100 ms button latency will need the IMU INT2 trick or a dedicated wire if we ever care.
- **Onboard GNSS (LC76G).** Not in our spec, but it's there and it's eating GPIO 17/18 by default. We should either ignore it or expose time/location to the kid as a future feature.

## Open items that still need eyes-on the board

1. **TCA9554 INT routing.** Cannot read from the photos. Polling-only is the safe assumption; if you see a trace on the back I missed, let me know.
2. **MCLK pin.** `pin_config.h` defines both `I2S_MCK_IO 16` (vestigial — GPIO 16 is on the header) and `MCLKPIN 42` (the one the working ES8311 example uses). I'm going with **GPIO 42** and treating the macro at 16 as a copy-paste leftover from the 1.8 board's `pin_config.h`.
3. **LCD TE polarity / use.** I want to set `disp_drv.full_refresh = 0` and gate flushes on the TE rising edge. Will validate experimentally on first bring-up.

## References

- Official engineering-sample repo: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75>
- Waveshare wiki landing page: <https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75>
- Schematic & silkscreen images supplied by the user 2026-04-26 (authoritative for everything not in `pin_config.h`).
