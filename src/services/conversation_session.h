// Bot-side view of an in-flight bot-to-bot conversation. Minimal — the
// companion app owns the real state machine; this is just enough state for
// safety nets (idle timeout if the companion goes silent, max-turn cap if
// it forgets to end the session).

#pragma once

#include <stdint.h>

namespace robimon::services::conversation_session {

void begin();

// Called by PlaybackServer when a /play request arrives. Returns false if
// the session_id changed or the turn cap is exceeded — caller should reject
// the play in that case.
bool note_play_received(const char* session_id, uint8_t turn_index);

// Called when playback finishes successfully. Updates last_activity so the
// idle timeout starts counting from now.
void note_playback_complete();

// Called by main loop. If we haven't heard from the companion app for
// SESSION_IDLE_TIMEOUT_SECONDS after the last play_complete, reset to idle.
void update(uint32_t now_ms);

// Returns true when a session is in flight (one or more /play received,
// not yet timed out or ended).
bool is_active();

// Forces session reset — used by /stop endpoint and proximity_lost.
void end_session(const char* reason);

const char* current_session_id();
uint8_t     current_turn_index();

}  // namespace robimon::services::conversation_session
