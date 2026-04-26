# Changelog

All notable changes to Robimon firmware are recorded here. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are SemVer with a stage suffix until 1.0.

## [Unreleased]

## [0.10.0-ha-H] — 2026-04-26

### Added
- `services/ha_client` — Home Assistant WebSocket client. Connects to
  `ws://host:port/api/websocket` (port defaults to 8123, path defaults
  to `/api/websocket`), authenticates with a long-lived access token,
  subscribes to `state_changed` events, and caches state for tracked
  entities. Auto-reconnects with 5 s backoff.
- `screens/ha_settings_screen` — modal form with three field rows
  (URL / token / entity) + forget. Each row pushes the text-input
  modal so values can be entered with the on-screen keyboard.
- `screens/ha_entity_screen` — carousel screen showing the configured
  default entity (friendly name + state + age). "online" / "offline"
  status header per spec (kid-safe; no IPs / stack traces). Empty
  state when no entity is configured.
- `services/serial_console` — line-buffered console over USB-CDC.
  Commands: `help`, `set <key> <value>`, `get <key>` (`ha_token` and
  `wifi_pass` masked when printed), `forget <key>`, `list`,
  `ha-reconnect`, `status`. Lets the user paste long values (HA token,
  URLs, system prompt) without typing them on the on-screen keyboard.
- Carousel now: face → alarms → ha-entity (3 screens, cyclic).
- "ha" tile in settings menu pushes the HA settings screen.

### Notes
- `get_states` (full HA state dump on auth) is intentionally NOT sent —
  it's 100+ KB on a real install and exceeds the WebSocket library's
  default 16 KB frame buffer, causing the server to drop the connection.
  Cache populates from incremental `state_changed` events instead.
- ArduinoJson nesting limit bumped to 20 (default 10) — some HA
  state-changed events nest deeper for entities with rich attributes
  (device_info / connections arrays).
- HTTPS / TLS not yet supported. Internal-LAN only.
- MQTT fallback (per the spec) is deferred — HA WebSocket reliably
  works on the same LAN, which is the only setup the user has today.

### Changed
- platformio.ini: `links2004/WebSockets ^2.6.1` and
  `bblanchon/ArduinoJson ^7.4.1` added.

## [0.9.0-alarms-G] — 2026-04-26

### Added
- `services/alarm_mgr` — up to 8 alarms, persisted as a binary blob in
  NVS. Each alarm: enabled, hour, minute, days_mask (Sun=bit0..Sat=bit6,
  0 means every day), expression index, label. update() checks the
  wall clock once per second; fires once per (yday, minute) so no
  double-fires within the same minute. Snooze defers re-fire by N
  minutes; auto-dismiss after 5 min per spec.
- `screens/alarms_settings_screen` — list view with `+ add` and tap-to-
  edit row entries.
- `screens/alarm_edit_screen` — time picker (− / + for hour and
  minute), 7-day toggle row, expression cycler (< / >), enabled
  toggle, save / delete / cancel.
- Face module gains a `set_label`/`clear_label` overlay rendered below
  the mouth — used by the alarm-firing UI to show the alarm's label.
- `audio::start_alarm` / `audio::stop_alarm` — repeating two-tone
  chime (600 Hz → 880 Hz, ~800 ms cadence) on a background task pinned
  to core 0 so the UI on core 1 stays responsive. Loops until dismiss
  or auto-dismiss; gracefully unmutes the NS4150B amp before tones and
  re-mutes on stop to avoid pops.

### Changed
- `alarms` tile in settings menu now pushes the alarms list (previously
  stub-only).
- main.cpp watches alarm_mgr firing transitions: on rising edge it
  sets the configured face expression, sets the label overlay, and
  starts the chime; on falling edge it stops the chime, clears the
  label, and tweens back to neutral. Tap dismisses, swipe snoozes
  (9 min) — these intercept the normal carousel/menu routing only
  while an alarm is firing.
- Default ES8311 boot volume bumped 30 → 55 % (still under the 70 %
  spec cap) so alarms and the test tone are audible without being
  startling.

## [0.8.0-time-F3] — 2026-04-26

