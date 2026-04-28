# Proximity-Triggered Bot-to-Bot Conversation

When two Robimon devices come into BLE proximity, they have a short
spoken conversation orchestrated by the companion server (the bots
themselves are dumb proximity sensors + audio playback endpoints — all
dialog generation and TTS happens on the server).

This document covers:
- The bot ↔ companion API contract
- How to pair two bots
- How to calibrate the RSSI thresholds
- The two-device end-to-end test plan
- Known TODOs

---

## Architecture (one paragraph)

Each bot runs a **NimBLE proximity monitor** (passive RSSI scan, 5-deep
moving average, hysteretic state machine with `OUT_OF_RANGE →
APPROACHING → IN_PROXIMITY → LEAVING → OUT_OF_RANGE`). When state
crosses `IN_PROXIMITY`, the bot fires `POST /api/proximity/detected`
to the companion server. Once both bots report proximity within a
30-second window, the orchestrator picks a script from `BOT_DIALOGS`,
generates audio for line 1 via Piper, and pushes it to the starter
bot's `POST /play`. When that bot finishes playback it acks via `POST
/api/playback/complete`, which prompts the orchestrator to generate
line 2 for the *other* bot, and so on. The session ends on script
exhaustion, on `proximity_lost` from either bot, on the bot-side
session idle timeout (30 s of no `/play` after a complete), or on the
bot-side max-turn cap (20).

---

## Bot ↔ Companion API contract

### Bot → companion (outbound, fire-and-forget)

All bodies are `application/json`. The bot includes `bot_id`, `ip`, and
`port` so the server can post audio back to it without out-of-band
discovery.

```
POST /api/proximity/detected
{
  "bot_id": "0F3C",          // last 4 hex of MAC
  "ip":     "192.168.1.7",   // bot's local IP (companion posts /play here)
  "port":   8000,            // PLAYBACK_SERVER_PORT on the bot
  "rssi":   -34,
  "timestamp_ms": 12345
}
```

```
POST /api/proximity/lost          // same body shape, no rssi
POST /api/playback/complete       // adds session_id, turn_index
POST /api/playback/error          // adds session_id, error (string)
```

The bot **never blocks the UI** waiting for a response. The HTTP
worker (companion_client.cpp) has a 2 s timeout, one retry, and silently
drops on failure.

### Companion → bot (inbound, one bot endpoint each)

```
POST  /play
  Headers:
    Content-Type:    audio/wav
    X-Session-Id:    sess-...        (chosen by companion, stable across the session)
    X-Turn-Index:    <int>           (0-based, monotonically increasing within a session)
    Content-Length:  <bytes>         (REQUIRED — the bot uses this to size the read)
  Body: WAV (mono 16-bit; 22050 Hz works, anything else is resampled to 16 kHz)
  Returns: 202 Accepted   (immediately; completion is signalled out-of-band)
           400 on bad headers / size
           409 on session-policy reject (max-turn cap, session-id changed)
           503 if a previous /play is still playing OR a higher-priority
               audio owner (alarm, voice) holds the audio path

POST  /stop                    Returns 200. Ends the session immediately;
                                current chunk-in-flight finishes.

