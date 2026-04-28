#include "conversation_session.h"
#include "proximity_config.h"
#include "../app/log.h"

#include <Arduino.h>
#include <string.h>

namespace robimon::services::conversation_session {

namespace {
constexpr const char* TAG = "convo";

constexpr size_t SESSION_ID_MAX = 40;   // matches X-Session-Id room

char     s_session_id[SESSION_ID_MAX + 1] = {0};
uint8_t  s_turn_index    = 0;
bool     s_active        = false;
uint32_t s_last_activity_ms = 0;

void reset(const char* reason) {
  if (s_active || s_session_id[0]) {
    LOG_I(TAG, "session %s ended (%s)", s_session_id, reason);
  }
  s_session_id[0] = '\0';
  s_turn_index    = 0;
  s_active        = false;
}
}  // namespace

void begin() {
  reset("init");
}

bool note_play_received(const char* session_id, uint8_t turn_index) {
  if (!session_id || !*session_id) return false;

  // New session — reset state and accept.
  if (!s_active || strncmp(s_session_id, session_id, SESSION_ID_MAX) != 0) {
    strncpy(s_session_id, session_id, SESSION_ID_MAX);
    s_session_id[SESSION_ID_MAX] = '\0';
    s_turn_index = turn_index;
    s_active     = true;
    s_last_activity_ms = millis();
    LOG_I(TAG, "session %s started @ turn %u", s_session_id, (unsigned)turn_index);
    return true;
  }

  // Same session — enforce the max-turn cap.
  if (turn_index >= proximity_config::SESSION_MAX_TURNS) {
    LOG_W(TAG, "session %s exceeded max turns (%u >= %u) — rejecting",
          s_session_id, (unsigned)turn_index,
          (unsigned)proximity_config::SESSION_MAX_TURNS);
    return false;
  }

  // Reject duplicate / out-of-order turn — defense in depth alongside
  // the server's (session_id, turn_index) dedup. Without this, a delayed
  // duplicate of an earlier turn would replay its audio.
  if (turn_index <= s_turn_index) {
    LOG_W(TAG, "session %s rejecting non-monotonic turn %u (have %u)",
          s_session_id, (unsigned)turn_index, (unsigned)s_turn_index);
    return false;
  }

  s_turn_index = turn_index;
  s_last_activity_ms = millis();
  return true;
}

void note_playback_complete() {
  s_last_activity_ms = millis();
}

void update(uint32_t now_ms) {
  if (!s_active) return;
  const uint32_t idle_ms = now_ms - s_last_activity_ms;
  const uint32_t timeout_ms = proximity_config::SESSION_IDLE_TIMEOUT_SECONDS * 1000UL;
  if (idle_ms > timeout_ms) {
    reset("idle timeout");
  }
}

bool is_active() { return s_active; }

void end_session(const char* reason) {
  reset(reason ? reason : "explicit");
}

const char* current_session_id() { return s_session_id; }
uint8_t     current_turn_index() { return s_turn_index; }

}  // namespace robimon::services::conversation_session
