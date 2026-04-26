# Robimon

Firmware for the Waveshare **ESP32-S3-Touch-AMOLED-1.75** (Amazon B0F99V6FGF, Waveshare SKU 31262), built as a kid-friendly companion device with an animated face, voice interaction via a local Ollama box, Home Assistant info screens, and alarms. Adults configure it once via a PIN-locked settings menu.

> **Status:** under active development. Stage A of the HAL is in (display + touch + stats). See [CHANGELOG.md](CHANGELOG.md).

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 — ESP32-S3R8 (8 MB OPI PSRAM, 16 MB QIO flash), 466×466 round AMOLED via QSPI (CO5300), capacitive touch (CST9217), dual digital mic array + ES8311 codec + ES7210 echo cancel + NS4150B amp, QMI8658 6-axis IMU, AXP2101 PMIC, PCF85063 RTC, TCA9554 I/O expander, microSD slot, onboard LC76G GNSS, MX1.25 connectors for an 8 Ω 2 W speaker and a 3.7 V Li-ion battery.
- **Pin map:** see [docs/pinmap.md](docs/pinmap.md). The ESP32-S3R8 was confirmed against the official `pin_config.h` from Waveshare and the schematic photos.
- **Architecture:** see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building

You'll need [PlatformIO](https://platformio.org/install) (CLI or the VS Code extension).

```bash
pio run -e robimon                    # build
pio run -e robimon -t upload          # build + flash over USB-C
pio device monitor                    # USB-CDC serial @ 115200
```

The first build pulls Arduino-ESP32 3.3.5, LVGL 8.4, Arduino_GFX, lewis-he/SensorLib, and lewis-he/XPowersLib. Allow a few minutes.

## Project layout

```
docs/                     pinmap, architecture, performance/power notes, HA + voice setup
src/
  app/                    main entry, logging, stats, event bus
  hal/                    display, touch, audio, IMU, PMIC, I/O expander
  ui/                     screen manager, gestures, theme
  face/                   vector face renderer + expression library + tweener
  screens/                face_screen, ha_screen_*, alarm_screen, settings_screen
  services/               wifi_mgr, ha_client, ollama_client, alarm_mgr, config_store
assets/                   face primitives, fonts, icons (no copyrighted art)
companion/                Python STT/TTS reference server (runs on the Ollama box)
```

The `src/hal/` boundary is the only place that talks to actual hardware — everything above it consumes the board through HAL interfaces. Swapping framework or board variant should require touching `hal/` only.

## First-run setup (once flashed)

On first boot the device runs a guided setup: WiFi → time/timezone → set PIN → optional Home Assistant → optional Ollama/voice. After that it boots straight into the face screen. Settings is reached by long-pressing anywhere for 1.5 s and entering the PIN (default `0000` until changed).

## Companion server (for voice)

Voice capture, STT, Ollama, and TTS are all served by a small Python script that you run on your local Ollama box. See [docs/VOICE_SETUP.md](docs/VOICE_SETUP.md) (created later in development).

## Troubleshooting

The device never shows raw errors on the kid-facing screens. To see what's actually happening, open Settings → Diagnostics → Logs (last 50 entries, ring buffer), or attach USB-C and run `pio device monitor`.

## License

TBD.
