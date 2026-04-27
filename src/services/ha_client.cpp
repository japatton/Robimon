#include "ha_client.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <string.h>

namespace robimon::services::ha_client {

namespace {
constexpr const char* TAG = "ha";
constexpr const char* KEY_URL   = "ha_url";
constexpr const char* KEY_TOKEN = "ha_token";

constexpr uint32_t RECONNECT_BACKOFF_MS = 5000;
constexpr int      WS_PORT_DEFAULT      = 8123;

WebSocketsClient    s_ws;
State               s_state = State::DISCONNECTED;
char                s_url[160] = {0};
char                s_token[256] = {0};
char                s_host[80] = {0};
int                 s_port = WS_PORT_DEFAULT;
char                s_path[80] = "/api/websocket";
uint32_t            s_last_attempt_ms = 0;
uint32_t            s_next_msg_id = 10;

EntityState         s_entities[MAX_TRACKED_ENTITIES] = {0};
int                 s_entity_count = 0;

// ---- URL parsing ---------------------------------------------------------
// Accepts:
//   ws://host:8123/api/websocket
//   ws://host
//   host:8123
//   host
// Defaults port to 8123 and path to /api/websocket if missing.
bool parse_url(const char* url) {
  if (!url || !*url) return false;
  const char* p = url;
  if (strncmp(p, "ws://", 5) == 0)   p += 5;
  else if (strncmp(p, "http://", 7) == 0) p += 7;
  // host[:port][/path]
  const char* slash = strchr(p, '/');
  const char* colon = strchr(p, ':');
  if (colon && (!slash || colon < slash)) {
    const size_t host_len = (size_t)(colon - p);
    if (host_len >= sizeof(s_host)) return false;
    memcpy(s_host, p, host_len);
    s_host[host_len] = '\0';
    s_port = atoi(colon + 1);
  } else {
    const size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len >= sizeof(s_host)) return false;
    memcpy(s_host, p, host_len);
    s_host[host_len] = '\0';
    s_port = WS_PORT_DEFAULT;
  }
  if (slash) {
    strncpy(s_path, slash, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
  } else {
    strcpy(s_path, "/api/websocket");
  }
  return s_port > 0 && s_host[0] != '\0';
}

// ---- Entity table --------------------------------------------------------
EntityState* find_entity(const char* entity_id) {
  for (int i = 0; i < s_entity_count; ++i) {
    if (strcmp(s_entities[i].entity_id, entity_id) == 0) return &s_entities[i];
  }
  return nullptr;
}

// ---- WS handlers ---------------------------------------------------------
// Helper: serialize doc into a stack buffer and send. Avoids the per-call
// String allocation (Arduino's String concatenates via realloc and fragments
// the heap on long-running services like this).
template <size_t N>
bool send_doc(const JsonDocument& doc, char (&buf)[N]) {
  const size_t len = serializeJson(doc, buf, N);
  if (len == 0 || len >= N) {
    LOG_W(TAG, "serialize overflow");
    return false;
  }
  s_ws.sendTXT((uint8_t*)buf, len);
  return true;
}

void send_auth() {
  JsonDocument doc;
  doc["type"]         = "auth";
  doc["access_token"] = s_token;
  char buf[384];
  send_doc(doc, buf);
  s_state = State::AUTHENTICATING;
}

void subscribe_state_changes() {
  JsonDocument doc;
  doc["id"]         = s_next_msg_id++;
  doc["type"]       = "subscribe_events";
  doc["event_type"] = "state_changed";
  char buf[128];
  send_doc(doc, buf);
}

void request_initial_states() {
  JsonDocument doc;
  doc["id"]   = s_next_msg_id++;
  doc["type"] = "get_states";
  char buf[64];
  send_doc(doc, buf);
}

void apply_state_object(JsonObjectConst obj) {
  const char* entity_id = obj["entity_id"];
  if (!entity_id) return;
  EntityState* e = find_entity(entity_id);
  if (!e) return;   // not tracked
  const char* st = obj["state"];
  JsonObjectConst attrs = obj["attributes"];
  const char* fn = attrs["friendly_name"];
  if (st) { strncpy(e->state, st, MAX_STATE_LEN); e->state[MAX_STATE_LEN] = '\0'; }
  if (fn) { strncpy(e->friendly_name, fn, MAX_NAME_LEN); e->friendly_name[MAX_NAME_LEN] = '\0'; }

  // Light attrs — present when the entity is a light, absent (= -1) otherwise.
  e->brightness             = attrs["brightness"]             | -1;
  e->color_temp_kelvin      = attrs["color_temp_kelvin"]      | -1;
  e->min_color_temp_kelvin  = attrs["min_color_temp_kelvin"]  | -1;
  e->max_color_temp_kelvin  = attrs["max_color_temp_kelvin"]  | -1;

  e->received_ms = millis();
  e->valid = true;
}

void on_text(const char* payload, size_t len) {
  // Hard cap on payload size. Real HA state_changed events are <8 KB; an
  // entity with a multi-MB attribute (or a malformed/malicious frame) would
  // otherwise force ArduinoJson to allocate up to that size.
  constexpr size_t MAX_FRAME_BYTES = 32 * 1024;
  if (len > MAX_FRAME_BYTES) {
    LOG_W(TAG, "frame too large: %u bytes", (unsigned)len);
    return;
  }

  JsonDocument doc;
  // HA state_changed events can nest fairly deep when entities have rich
  // attributes (e.g., nested device_info / connections arrays). The
  // ArduinoJson default of 10 levels triggers TooDeep on some entities;
  // 20 is generous and well clear of any real HA payload.
  const auto err = deserializeJson(doc, payload, len,
                                    DeserializationOption::NestingLimit(20));
  if (err) {
    LOG_W(TAG, "json parse error: %s", err.c_str());
    return;
  }
  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "auth_required") == 0) {
    send_auth();
  } else if (strcmp(type, "auth_ok") == 0) {
    s_state = State::CONNECTED;
    LOG_I(TAG, "auth ok (HA %s)", (const char*)(doc["ha_version"] | "?"));
    // Skip get_states: on real HA installs the response is 100+ KB, well
    // past the WebSocket frame buffer, and the server drops the connection
    // when it can't deliver. Just subscribe to state_changed and let the
    // entity cache fill in as the world updates around us.
    subscribe_state_changes();
  } else if (strcmp(type, "auth_invalid") == 0) {
    LOG_W(TAG, "auth invalid: %s", (const char*)(doc["message"] | "?"));
    s_state = State::FAILED;
    s_ws.disconnect();
  } else if (strcmp(type, "result") == 0) {
    // Either a get_states response (result is an array of state objects),
    // or a result for our subscribe_events ack (ignored).
    if (doc["result"].is<JsonArrayConst>()) {
      for (JsonObjectConst e : doc["result"].as<JsonArrayConst>()) {
        apply_state_object(e);
      }
    }
  } else if (strcmp(type, "event") == 0) {
    if (doc["event"]["event_type"] == "state_changed") {
      apply_state_object(doc["event"]["data"]["new_state"].as<JsonObjectConst>());
    }
  }
}

