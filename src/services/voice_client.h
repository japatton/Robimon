// Voice client. Triggered by double-tap on the face screen.
//
// Flow (runs on a background task pinned to core 0 so the UI stays
// responsive on core 1):
//
//   1. RECORDING — capture 5 s of mono int16 PCM at 16 kHz
//   2. SENDING   — wrap PCM in a WAV header, POST to the companion's
//                  /chat endpoint, wait for the response WAV
//   3. PLAYING   — parse response WAV, resample to 16 kHz if needed,
//                  push to the speaker
//   4. IDLE
//
// Errors get a transient ERROR_* state and the face flashes "confused"
// briefly before going back to neutral. We never surface a stack trace
// or HTTP code to the user — diagnostics live in the serial log.

#pragma once

#include <stdint.h>

namespace robimon::services::voice {

enum class State : uint8_t {
  IDLE,
  RECORDING,
  SENDING,
  PLAYING,
  ERROR_NETWORK,
  ERROR_NO_SPEECH,
  ERROR_OTHER,
};

void begin();
void update(uint32_t now_ms);

State        state();
const char*  state_str();

// Trigger the full record → send → play sequence. Returns false if a
// previous flow is still running, or if the URL isn't configured.
bool start();

}  // namespace robimon::services::voice
