#include "time_svc.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <string.h>
#include <time.h>

namespace robimon::services::time_svc {

namespace {
constexpr const char* TAG = "time";
constexpr const char* KEY_TZ = "tz_posix";
constexpr int DEFAULT_TZ_IDX = 1;  // "Central" — user is in Swansea, IL

char       s_tz[48] = {0};
bool       s_sntp_started = false;
bool       s_synced = false;
uint32_t   s_last_check_ms = 0;

void apply_tz(const char* tz) {
  setenv("TZ", tz, 1);
  tzset();
}

void start_sntp_if_needed() {
  if (s_sntp_started) return;
  if (wifi_mgr::state() != wifi_mgr::State::CONNECTED) return;

  // Standard pool servers; ESP-IDF SNTP picks the first that responds.
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  s_sntp_started = true;
  LOG_I(TAG, "SNTP started (TZ=%s)", s_tz);
}

}  // namespace

void begin() {
  // Restore saved TZ or apply default. We never persist the empty string.
  char saved[sizeof(s_tz)];
  if (config::get_string(KEY_TZ, saved, sizeof(saved)) > 0) {
    strncpy(s_tz, saved, sizeof(s_tz) - 1);
  } else {
    strncpy(s_tz, TZ_PRESETS[DEFAULT_TZ_IDX].posix_tz, sizeof(s_tz) - 1);
  }
  s_tz[sizeof(s_tz) - 1] = '\0';
  apply_tz(s_tz);
  LOG_I(TAG, "tz=%s", s_tz);
}

void update(uint32_t now_ms) {
  // Every 2 s: kick SNTP if WiFi just came up; check sync status.
  if ((now_ms - s_last_check_ms) < 2000) return;
  s_last_check_ms = now_ms;

  start_sntp_if_needed();

  if (!s_synced) {
    const sntp_sync_status_t st = sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_COMPLETED) {
      s_synced = true;
      time_t now;
      time(&now);
      struct tm lt;
      localtime_r(&now, &lt);
      LOG_I(TAG, "synced: %04d-%02d-%02d %02d:%02d:%02d",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
            lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
  }
}

bool is_synced() { return s_synced; }

bool get_local_time(struct tm* out) {
  if (!out) return false;
  time_t now;
  time(&now);
  if (now < 1700000000) return false;   // pre-2023 = clock not yet set
  localtime_r(&now, out);
  return true;
}

const char* current_tz_string() { return s_tz; }

const char* current_tz_name() {
  for (int i = 0; i < N_TZ_PRESETS; ++i) {
    if (strcmp(s_tz, TZ_PRESETS[i].posix_tz) == 0) return TZ_PRESETS[i].name;
  }
  return "custom";
}

bool set_tz_preset(int idx) {
  if (idx < 0 || idx >= N_TZ_PRESETS) return false;
  return set_tz_string(TZ_PRESETS[idx].posix_tz);
}

bool set_tz_string(const char* posix_tz) {
  if (!posix_tz || !*posix_tz) return false;
  strncpy(s_tz, posix_tz, sizeof(s_tz) - 1);
  s_tz[sizeof(s_tz) - 1] = '\0';
  apply_tz(s_tz);
  config::set_string(KEY_TZ, s_tz);
  LOG_I(TAG, "tz set to %s", s_tz);
  return true;
}

}  // namespace robimon::services::time_svc
