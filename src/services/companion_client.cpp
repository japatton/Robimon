#include "companion_client.h"
#include "proximity_config.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
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

  // Single attempt, no retry on failure. POSTs aren't idempotent on the
  // wire — the server already has (session_id, turn_index) dedup as
  // defense in depth, but the client retrying on what looked like a
  // failed POST (timeout while server actually completed) was the
  // duplicate-playback_complete root cause.
  HTTPClient http;
  // setReuse(true) keeps the underlying TCP connection alive across
  // POSTs from this same HTTPClient instance — saves the ~50 ms TCP
  // handshake on subsequent calls. The instance is local to this call,
  // so the win is bounded; for true cross-call reuse we'd need to
  // promote http to file scope.
  http.setReuse(true);
  http.setTimeout(proximity_config::OUTBOUND_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    LOG_W(TAG, "begin failed: %s", url);
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const uint32_t t0 = millis();
  const int code = http.POST((uint8_t*)body, body_len);
  const uint32_t dt = millis() - t0;
  http.end();
  if (code >= 200 && code < 300) {
    if (dt > 1000) LOG_I(TAG, "POST %s -> %d (%lu ms)", path, code, (unsigned long)dt);
    return true;
  }
  LOG_W(TAG, "POST %s -> %d (%lu ms)", path, code, (unsigned long)dt);
  return false;
}

void process(const Request& r) {
  char body[256];
  // The companion app needs the bot's local IP to POST /play back to it.
  // Include it on every event so the server's per-bot IP cache stays
  // fresh even if the bot's IP changes (DHCP renewal, etc).
  const IPAddress ip = WiFi.localIP();
  char ipstr[16];
  snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  const uint16_t port = proximity_config::PLAYBACK_SERVER_PORT;

  switch (r.kind) {
    case Kind::PROXIMITY_DETECTED:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"ip\":\"%s\",\"port\":%u,"
               "\"rssi\":%d,\"timestamp_ms\":%lu}",
               s_bot_id, ipstr, (unsigned)port,
               (int)r.rssi, (unsigned long)millis());
      post_json("/api/proximity/detected", body, strlen(body));
      break;
    case Kind::PROXIMITY_LOST:
      snprintf(body, sizeof(body),
               "{\"bot_id\":\"%s\",\"ip\":\"%s\",\"port\":%u,"
               "\"timestamp_ms\":%lu}",
               s_bot_id, ipstr, (unsigned)port, (unsigned long)millis());
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
  // Bot ID: last 4 hex chars of the BLE display address (Robimon-XXXX
  // form). The BLE address is the BT MAC = efuse base MAC + 2 in the
  // low byte, so we derive it without depending on BLE being up yet.
  // Without this alignment the companion server logs "bot_id=8898" but
  // the BLE scanner shows "Robimon-B355" — same device, different
  // identifiers — which has already cost debugging time.
  uint64_t base = ESP.getEfuseMac();
  uint8_t* b = (uint8_t*)&base;     // memory order = display order, byte 0..5
  const uint8_t ble_lsb_minus1 = b[4];
  const uint8_t ble_lsb        = (uint8_t)(b[5] + 2);   // BT MAC = base + 2 in LSB
  snprintf(s_bot_id, sizeof(s_bot_id), "%02X%02X", ble_lsb_minus1, ble_lsb);

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
