#include "alarm_mgr.h"
#include "config_store.h"
#include "time_svc.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace robimon::services::alarm_mgr {

namespace {
constexpr const char* TAG       = "alarm";
constexpr const char* KEY_BLOB  = "alarms_blob";

Alarm    s_alarms[MAX_ALARMS] = {0};
int      s_count              = 0;
// Last (yday, minute) at which each alarm fired, used to ensure we don't
// fire the same alarm multiple times within a single minute.
int16_t  s_last_yday[MAX_ALARMS]   = {-1, -1, -1, -1, -1, -1, -1, -1};
int16_t  s_last_minute[MAX_ALARMS] = {-1, -1, -1, -1, -1, -1, -1, -1};
// Snooze: ms timestamp at which the alarm should be allowed to re-fire.
uint32_t s_snooze_until_ms[MAX_ALARMS] = {0};

int      s_firing_idx        = -1;
uint32_t s_firing_started_ms = 0;
uint32_t s_last_check_ms     = 0;

void persist() {
  Preferences p;
  if (!p.begin("robimon", false)) return;
  p.putBytes(KEY_BLOB, s_alarms, sizeof(Alarm) * MAX_ALARMS);
  p.putUInt("alarms_n", (uint32_t)s_count);
  p.end();
}

void restore() {
  Preferences p;
  if (!p.begin("robimon", true)) return;
  if (p.isKey(KEY_BLOB)) {
    p.getBytes(KEY_BLOB, s_alarms, sizeof(Alarm) * MAX_ALARMS);
    s_count = (int)p.getUInt("alarms_n", 0);
    if (s_count < 0 || s_count > MAX_ALARMS) s_count = 0;
  }
  p.end();
}

bool day_matches(uint8_t mask, int wday_sun0) {
  if (mask == 0) return true;          // "every day"
  return (mask >> wday_sun0) & 0x01;
}

}  // namespace

void begin() {
  restore();
  LOG_I(TAG, "alarms loaded: count=%d", s_count);
}

void update(uint32_t now_ms) {
  // Check no more than once per second; the wall clock only moves in seconds.
  if ((now_ms - s_last_check_ms) < 1000) {
    // Auto-dismiss timer for the firing alarm — do this at the higher rate.
    if (s_firing_idx >= 0 && (now_ms - s_firing_started_ms) > AUTO_DISMISS_MS) {
      LOG_I(TAG, "auto-dismiss alarm %d", s_firing_idx);
      dismiss_current();
    }
    return;
  }
  s_last_check_ms = now_ms;

  if (s_firing_idx >= 0 && (now_ms - s_firing_started_ms) > AUTO_DISMISS_MS) {
    LOG_I(TAG, "auto-dismiss alarm %d", s_firing_idx);
    dismiss_current();
  }

  // Get current local time. If clock isn't synced yet, alarms can't fire.
  struct tm lt;
  if (!time_svc::get_local_time(&lt)) return;

  for (int i = 0; i < s_count; ++i) {
    const Alarm& a = s_alarms[i];
    if (!a.enabled) continue;

    // Snooze gate
    if (s_snooze_until_ms[i] > now_ms) continue;

    // Day match
    if (!day_matches(a.days_mask, lt.tm_wday)) continue;

    // Time match — but only fire once per (yday, minute).
    if (lt.tm_hour != a.hour || lt.tm_min != a.minute) continue;
    if (s_last_yday[i] == lt.tm_yday && s_last_minute[i] == lt.tm_min) continue;

    // Fire it.
    s_last_yday[i]   = (int16_t)lt.tm_yday;
    s_last_minute[i] = (int16_t)lt.tm_min;
    if (s_firing_idx < 0) {
      s_firing_idx = i;
      s_firing_started_ms = now_ms;
      LOG_I(TAG, "FIRE alarm %d: %02d:%02d '%s' (expr %u)",
            i, a.hour, a.minute, a.label, (unsigned)a.expression);
    }
  }
}

int          count()        { return s_count; }
const Alarm* get(int idx)   { return (idx >= 0 && idx < s_count) ? &s_alarms[idx] : nullptr; }

