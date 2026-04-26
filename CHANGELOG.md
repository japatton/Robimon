# Changelog

All notable changes to Robimon firmware are recorded here. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are SemVer with a stage suffix until 1.0.

## [Unreleased]

## [0.2.0-hal-B] — 2026-04-26

### Added
- HAL stage B brings up the rest of the on-board peripherals:
  - **TCA9554PWR I/O expander** at I2C `0x20`. Configures SYS_OUT (P4) and GPS_RST (P7) as outputs, the rest as inputs. Lets the firmware service AXP2101/RTC/IMU interrupts and initiate a controlled power-down via SYS_OUT.
  - **AXP2101 PMIC** at I2C `0x34` via XPowersLib. Reports battery percentage, voltages (battery + VBUS), die temp, charge state. Configures 500 mA charge current cap. Surfaces PWRKEY short-press IRQs.
  - **QMI8658 IMU** at I2C `0x6B` via SensorLib. ±4 g accel + ±256 dps gyro at ~1 kHz. Address probe falls back to `0x6A` if `0x6B` doesn't ack.
  - **Audio HAL**: ES8311 codec init (vendored from Waveshare) + I2S stereo @ 16 kHz + NS4150B amp gate on GPIO 46. Mono `play()` / `capture()` APIs; `play_test_tone()` for bring-up.
- Vendored Waveshare's ES8311 driver into `src/hal/codec/`.
- Boot test tone behind `-DROBIMON_BOOT_TEST_TONE=1` build flag.

### Changed
- `touch::begin()` now passes `-1, -1` for SDA/SCL pins to SensorLib, eliminating the `Wire.cpp setPins(): bus already initialized` warning we saw in stage A.
- Stats reporter prints a second line with battery %, voltage, VBUS, charge state, and accel-magnitude (`|a|`).
- Bumped to `0.2.0-hal-B`.

### Notes
- With no battery connected, the AXP2101 reports `bat=255%` (0xFF sentinel) and `0.00 V` — that's correct, not a bug. Plug a battery into J1 to test percentage tracking.
- IMU `|a|` should sit at ~1.00 g at rest and jump higher when the board is moved.

## [0.1.0-hal-A] — 2026-04-26

### Added
- Project skeleton: PlatformIO build for Arduino-ESP32 3.x targeting the Waveshare ESP32-S3-Touch-AMOLED-1.75.
- Partition table with two 6 MB OTA slots, 3.5 MB SPIFFS, and a coredump partition.
- LVGL v8.4 wired up with PSRAM draw buffers and 16-bit RGB565 color.
- HAL stage A: CO5300 display driver (with TE-pin sync), CST9217 touch driver.
- Logging macros (`LOG_E/W/I/D`) with compile-time level switch via `ROBIMON_LOG_LEVEL`.
- Stats reporter (free heap, free PSRAM, FPS, uptime) over USB-CDC serial.
- Verified pin map (`docs/pinmap.md`) cross-referenced against schematic and silkscreen photos.
- Architecture sketch (`docs/ARCHITECTURE.md`) covering module boundaries, the event bus, and the alarm-audio extension point.

### Notes
- ESPHome and pure ESP-IDF were both considered and ruled out: see `docs/ARCHITECTURE.md` § Framework choice.
- The TCA9554 I/O expander on this board is wired to RTC/PMIC/IMU interrupts and a power-down output. Wrapper module is on the Stage B agenda.
