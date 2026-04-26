# Architecture

The shape of the firmware. This document is the north star for "where does this code go?" decisions.

## Framework choice

Arduino + PlatformIO + LVGL.

- Arduino-ESP32 (3.x) is itself an ESP-IDF wrapper, so we keep access to FreeRTOS task pinning, `esp_lcd`, raw I2S DMA, PSRAM allocators, the task watchdog, and `esp_timer` whenever we need them.
- The Waveshare reference examples (CO5300 panel init, CST9217 touch quirks, ES8311 codec setup, AXP2101 PMIC) are all Arduino. Going pure ESP-IDF would mean porting drivers before drawing a pixel.
- LVGL v8 carries its own partial/dirty-rect rendering and timeline animator — both load-bearing for our 30+ FPS budget on a 466×466 AMOLED.
- ESP-ADF (the IDF-native audio pipeline) is the one tradeoff. Mitigated: our voice path is PCM-over-HTTP to a local STT/TTS companion, the on-board ES7210 chip handles AEC in hardware, VAD is a 30-line energy threshold, and lipsync is amplitude-envelope. We don't need ADF.

ESPHome was ruled out by the project brief — native HA integration isn't worth giving up animation control, audio pipeline flexibility, and the Ollama integration.

## Module map

```
app/        main entry, logging, runtime stats, central event bus
hal/        sole owner of hardware: display, touch, audio, IMU, PMIC, expander, SD
ui/         screen manager, gesture handler, theme constants
face/       vector face renderer, expression library, animation tweener
screens/    face_screen, ha_screen_*, alarm_screen, settings_screen
services/   wifi_mgr, ha_client, ollama_client, alarm_mgr, config_store, ota
assets/     packed binary face primitives, fonts, icons
```

The `hal/` boundary is the only place that pokes hardware. Everything above it consumes the board through HAL interfaces. Swapping framework or board variant should require touching `hal/` only.

## Event bus

A single `EventBus` (FreeRTOS queue under the hood) carries small, copyable event records between modules. The face renderer doesn't import `wifi_mgr` or `ollama_client` — it subscribes to events like `EXPRESSION_CHANGED`, `LISTENING_STARTED`, `SPEAKING_AMPLITUDE`, `ALARM_FIRING`, and reacts.

Producers and consumers (initial set):

| Producer | Event | Consumers |
| --- | --- | --- |
| `touch` HAL | `TOUCH_BEGIN`, `TOUCH_END`, `SWIPE_*`, `LONG_PRESS` | `ui/screen_mgr`, current screen |
| `alarm_mgr` | `ALARM_FIRING`, `ALARM_DISMISSED`, `ALARM_SNOOZED` | face renderer, screen manager |
| `ollama_client` | `LISTENING_STARTED`, `LISTENING_ENDED`, `THINKING`, `SPEAKING_STARTED`, `SPEAKING_AMPLITUDE`, `SPEAKING_ENDED` | face renderer |
| `ha_client` | `HA_ENTITY_UPDATE`, `HA_DISCONNECTED`, `HA_RECONNECTED` | HA screens |
| `power` HAL | `BATTERY_CHANGED`, `CHARGE_STATE`, `PWRKEY_PRESSED`, `LOW_BATTERY`, `CRITICAL_BATTERY` | screen manager (low-power UX), settings stats screen |
| `imu` HAL | `MOTION_WAKE` | screen manager (wake from idle) |

Events are small structs (`<= 32 bytes`) so the queue can be bounded in static memory.

## Threading model

Two cores. The split is hard, not advisory.

- **Core 0 (system):** WiFi, MQTT/HA-WebSocket client, Ollama HTTP client, NVS writes, OTA, alarm scheduler, log flush.
- **Core 1 (UI + audio):** LVGL task (16 ms tick), display flush, touch poll, IMU poll, audio capture/playback I2S DMA. The animation pipeline must never block waiting on the network.

The task watchdog is enabled with a 100 ms threshold on Core 1. Anything that takes longer must yield (`vTaskDelay(1)`) or be moved to Core 0. Periodic stats reporter prints any task that trips the threshold.

## Display pipeline

- Double-buffered LVGL draw buffers in PSRAM, sized 1/4 screen each (~108 KB) — leaves SRAM hot for I2S DMA and animation state.
- `disp_drv.full_refresh = 0`, `disp_drv.direct_mode = 0` — partial dirty-rect flush is the default. The face redraws only the eyes/mouth bounding boxes between frames, not the whole 466×466.
- The CO5300 has a 6-pixel column offset baked into its window registers; the panel constructor handles this. **Do not** pass it your own offset.
- TE pin (GPIO 13) is wired up. Phase 1 just polls; phase 2 will gate `lv_disp_flush_ready` on TE rising-edge to eliminate the tearing band on full-screen swipes.

## Power and idle

The AXP2101 PMIC charges from USB while the device runs, exposes battery + bus voltages over I2C, and reports button events. We poll its status on a 500 ms timer (Core 0).

Idle ladder (configurable, defaults shown):

1. **Active** — full FPS, full brightness.
2. **Dimmed idle** (after 60 s of no touch) — brightness ramps to 25 %, FPS unchanged.
3. **Sleepy face** (after 120 s) — face screen shows a slow blink loop, FPS drops to 5.
4. **Light sleep** (after 5 min) — display off, ESP32 in light sleep. Wake sources: touch INT (GPIO 11), IMU INT2 (GPIO 21).
5. **Critical battery** (< 10 %) — sleepy face + reduced rate.
6. **Halt** (< 5 %) — single "needs charging" message, all non-essential tasks suspended. Driving TCA9554 EXIO4 low forces an AXP2101 power-down if charge isn't restored.

## Alarm system extension point

Alarms are visual-only by spec (no audio on alarm). To preserve the option of audio alarms later without touching the face system, the alarm manager publishes `ALARM_FIRING` to the event bus with a `play_audio: bool` field that today is always `false`. Adding audio alarms is two changes:

1. Set `play_audio = true` in the alarm record.
2. Add an `audio_pipeline` consumer for `ALARM_FIRING` that plays a configurable WAV from SPIFFS.

The face renderer doesn't change. The screen manager doesn't change.

## Configuration

All persistent settings live in NVS under namespace `robimon`:

- WiFi credentials
- HA URL / token / MQTT broker config
- Ollama companion endpoint, model, system prompt
- Alarms (one CBOR blob)
- PIN hash (PBKDF2, not the PIN itself)
- Display brightness, idle timeouts
- Timezone
- Diagnostics: ring buffer of last 50 log entries (separate namespace, written through-only on errors)

Schema versioning: each NVS key is prefixed with `v1_` etc. Migration on boot if the schema version key changes.

## OTA

OTA image is fetched from a URL configured in settings. We use the standard Arduino `Update` library against a manifest JSON the user hosts (e.g., on the same companion box). Manifest format:

```json
{ "version": "0.2.0", "url": "http://...firmware.bin", "sha256": "..." }
```

We refuse to apply if `version <= ROBIMON_VERSION` or sha256 mismatches.