bool set(int idx, const Alarm& a) {
  if (idx < 0 || idx >= s_count) return false;
  s_alarms[idx] = a;
  s_alarms[idx].label[MAX_LABEL_LEN] = '\0';
  // Editing an alarm clears its "fired this minute" marker so a freshly
  // set alarm at the current minute can fire on the next check.
  s_last_yday[idx] = -1;
  s_last_minute[idx] = -1;
  s_snooze_until_ms[idx] = 0;
  persist();
  return true;
}

bool add(const Alarm& a, int* out_idx) {
  if (s_count >= MAX_ALARMS) return false;
  s_alarms[s_count] = a;
  s_alarms[s_count].label[MAX_LABEL_LEN] = '\0';
  if (out_idx) *out_idx = s_count;
  s_count++;
  persist();
  return true;
}

bool remove(int idx) {
  if (idx < 0 || idx >= s_count) return false;
  for (int i = idx; i < s_count - 1; ++i) {
    s_alarms[i]          = s_alarms[i + 1];
    s_last_yday[i]       = s_last_yday[i + 1];
    s_last_minute[i]     = s_last_minute[i + 1];
    s_snooze_until_ms[i] = s_snooze_until_ms[i + 1];
  }
  s_count--;
  persist();
  return true;
}

bool          any_firing()           { return s_firing_idx >= 0; }
const Alarm*  current_firing()       { return any_firing() ? &s_alarms[s_firing_idx] : nullptr; }
int           current_firing_idx()   { return s_firing_idx; }

void dismiss_current() {
  if (s_firing_idx < 0) return;
  LOG_I(TAG, "dismiss alarm %d", s_firing_idx);
  s_firing_idx = -1;
}

void snooze_current(uint32_t snooze_minutes) {
  if (s_firing_idx < 0) return;
  s_snooze_until_ms[s_firing_idx] = millis() + snooze_minutes * 60UL * 1000UL;
  // Clear the "fired this minute" marker so the snooze can re-fire next
  // matching minute even if we haven't crossed a day boundary.
  s_last_yday[s_firing_idx]   = -1;
  s_last_minute[s_firing_idx] = -1;
  LOG_I(TAG, "snooze alarm %d for %u min", s_firing_idx, (unsigned)snooze_minutes);
  s_firing_idx = -1;
}

bool next_alarm(int* out_idx, struct tm* out_when_local) {
  // Cheap O(N · 7·24·60) brute-force scan would be silly; instead, find the
  // smallest delta from now to each alarm's next occurrence. Granularity is
  // minutes — not 0.001-second exact, but plenty for "next alarm" UI.
  struct tm now;
  if (!time_svc::get_local_time(&now)) return false;
  int best_delta = INT32_MAX;
  int best_idx   = -1;
  struct tm best_when{};
  for (int i = 0; i < s_count; ++i) {
    const Alarm& a = s_alarms[i];
    if (!a.enabled) continue;
    // Search up to 7 days ahead.
    for (int day_offset = 0; day_offset < 7; ++day_offset) {
      const int wday = (now.tm_wday + day_offset) % 7;
      if (!day_matches(a.days_mask, wday)) continue;
      const int delta_minutes = day_offset * 24 * 60
        + (a.hour * 60 + a.minute) - (now.tm_hour * 60 + now.tm_min);
      if (delta_minutes <= 0 && day_offset == 0) continue;   // already passed today
      if (delta_minutes < best_delta) {
        best_delta = delta_minutes;
        best_idx   = i;
        best_when  = now;
        best_when.tm_mday += day_offset;
        best_when.tm_hour = a.hour;
        best_when.tm_min  = a.minute;
        best_when.tm_sec  = 0;
        mktime(&best_when);
        break;
      }
    }
  }
  if (best_idx < 0) return false;
  if (out_idx) *out_idx = best_idx;
  if (out_when_local) *out_when_local = best_when;
  return true;
}

void days_summary(uint8_t days_mask, char* out, size_t max) {
  if (max < 14) { if (max > 0) out[0] = '\0'; return; }
  if (days_mask == 0) {
    snprintf(out, max, "every day");
    return;
  }
  static const char letters[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };
  size_t pos = 0;
  for (int i = 0; i < 7 && pos + 2 < max; ++i) {
    if (days_mask & (1 << i)) {
      out[pos++] = letters[i];
      out[pos++] = ' ';
    }
  }
  if (pos > 0) pos--;   // trim trailing space
  out[pos] = '\0';
}

}  // namespace robimon::services::alarm_mgr
