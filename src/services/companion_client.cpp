#include "companion_client.h"
#include "proximity_config.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

namespace robimon::services::companion_client {

namespace {
constexpr const char* TAG = "comp";

constexpr UBaseType_t QUEUE_DEPTH = 8;

enum class Kind : uint8_t {
  PROXIMITY_DETECTED,
  PROXIMITY_LOST,
  PLAYBACK_COMPLETE,
  PLAYBACK_ERROR,
};

struct Request {
  Kind     kind;
  int8_t   rssi;            // detected
  uint8_t  turn_index;      // playback_complete
  char     session_id[40];  // *_complete / playback_error
  char     err[64];         // playback_error
};

QueueHandle_t s_queue   = nullptr;
TaskHandle_t  s_task    = nullptr;

char s_bot_id[6] = "????";   // 4 hex + NUL + slack
char s_base_url[160] = {0};

void load_base_url() {
  if (config::get_string(proximity_config::NVS_KEY_COMPANION_URL,
                          s_base_url, sizeof(s_base_url)) == 0) {
    strncpy(s_base_url, proximity_config::DEFAULT_COMPANION_URL,
            sizeof(s_base_url) - 1);
    s_base_url[sizeof(s_base_url) - 1] = '\0';
  }
}

bool post_json(const char* path, const char* body, size_t body_len) {
  if (wifi_mgr::state() != wifi_mgr::State::CONNECTED) {
    LOG_W(TAG, "skipping POST %s — wifi down", path);
    return false;
  }
  char url[224];
  snprintf(url, sizeof(url), "%s%s", s_base_url, path);

  for (uint8_t attempt = 0; attempt <= proximity_config::OUTBOUND_HTTP_RETRIES; ++attempt) {
    HTTPClient http;
    http.setTimeout(proximity_config::OUTBOUND_HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
      LOG_W(TAG, "begin failed: %s", url);
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST((uint8_t*)body, body_len);
    http.end();
    if (code >= 200 && code < 300) {
      return true;
    }
    LOG_W(TAG, "POST %s -> %d (attempt %u)", path, code, (unsigned)(attempt + 1));
    esp_task_wdt_reset();
    if (attempt < proximity_config::OUTBOUND_HTTP_RETRIES) {
      vTaskDelay(pdMS_TO_TICKS(150));
    }
  }
  return false;
}

void process(const Request& r) {
  char body[256];
  switch (r.kind) {
    case Kind::PROXIMITY_DETECTED:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"rssi\":%d,\"timestamp_ms\":%lu}",
               s_bot_id, (int)r.rssi, (unsigned long)millis());
      post_json("/api/proximity/detected", body, strlen(body));
      break;
    case Kind::PROXIMITY_LOST:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"timestamp_ms\":%lu}",
               s_bot_id, (unsigned long)millis());
      post_json("/api/proximity/lost", body, strlen(body));
      break;
    case Kind::PLAYBACK_COMPLETE:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"session_id\":\"%s\",\"turn_index\":%u,\"timestamp_ms\":%lu}",
               s_bot_id, r.session_id, (unsigned)r.turn_index,
               (unsigned long)millis());
      post_json("/api/playback/complete", body, strlen(body));
      break;
    case Kind::PLAYBACK_ERROR:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"session_id\":\"%s\",\"error\":\"%s\",\"timestamp_ms\":%lu}",
               s_bot_id, r.session_id, r.err, (unsigned long)millis());
      post_json("/api/playback/error", body, strlen(body));
      break;
  }
}

void worker_task(void*) {
  esp_task_wdt_add(NULL);
  Request r;
  for (;;) {
    if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(500)) == pdTRUE) {
      process(r);
    }
    esp_task_wdt_reset();
  }
}

bool enqueue(const Request& r) {
  if (!s_queue) return false;
  if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
    LOG_W(TAG, "queue full — dropping %u", (unsigned)r.kind);
    return false;
  }
  return true;
}

}  // namespace

void begin() {
  // Bot ID: last 4 hex of efuse MAC. Matches the BLE device-name suffix
  // and the existing voice client's identification scheme.
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(s_bot_id, sizeof(s_bot_id), "%04X", (uint16_t)(mac & 0xFFFF));

  load_base_url();
  LOG_I(TAG, "bot_id=%s base_url=%s", s_bot_id, s_base_url);

  s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(Request));
  if (!s_queue) {
    LOG_E(TAG, "queue create failed");
    return;
  }
  // 8 KB stack covers HTTPClient + TLS-less POST locals.
  xTaskCreatePinnedToCore(worker_task, "comp_http", 8192, nullptr,
                           /*priority=*/1, &s_task, /*core=*/0);
}

const char* bot_id() { return s_bot_id; }

bool send_proximity_detected(int8_t rssi) {
  Request r{};
  r.kind = Kind::PROXIMITY_DETECTED;
  r.rssi = rssi;
  return enqueue(r);
}

bool send_proximity_lost() {
  Request r{};
  r.kind = Kind::PROXIMITY_LOST;
  return enqueue(r);
}

bool send_playback_complete(const char* session_id, uint8_t turn_index) {
  Request r{};
  r.kind = Kind::PLAYBACK_COMPLETE;
  r.turn_index = turn_index;
  if (session_id) {
    strncpy(r.session_id, session_id, sizeof(r.session_id) - 1);
  }
  return enqueue(r);
}

bool send_playback_error(const char* session_id, const char* err) {
  Request r{};
  r.kind = Kind::PLAYBACK_ERROR;
  if (session_id) {
    strncpy(r.session_id, session_id, sizeof(r.session_id) - 1);
  }
  if (err) {
    strncpy(r.err, err, sizeof(r.err) - 1);
  }
  return enqueue(r);
}

}  // namespace robimon::services::companion_client
