#include "wifi_mgr.h"
#include "config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

namespace robimon::services::wifi_mgr {

namespace {
constexpr const char* TAG = "wifi";
constexpr const char* KEY_SSID = "wifi_ssid";
constexpr const char* KEY_PASS = "wifi_pass";

constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

State        s_state = State::DISCONNECTED;
char         s_ssid[MAX_SSID_LEN + 1] = {0};
char         s_pass[MAX_PASS_LEN + 1] = {0};
char         s_ip[16] = {0};
uint32_t     s_connect_started_ms = 0;

ScanResult   s_scan_results[MAX_SCAN_RESULTS] = {0};
int          s_scan_count = 0;
bool         s_scan_running = false;

void update_ip_string() {
  if (WiFi.isConnected()) {
    const IPAddress ip = WiFi.localIP();
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    s_ip[0] = '\0';
  }
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);   // we manage credential persistence ourselves via config_store
  s_state = State::DISCONNECTED;
  s_ip[0] = '\0';
  LOG_I(TAG, "wifi mgr ready");
}

bool auto_connect() {
  char ssid[MAX_SSID_LEN + 1] = {0};
  char pass[MAX_PASS_LEN + 1] = {0};
  if (config::get_string(KEY_SSID, ssid, sizeof(ssid)) == 0) {
    LOG_I(TAG, "no saved SSID; auto_connect skipped");
    return false;
  }
  config::get_string(KEY_PASS, pass, sizeof(pass));   // may be empty (open AP)
  LOG_I(TAG, "auto_connect -> '%s'", ssid);
  return connect(ssid, pass);
}

bool connect(const char* ssid, const char* password) {
  if (!ssid || !*ssid) return false;

  // Cache for state machine + later persistence on success.
  strncpy(s_ssid, ssid,           sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0';
  if (password) { strncpy(s_pass, password, sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0'; }
  else { s_pass[0] = '\0'; }

  WiFi.disconnect(true, false);
  delay(50);
  WiFi.begin(s_ssid, s_pass);
  s_state = State::CONNECTING;
  s_connect_started_ms = millis();
  s_ip[0] = '\0';
  LOG_I(TAG, "connect '%s' (passlen=%u)", s_ssid, (unsigned)strlen(s_pass));
  return true;
}

void disconnect() {
  WiFi.disconnect(true, false);
  s_state = State::DISCONNECTED;
  s_ip[0] = '\0';
  LOG_I(TAG, "disconnected");
}

void forget() {
  disconnect();
  config::set_string(KEY_SSID, "");
  config::set_string(KEY_PASS, "");
  s_ssid[0] = '\0';
  s_pass[0] = '\0';
  LOG_I(TAG, "creds forgotten");
}

void update(uint32_t now_ms) {
  switch (s_state) {
    case State::CONNECTING:
      if (WiFi.isConnected()) {
        s_state = State::CONNECTED;
        update_ip_string();
        // Persist creds now that they actually worked.
        config::set_string(KEY_SSID, s_ssid);
        config::set_string(KEY_PASS, s_pass);
        LOG_I(TAG, "connected to '%s' @ %s (RSSI %d)", s_ssid, s_ip, (int)WiFi.RSSI());
      } else if ((now_ms - s_connect_started_ms) > CONNECT_TIMEOUT_MS) {
        s_state = State::FAILED;
        s_ip[0] = '\0';
        LOG_W(TAG, "connect to '%s' timed out (%u ms)", s_ssid, (unsigned)CONNECT_TIMEOUT_MS);
      }
      break;
    case State::CONNECTED:
      if (!WiFi.isConnected()) {
        s_state = State::DISCONNECTED;
        s_ip[0] = '\0';
        LOG_W(TAG, "lost connection to '%s'", s_ssid);
      }
      break;
    default:
      break;
  }

  // Drive scan completion polling.
  if (s_scan_running) {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
      s_scan_running = false;
      s_scan_count = 0;
      LOG_W(TAG, "scan failed");
    } else if (n >= 0) {
      s_scan_running = false;
      const int take = (n < MAX_SCAN_RESULTS) ? n : MAX_SCAN_RESULTS;
      for (int i = 0; i < take; ++i) {
        const String ssid = WiFi.SSID(i);
        strncpy(s_scan_results[i].ssid, ssid.c_str(), MAX_SSID_LEN);
        s_scan_results[i].ssid[MAX_SSID_LEN] = '\0';
        s_scan_results[i].rssi   = (int8_t)WiFi.RSSI(i);
        s_scan_results[i].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      s_scan_count = take;
      WiFi.scanDelete();
      LOG_I(TAG, "scan complete: %d networks", s_scan_count);
    }
  }
}

State        state()         { return s_state; }
const char*  state_str() {
  switch (s_state) {
    case State::DISCONNECTED: return "disconnected";
    case State::CONNECTING:   return "connecting";
    case State::CONNECTED:    return "connected";
    case State::FAILED:       return "failed";
    default:                  return "?";
  }
}
const char*  current_ssid()  { return s_ssid; }
const char*  current_ip()    { return s_ip; }
int8_t       current_rssi()  { return WiFi.isConnected() ? (int8_t)WiFi.RSSI() : 0; }

bool start_scan() {
  if (s_scan_running) return false;
  // Async scan; results delivered via update() polling scanComplete().
  const int r = WiFi.scanNetworks(/*async=*/true);
  s_scan_running = (r == WIFI_SCAN_RUNNING);
  if (s_scan_running) LOG_I(TAG, "scan started");
  return s_scan_running;
}

bool   scan_in_progress() { return s_scan_running; }
int    scan_count()       { return s_scan_count; }
const ScanResult* scan_results() { return s_scan_results; }

}  // namespace robimon::services::wifi_mgr