void on_event(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_I(TAG, "ws disconnected");
      if (s_state != State::FAILED) s_state = State::DISCONNECTED;
      break;
    case WStype_CONNECTED:
      LOG_I(TAG, "ws connected");
      // HA sends auth_required as the first frame; we react in on_text.
      break;
    case WStype_TEXT:
      on_text((const char*)payload, len);
      break;
    case WStype_ERROR:
      LOG_W(TAG, "ws error");
      break;
    default:
      break;
  }
}

}  // namespace

void begin() {
  s_state = State::DISCONNECTED;
  s_url[0] = s_token[0] = '\0';
}

void update(uint32_t now_ms) {
  if (s_state == State::CONNECTED || s_state == State::AUTHENTICATING ||
      s_state == State::CONNECTING) {
    s_ws.loop();
  }
  // Backoff retry when failed/disconnected with valid creds.
  if (s_state == State::DISCONNECTED && s_url[0] && s_token[0] &&
      wifi_mgr::state() == wifi_mgr::State::CONNECTED &&
      (now_ms - s_last_attempt_ms) > RECONNECT_BACKOFF_MS) {
    s_last_attempt_ms = now_ms;
    if (parse_url(s_url)) {
      LOG_I(TAG, "connecting %s:%d%s", s_host, s_port, s_path);
      s_ws.begin(s_host, s_port, s_path);
      s_ws.onEvent(on_event);
      s_ws.setReconnectInterval(0);   // we drive reconnect ourselves
      s_state = State::CONNECTING;
    } else {
      LOG_W(TAG, "bad URL: '%s'", s_url);
      s_state = State::FAILED;
    }
  }
}

