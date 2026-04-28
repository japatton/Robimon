// Inbound HTTP server for the proximity feature. Listens on
// PLAYBACK_SERVER_PORT for the companion app to push audio for one turn
// of a bot-to-bot conversation.
//
// Endpoints:
//   POST /play     — receives an audio body (WAV, mono 16-bit), enqueues
//                    playback, returns 202 immediately. When playback
//                    finishes, the playback worker fires
//                    CompanionAppClient::send_playback_complete.
//   POST /stop     — stops current playback + ends session. Returns 200.
//   GET  /status   — returns {"bot_id", "playing", "current_session"}.
//
// Headers expected on /play:
//   X-Session-Id    UUID-ish identifier the companion app picks for the
//                   current conversation.
//   X-Turn-Index    Integer, the 0-based turn number within the session.
//
// Two FreeRTOS tasks pinned to core 0:
//   - http task: WebServer.handleClient() loop
//   - play task: drains a semaphore + plays whatever the http task staged
//
// PSRAM buffers (~1.5 MB total) are allocated once at begin().

#pragma once

#include <stdint.h>

namespace robimon::services::playback_server {

bool begin();

// True while playback is in flight on this device.
bool is_playing();

}  // namespace robimon::services::playback_server
