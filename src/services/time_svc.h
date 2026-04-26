// Time service. Wraps SNTP + POSIX timezone handling so the rest of the
// firmware can ask "what time is it (in the user's timezone)" without
// needing to know about lwip or tz strings.
//
// SNTP starts after WiFi connects. Sync usually completes within a few
// seconds. Until it does, time() returns a small post-1970 value; the
// is_synced() accessor tells you whether the clock is trustworthy.
//
// Timezone is a POSIX TZ string (e.g., "CST6CDT,M3.2.0,M11.1.0" for
// America/Chicago). We expose a small set of US-zone presets via the
// TZ_PRESETS table for the settings UI; advanced users can set any
// POSIX TZ via set_tz().

#pragma once

#include <stdint.h>
#include <time.h>

namespace robimon::services::time_svc {

struct TzPreset {
  const char* name;        // human-readable, shown in UI
  const char* posix_tz;    // POSIX TZ string
};

// Common US timezones plus UTC. Order matches what the user expects to see.
constexpr TzPreset TZ_PRESETS[] = {
  { "Eastern",  "EST5EDT,M3.2.0,M11.1.0"   },
  { "Central",  "CST6CDT,M3.2.0,M11.1.0"   },   // default
  { "Mountain", "MST7MDT,M3.2.0,M11.1.0"   },
  { "Arizona",  "MST7"                     },   // no DST
  { "Pacific",  "PST8PDT,M3.2.0,M11.1.0"   },
  { "Alaska",   "AKST9AKDT,M3.2.0,M11.1.0" },
  { "Hawaii",   "HST10"                    },
  { "UTC",      "UTC0"                     },
};
constexpr int N_TZ_PRESETS = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);

void begin();
void update(uint32_t now_ms);   // call from main loop; checks SNTP status

bool is_synced();
bool get_local_time(struct tm* out);  // local time per current TZ

const char* current_tz_name();  // matches a preset name, or "custom"
const char* current_tz_string();

bool set_tz_preset(int idx);    // 0..N_TZ_PRESETS-1
bool set_tz_string(const char* posix_tz);

}  // namespace robimon::services::time_svc