### Added
- `services/time_svc` — SNTP wrapper. Starts after WiFi connects, syncs
  against pool.ntp.org / time.google.com / time.cloudflare.com.
  Timezone is a POSIX TZ string saved in NVS; defaults to Central
  (`CST6CDT,M3.2.0,M11.1.0`) for Swansea, IL. 8 US-zone presets
  exposed via `TZ_PRESETS` for the settings UI.
- `screens/time_settings_screen` — live HH:MM:SS clock + sync status
  + current TZ name + tap-to-pick list of TZ presets.
- `screens/display_settings_screen` — 8-segment brightness slider
  (applies live, persists), idle-dim and sleep-timeout pickers (saved
  but not yet enforced — needs idle manager).
- `screens/about_screen` — version, board name, free heap/PSRAM, MAC,
  uptime, and a stub "OTA (soon)" button.
- Boot now applies the saved brightness preference.

### Changed
- Settings menu wires the `wifi`, `time`, `display`, and `about`
  tiles to push their respective modals; `ha`, `voice`, `alarms`,
  and `PIN` tiles still log "stub" and stay no-op until F-4 / G.

## [0.7.0-wifi-F2] — 2026-04-26

### Added
- `services/wifi_mgr` — WiFi state machine with async scan, manual
  connect, and credential persistence into config_store NVS. Auto-
  connect on boot when saved creds exist; silent no-op otherwise.
- `screens/wifi_setup_screen` — modal wifi UI with current connection
  status, scan button, scrollable network list (SSID, "L" lock
  indicator for secured nets, RSSI), and a forget button when
  connected. Round-display-safe layout (~340 px content width,
  centered, all elements within the inscribed circle).
- `screens/text_input_screen` — generic text-entry modal with QWERTY
  on-screen keyboard. Supports lower/UPPER toggle (auto-deshift after
  one shifted letter), 123/symbol page, masked input + show/hide
  toggle, configurable max length, and a callback that fires on done
  or cancel. Reused by HA URL/token, Ollama URL, system prompt
  editor, etc. in later stages.
- "wifi" tile in the settings menu now pushes the wifi setup screen.

### Changed
- Keyboard keys bumped from 40×32 to 40×38 for less mistapping.
- WiFi screen layout reworked to be round-display-aware after the
  initial release rendered the back button into the bezel-corner
  region.

## [0.6.0-settings-F1] — 2026-04-26

### Added
- `services/config_store` — NVS Preferences wrapper. Stores PIN as a
  HMAC-SHA256(salt, pin) hash with a per-device random salt; the PIN
  itself is never persisted. On a fresh device, `pin_check()` returns
  true for the default `0000` so the first-run flow can let an adult in.
- Modal stack on top of `ui/screen_mgr`. Settings, PIN, and future
  sub-screens push/pop modals; while a modal is on the stack, swipes
  are ignored (no carousel nav under a modal) and on_tap goes to the
  topmost modal.
- `screens/pin_pad_screen` — modal 4-digit PIN entry. 3×4 number grid
  with backspace and cancel; max 3 wrong tries before auto-dismiss.
  PIN-correct pops itself and pushes the settings menu.
- `screens/settings_menu_screen` — top-level settings menu with eight
  section tiles (`wifi`, `ha`, `voice`, `alarms`, `display`, `time`,
  `PIN`, `about`) and a `back` button. Sections are stubs for now;
  they get wired up in F-2..F-4.
- Long-press on the face/alarms screens now opens the PIN pad as a
  modal instead of just flashing "settings".

### Changed
- Gesture detector restructured to a 3-state machine (IDLE / ACTIVE /
  RELEASING). Brief touch-controller dropouts (<40 ms) are debounced as
  flutter rather than treated as a release, so a long swipe through
  several flutters now accumulates motion correctly instead of getting
  fragmented into many sub-threshold non-events.
- TAP and LONG_PRESS get the same 200 ms post-event quiet window the
  swipe already had. Fixes the "press one digit, three register" bug
  caused by the controller flashing n=0 mid-press.

### Notes
- `[E][TouchPoints.cpp:68] getPoint(): Invalid touch point index: 0`
  in the serial log is a SensorLib internal bug —
  `TouchDrvCST92xx::getTouchPoints()` calls `getPoint(0).event`
  unconditionally to read the event byte, even when zero points were
  reported. The driver clears its state on the error so it's
  cosmetic; fixing requires a SensorLib patch (or a wrapper that
  swallows the log).