GET   /status                  Returns {bot_id, playing, current_session}
```

### Companion-side diagnostic

```
GET /api/proximity/status      // returns the bot registry + active session
```

Useful when debugging — shows what bots the server has heard from, when, and
what the current session looks like.

---

## Pairing (one-time, per device)

The bots use BLE only for RSSI measurement — there is no GATT data
exchange and no formal BLE pairing/bonding. "Pairing" here just means
each bot remembers the other's MAC address in NVS so it knows which
peer's RSSI to track.

Steps via the serial console (115200 baud):

1. Open serial monitor on bot A. Type `bot-id`. It prints the BLE
   address (e.g. `98:88:E0:03:B3:55`).
2. Open serial monitor on bot B. Type `bot-id`. Note that MAC.
3. On bot A: `pair-peer 3C:0F:02:C0:28:35` (bot B's MAC).
4. On bot B: `pair-peer 98:88:E0:03:B3:55` (bot A's MAC).
5. Confirm with `proximity` on each — should report
   `state: OUT_OF_RANGE  smoothed_rssi: ...`.

Pairing persists in NVS across reboots. To unpair, run `unpair-peer`.

---

## RSSI calibration

The shipped defaults in `proximity_config.h` are starting points:

```
PROXIMITY_RSSI_THRESHOLD = -55   // trigger when smoothed >= this for 3 s
RELEASE_RSSI_THRESHOLD   = -70   // release when smoothed < this for 5 s
```

For a typical living room these correspond to roughly arm's reach (in)
and across-the-room (out). They will be different in your environment
(walls, body absorption, antenna orientation) — calibrate on real
hardware:

1. Pair both bots, watch one bot's serial output (`pio device monitor`).
2. Place the bots far apart (different rooms ideally). Note the
   smoothed RSSI from the `prox` log lines — this is your "out of
   range" floor. Pick `RELEASE_RSSI_THRESHOLD` 5–10 dBm above this.
3. Bring the bots close (a foot apart). Note the smoothed RSSI again —
   this is your "definitely in range" ceiling. Pick
   `PROXIMITY_RSSI_THRESHOLD` 5–10 dBm below this.
4. Maintain at least a 10–15 dBm hysteresis gap between the two so
   the state doesn't flap on bouncing samples.
5. Edit `proximity_config.h`, rebuild, reflash both bots.

You can also tweak the dwell windows (`PROXIMITY_DWELL_SECONDS=3`,
`PROXIMITY_RELEASE_SECONDS=5`) — shorter is more responsive, longer
filters more fly-by detections.

---

## Two-device end-to-end test plan

Pre-requisites:
- Both bots flashed with the proximity feature
- Both bots on the same Wi-Fi
- Companion server running with the `/api/proximity/*` and
  `/api/playback/*` endpoints (this branch's `companion/server.py`)
- Both bots paired with each other (per the pairing section above)
- Each bot's `voice_url` (NVS, set via serial `set voice_url <url>`)
  pointing at the companion — same value as the voice flow uses.
  *Note:* the proximity feature reads `companion_url` from NVS first,
  falling back to `DEFAULT_COMPANION_URL`. Set with
  `set companion_url http://192.168.1.50:8765`.

Test:

1. Power both bots, place 2+ meters apart. Verify each bot's serial
   reports `prox  state APPROACHING -> OUT_OF_RANGE` if it briefly
   saw the peer, then settles in `OUT_OF_RANGE`.
2. Tail the companion server log:
   `docker logs -f robimon-companion` (or wherever it runs).
3. Walk one bot toward the other. After ~3 s in close range, both
   bots should fire `prox  state APPROACHING -> IN_PROXIMITY` and
   the companion should log `proximity_detected` from each.
4. After the second `proximity_detected` arrives, the companion logs
   `starting session sess-... with bots [...]` and immediately POSTs
   the first turn's audio to one of the bots.
5. That bot speaks; you should hear it. When it finishes it POSTs
   `playback_complete` to the companion, which generates the next
   turn's audio and POSTs to the *other* bot. Continue until the
   script ends (5 turns by default).
6. Walk the bots apart. Within ~5 s both fire `proximity_lost`,
   companion ends the session.

**Top three things to look at if it doesn't work:**

1. **Companion server registry.** Hit
   `GET /api/proximity/status` from your laptop's browser. Both bots
   should appear with non-zero `last_seen_age_s` and `in_proximity:
   true` while close. If only one is listed, the other isn't reaching
   the server (wifi issue, wrong companion_url).
2. **Bot-to-bot RSSI calibration.** If state stays in `APPROACHING`
   and never reaches `IN_PROXIMITY`, the smoothed RSSI is hovering
   near the threshold without crossing. Bring the bots closer or
   bump `PROXIMITY_RSSI_THRESHOLD` down (more negative).
3. **Audio doesn't play but `play` server logs the request.** Check
   `audio busy` in the bot logs — voice or alarm has the path. If
   neither: check WAV format (must be mono 16-bit; verify with
   `file out.wav` on the companion).

---

## Known TODOs / limitations

- **No auth on `/play`.** Anyone on the LAN can POST audio to a bot.
  Acceptable for a closed home network; for shared/public networks,
  add a shared-secret header check (proposed: `X-Robimon-Auth: <key>`
  set in NVS, configurable via serial console).
- **No mid-playback preemption.** Audio arbitration is cooperative —
  if alarm fires while voice or proximity is mid-`play()`, alarm waits
  for the chunk to finish. Chunks are short (~tens of ms), so this is
  rarely audible, but a real preempt would cut over instantly.
- **WAV-only.** No MP3 decoder. If companion-bot wifi bandwidth becomes
  an issue, add libhelix-mp3 (~30 KB flash, +1 task).
- **Single-pair only.** The orchestrator handles one in-flight session
  at a time, and treats "any two in-proximity bots" as the pair.
  Multi-pair (e.g., two pairs of bots in different rooms) would need
  pair-aware session tracking.
- **No per-bot voice.** Both bots speak in the same Piper voice.
  Adding `PIPER_MODEL_A` / `PIPER_MODEL_B` env vars + per-bot routing
  is small but deliberately deferred.
- **Pairing UX is serial-console only.** No on-screen pairing flow.
  Acceptable for the developer/maker stage.
- **Hardware IMU wake-on-motion still polled.** ESP32-S3's QMI8658
  supports interrupt-driven wake, but we poll at 10 Hz instead. Move
  to interrupt when light/deep sleep lands.