State        state()       { return s_state; }
const char*  state_str() {
  switch (s_state) {
    case State::DISCONNECTED:   return "disconnected";
    case State::CONNECTING:     return "connecting";
    case State::AUTHENTICATING: return "auth";
    case State::CONNECTED:      return "connected";
    case State::FAILED:         return "failed";
  }
  return "?";
}
const char*  url()         { return s_url; }

bool connect(const char* url, const char* token) {
  if (!url || !*url || !token || !*token) return false;
  strncpy(s_url,   url,   sizeof(s_url)   - 1); s_url[sizeof(s_url) - 1] = '\0';
  strncpy(s_token, token, sizeof(s_token) - 1); s_token[sizeof(s_token) - 1] = '\0';
  config::set_string(KEY_URL,   s_url);
  config::set_string(KEY_TOKEN, s_token);
  // Force a reconnect attempt right away.
  s_ws.disconnect();
  s_state = State::DISCONNECTED;
  s_last_attempt_ms = 0;
  return true;
}

void disconnect() {
  s_ws.disconnect();
  s_state = State::DISCONNECTED;
}

void forget() {
  disconnect();
  config::set_string(KEY_URL,   "");
  config::set_string(KEY_TOKEN, "");
  s_url[0] = s_token[0] = '\0';
}

bool auto_connect() {
  char url[sizeof(s_url)]   = {0};
  char tok[sizeof(s_token)] = {0};
  if (config::get_string(KEY_URL,   url, sizeof(url)) == 0) return false;
  if (config::get_string(KEY_TOKEN, tok, sizeof(tok)) == 0) return false;
  return connect(url, tok);
}

bool track_entity(const char* entity_id) {
  if (!entity_id || !*entity_id) return false;
  if (find_entity(entity_id)) return true;
  if (s_entity_count >= MAX_TRACKED_ENTITIES) return false;
  EntityState& e = s_entities[s_entity_count++];
  strncpy(e.entity_id, entity_id, MAX_ENTITY_ID_LEN);
  e.entity_id[MAX_ENTITY_ID_LEN] = '\0';
  e.friendly_name[0] = '\0';
  e.state[0] = '\0';
  e.received_ms = 0;
  e.valid = false;
  // We rely on incoming state_changed events to populate the cache. For an
  // entity that updates rarely, the screen will show "(waiting for data)"
  // until the first change. A future enhancement: per-entity REST fetch.
  return true;
}

const EntityState* get_state(const char* entity_id) {
  EntityState* e = find_entity(entity_id);
  if (!e || !e->valid) return nullptr;
  return e;
}

bool call_service(const char* domain, const char* service, const char* entity_id) {
  if (s_state != State::CONNECTED) return false;
  if (!domain || !service || !entity_id) return false;

  JsonDocument doc;
  doc["id"]                   = s_next_msg_id++;
  doc["type"]                 = "call_service";
  doc["domain"]               = domain;
  doc["service"]              = service;
  doc["target"]["entity_id"]  = entity_id;

  char buf[256];
  if (!send_doc(doc, buf)) return false;
  LOG_I(TAG, "call_service %s.%s -> %s", domain, service, entity_id);
  return true;
}

bool light_turn_on(const char* entity_id, int brightness, int color_temp_kelvin) {
  if (s_state != State::CONNECTED || !entity_id) return false;
  JsonDocument doc;
  doc["id"]                   = s_next_msg_id++;
  doc["type"]                 = "call_service";
  doc["domain"]               = "light";
  doc["service"]              = "turn_on";
  doc["target"]["entity_id"]  = entity_id;
  if (brightness        >= 0) doc["service_data"]["brightness"]        = brightness;
  if (color_temp_kelvin >= 0) doc["service_data"]["color_temp_kelvin"] = color_temp_kelvin;
  char buf[320];
  if (!send_doc(doc, buf)) return false;
  LOG_I(TAG, "light.turn_on %s b=%d k=%d", entity_id, brightness, color_temp_kelvin);
  return true;
}

bool light_turn_off(const char* entity_id) {
  return call_service("light", "turn_off", entity_id);
}

}  // namespace robimon::services::ha_client