## [0.5.0-screens-E] — 2026-04-26

### Added
- `ui/screen_mgr` — minimal screen manager. Holds an ordered list of
  swipeable screens, tracks the current index, routes per-frame `update`
  and `on_tap` to it. Snap transitions for now (no slide animation —
  saved for later if it bothers).
- `ui::Screen` virtual interface (update / on_tap / on_appear /
  on_disappear / name).
- `screens/face_screen` — thin wrapper that hosts the face module.
- `screens/alarms_screen` — placeholder alarms list. Renders a header,
  empty-state text, and a live "Ns on this screen" heartbeat counter at
  10 FPS so the screen doesn't *feel* hung when there's nothing to do.
  Real alarm rendering lands when the alarm manager ships in stage G.

### Changed
- Gestures classify on **release** instead of mid-hold. Slow swipes
  (where the user pauses before moving) used to get preempted by
  long-press; they now reliably register as swipes. Long-press fires
  when the user lifts after holding ≥1.2 s.
- 250 ms quiet window after any swipe fires. The CST92xx touch
  controller occasionally reports n=0 mid-drag, which used to cause the
  remainder of the swipe to land as a tap on the destination screen
  (which then opened the radial menu unintentionally on the face).

## [0.4.0-touch-D] — 2026-04-26

### Added
- `ui/gestures` module — state-machine gesture detector that consumes raw
  touch points and emits TAP / LONG_PRESS / SWIPE_LEFT / SWIPE_RIGHT
  (DOUBLE_TAP reserved for the voice mode in stage F). Tunable thresholds:
  long-press 1.2 s with 50 px motion tolerance, tap < 350 ms with 40 px
  drift, swipe ≥ 50 px horizontal motion. Generous on motion because
  fingertips on a round AMOLED naturally drift a few pixels.
- Radial expression menu: tap the face to open 8 cyan-ringed buttons
  (happy / yay / wow / huh / angry / sad / sleep / think) arranged
  clockwise from 12 o'clock. Tap an item to apply, tap outside to dismiss,
  5 s timeout for auto-dismiss.
- Brief on-screen text flashes confirm long-press (`settings`) and swipes
  (`<` / `>`) until the screen-manager and settings UI ship in stage E.

### Changed
- Demo cycle off by default in `main.cpp`; touch now drives expressions.
  Demo stays available via `face::enable_demo_cycle(true)`.

## [0.3.0-face-C] — 2026-04-26

### Added
- `display` HAL now owns an `Arduino_Canvas` back-buffer (~298 KB in PSRAM)
  and exposes `flush()` to push to the panel in one QSPI burst. Drawing
  primitives never touch the panel directly anymore — kills the
  mid-scanout tearing that the per-element direct-draw path produced.
- QSPI clocked at 80 MHz (default in the GFX library is 40 MHz); doubled
  effective flush bandwidth.
- LCD TE pin (GPIO 13) configured as input, ready for vsync-gated flushes
  once we send the CO5300 `TEON` (0x35) command — not wired yet.
- Pixelated face renderer (`src/face/face.cpp`):
  - Two large rounded-square ("octagon") cyan eyes with a per-eye lid
    coverage model (`top_lid_outer`, `top_lid_inner`, `bot_lid_outer`,
    `bot_lid_inner`) that supports asymmetric expressions like THINKING
    and CONFUSED.
  - Pixelated mouth with a few hand-drawn bitmaps (smile / frown / dash /
    `O` / hmm / grimace / speak).
  - Idle behaviors: random blink (2-6 s), subtle breathing (±2 % eye
    scale, 4 s period).
  - Tween between expressions (~350 ms) with cubic ease-out.
- Demo loop cycles through 9 expressions every 3.5 s (off by default in
  Stage D once touch picks them).

### Changed
- Eye color settled on `0x055F` (electric blue-cyan) — sci-fi desk-robot
  look rather than near-white cyan.
- Cell size 10 px with 1 px black gaps; 2-cell corner chamfer on each eye.
- Performance: render ≈ 10 ms, flush ≈ 31 ms, sustained ~22-24 FPS at
  466×320 PSRAM canvas.

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
