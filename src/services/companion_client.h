// Outbound HTTP wrapper for the proximity feature.
//
// Each call enqueues a small request struct and returns immediately. A
// dedicated worker task on core 0 pops the queue, builds JSON, and POSTs
// to the companion app with a short timeout + one retry. Failures are
// logged at LOG_W and never block or surface to the kid-facing UI.

#pragma once

#include <stdint.h>

namespace robimon::services::companion_client {

void begin();

// Returns the bot's stable identifier — last 4 hex of the MAC, matching
// the BLE device-name scheme. Lifetime: forever (static buffer).
const char* bot_id();

// Outbound endpoints. All four are fire-and-forget — they enqueue a job
// and return true if the queue had space, false if full (in which case
// the call is silently dropped — favors UI responsiveness over delivery
// guarantees, since the companion app has its own retry on the next
// proximity event anyway).
bool send_proximity_detected(int8_t rssi);
bool send_proximity_lost();
bool send_playback_complete(const char* session_id, uint8_t turn_index);
bool send_playback_error   (const char* session_id, const char* err);

}  // namespace robimon::services::companion_client
