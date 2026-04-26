// Alarm manager. Holds up to MAX_ALARMS alarm definitions, persists them to
// NVS, checks every loop iteration whether the wall-clock minute has rolled
// over and any alarm should fire, and tracks the currently-firing alarm
// (with auto-dismiss after a configurable timeout).
//
// Visual-only by spec — no audio. The architecture doc notes the audio
// extension point: alarm_mgr publishes the firing event; an audio_pipeline
// consumer can subscribe later without touching the face system.

#pragma once

#include <stdint.h>
#include <time.h>

namespace robimon::services::alarm_mgr {

constexpr int    MAX_ALARMS       = 8;
constexpr size_t MAX_LABEL_LEN    = 23;     // + NUL = 24 bytes
constexpr uint32_t AUTO_DISMISS_MS = 5UL * 60UL * 1000UL;   // 5 min, per spec

struct Alarm {
  bool     enabled;
  uint8_t  hour;          // 0..23
  uint8_t  minute;        // 0..59
  uint8_t  days_mask;     // bit 0=Sun .. bit 6=Sat. 0 means "every day"
  uint8_t  expression;    // index into face::Expression enum
  char     label[MAX_LABEL_LEN + 1];
};

void begin();
void update(uint32_t now_ms);

int          count();
const Alarm* get(int idx);
bool         set(int idx, const Alarm& a);
bool         add(const Alarm& a, int* out_idx = nullptr);
bool         remove(int idx);

// Firing state
bool          any_firing();
const Alarm*  current_firing();
int           current_firing_idx();
void          dismiss_current();
void          snooze_current(uint32_t snooze_minutes);   // re-fires after N minutes

// For UI: when's the next alarm? Returns false if none enabled.
bool next_alarm(int* out_idx, struct tm* out_when_local);

// Helper: returns "S M T W T F S" with selected days highlighted as a
// human-readable string written to out (max 16 chars).
void days_summary(uint8_t days_mask, char* out, size_t max);

}  // namespace robimon::services::alarm_mgr
