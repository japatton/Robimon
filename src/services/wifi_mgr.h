// WiFi connection manager. Owns the WiFi state machine (disconnected ↔
// connecting ↔ connected/failed), background scans, and persistence of the
// last successfully-used SSID/password into config_store NVS.
//
// Auto-connect on boot: if there's a saved SSID/password, we try to
// connect during begin(). update() drives the state transitions and is
// safe to call every loop iteration.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace robimon::services::wifi_mgr {

enum class State : uint8_t {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  FAILED,
};

constexpr int    MAX_SCAN_RESULTS = 12;
constexpr size_t MAX_SSID_LEN     = 32;
constexpr size_t MAX_PASS_LEN     = 64;

struct ScanResult {
  char    ssid[MAX_SSID_LEN + 1];
  int8_t  rssi;
  bool    secure;
};

void begin();
void update(uint32_t now_ms);

// State accessors
State        state();
const char*  state_str();
const char*  current_ssid();
const char*  current_ip();
int8_t       current_rssi();

// Manual connect (also saves creds on success)
bool connect(const char* ssid, const char* password);
void disconnect();
void forget();   // disconnect + clear saved creds

// Try saved creds from NVS. Returns false if there are no saved creds.
bool auto_connect();

// Async scan
bool start_scan();
bool scan_in_progress();
int  scan_count();
const ScanResult* scan_results();

}  // namespace robimon::services::wifi_mgr
