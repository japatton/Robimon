# Changelog

All notable changes to Robimon firmware are recorded here. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are SemVer with a stage suffix until 1.0.

## [Unreleased]

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
