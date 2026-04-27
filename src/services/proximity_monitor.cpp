#include "proximity_monitor.h"
#include "proximity_config.h"
#include "config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <ctype.h>

namespace robimon::services::proximity {

namespace {
constexpr const char* TAG = "prox";

// ---- State, owned by the proximity task -----------------------------------
StateChangedFn s_cb              = nullptr;
uint8_t        s_peer_mac[6]     = {0};
bool           s_has_peer        = false;
NimBLEAddress  s_peer_addr;     // mirrors s_peer_mac for fast == comparison

State    s_state             = State::UNPAIRED;
int8_t   s_smoothed_rssi     = -127;
uint32_t s_state_started_ms  = 0;
uint32_t s_last_sample_ms    = 0;

TaskHandle_t s_task           = nullptr;
char         s_own_addr_str[18] = {0};

// ---- RSSI ring buffer, written by NimBLE scan task, read by our task -----
portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
int8_t       s_ring[proximity_config::RSSI_SAMPLE_WINDOW] = {0};
uint8_t      s_ring_count = 0;
uint8_t      s_ring_pos   = 0;

void push_sample(int8_t rssi) {
  portENTER_CRITICAL(&s_ring_mux);
  s_ring[s_ring_pos] = rssi;
  s_ring_pos = (s_ring_pos + 1) % proximity_config::RSSI_SAMPLE_WINDOW;
  if (s_ring_count < proximity_config::RSSI_SAMPLE_WINDOW) s_ring_count++;
  s_last_sample_ms = millis();
  portEXIT_CRITICAL(&s_ring_mux);
}

int8_t compute_smoothed() {
  int sum = 0;
  uint8_t n = 0;
  portENTER_CRITICAL(&s_ring_mux);
  n = s_ring_count;
  for (uint8_t i = 0; i < n; ++i) sum += s_ring[i];
  portEXIT_CRITICAL(&s_ring_mux);
  if (n == 0) return -127;
  return (int8_t)(sum / n);
}

void clear_samples() {
  portENTER_CRITICAL(&s_ring_mux);
  s_ring_count = 0;
  s_ring_pos   = 0;
  portEXIT_CRITICAL(&s_ring_mux);
}

// ---- NimBLE scan callback -------------------------------------------------
class ScanCB : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!s_has_peer) return;
    if (dev->getAddress() != s_peer_addr) return;
    const int rssi = dev->getRSSI();
    push_sample((int8_t)rssi);
  }
};
ScanCB s_scan_cb;

// ---- Address helpers ------------------------------------------------------
void mac_to_string(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool string_to_mac(const char* in, uint8_t out[6]) {
  // Accepts AA:BB:CC:DD:EE:FF or AABBCCDDEEFF (no separators).
  uint8_t b = 0;
  uint8_t nibble = 0;
  for (const char* p = in; *p && b < 6; ++p) {
    if (*p == ':' || *p == '-' || *p == ' ') continue;
    int v;
    if      (*p >= '0' && *p <= '9') v = *p - '0';
    else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
    else return false;
    if (nibble == 0) { out[b] = (uint8_t)(v << 4); nibble = 1; }
    else             { out[b] |= (uint8_t)v;       nibble = 0; b++; }
  }
  return b == 6 && nibble == 0;
}

// ---- NVS load / save ------------------------------------------------------
bool load_peer_from_nvs() {
  char buf[32] = {0};
  if (config::get_string(proximity_config::NVS_KEY_PEER_MAC, buf, sizeof(buf)) == 0) {
    return false;
  }
  if (!string_to_mac(buf, s_peer_mac)) {
    LOG_W(TAG, "stored peer MAC unparseable: '%s'", buf);
    return false;
  }
  s_peer_addr = NimBLEAddress(buf, BLE_ADDR_PUBLIC);
  s_has_peer = true;
  return true;
}

void save_peer_to_nvs() {
  char buf[18];
  mac_to_string(s_peer_mac, buf);
  config::set_string(proximity_config::NVS_KEY_PEER_MAC, buf);
}

// ---- State machine -------------------------------------------------------
void transition(State next, uint32_t now_ms) {
  if (s_state == next) return;
  LOG_I(TAG, "state %s -> %s (rssi=%d)",
        state_str(s_state), state_str(next), (int)s_smoothed_rssi);
  s_state            = next;
  s_state_started_ms = now_ms;
  if (s_cb) s_cb(next, s_smoothed_rssi);
}

void update_state_machine() {
  using namespace proximity_config;
  const uint32_t now_ms = millis();

  // No peer paired? Stay UNPAIRED. set_paired_peer() will kick us out.
  if (!s_has_peer) {
    transition(State::UNPAIRED, now_ms);
    return;
  }

  // No sample seen recently? Treat as out-of-range. The scan window means
  // we should hear from a nearby peer at least every BLE_ADV_INTERVAL_MS;
  // 4× that with no sample = peer is gone.
  const uint32_t stale_ms = now_ms - s_last_sample_ms;
  const uint32_t stale_threshold_ms = BLE_ADV_INTERVAL_MS * 4UL;
  if (stale_ms > stale_threshold_ms) {
    s_smoothed_rssi = -127;
    clear_samples();
  } else {
    s_smoothed_rssi = compute_smoothed();
  }

  switch (s_state) {
    case State::UNPAIRED:
      // Just paired? Wait for samples.
      transition(State::OUT_OF_RANGE, now_ms);
      break;

    case State::OUT_OF_RANGE:
      if (s_smoothed_rssi >= PROXIMITY_RSSI_THRESHOLD) {
        transition(State::APPROACHING, now_ms);
      }
      break;

    case State::APPROACHING:
      if (s_smoothed_rssi < RELEASE_RSSI_THRESHOLD) {
        transition(State::OUT_OF_RANGE, now_ms);
      } else if ((now_ms - s_state_started_ms) >= PROXIMITY_DWELL_SECONDS * 1000UL) {
        transition(State::IN_PROXIMITY, now_ms);
      }
      break;

    case State::IN_PROXIMITY:
      if (s_smoothed_rssi < RELEASE_RSSI_THRESHOLD) {
        transition(State::LEAVING, now_ms);
      }
      break;

    case State::LEAVING:
      if (s_smoothed_rssi >= PROXIMITY_RSSI_THRESHOLD) {
        // Came back into range — straight to IN_PROXIMITY (no dwell, since
        // we already established the connection).
        transition(State::IN_PROXIMITY, now_ms);
      } else if ((now_ms - s_state_started_ms) >= PROXIMITY_RELEASE_SECONDS * 1000UL) {
        transition(State::OUT_OF_RANGE, now_ms);
      }
      break;
  }
}

// ---- Scan lifecycle ------------------------------------------------------
void start_scan() {
  using namespace proximity_config;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return;
  scan->setActiveScan(false);                                    // passive — we only need RSSI
  scan->setInterval(BLE_SCAN_INTERVAL_MS * 16 / 10);             // ms → 0.625ms slots
  scan->setWindow(BLE_SCAN_WINDOW_MS   * 16 / 10);
  scan->setMaxResults(0);                                        // streaming, no result cache
  scan->setScanCallbacks(&s_scan_cb, /*duplicate_filter=*/false);
  scan->start(0, /*restart=*/true);                              // 0 = continuous
}

void start_advertising() {
  using namespace proximity_config;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  // Convert ms to 0.625ms slots.
  adv->setMinInterval(BLE_ADV_INTERVAL_MS * 16 / 10);
  adv->setMaxInterval(BLE_ADV_INTERVAL_MS * 16 / 10);
  adv->start();
}

// ---- Task body -----------------------------------------------------------
void proximity_task(void*) {
  esp_task_wdt_add(NULL);

  start_advertising();
  if (s_has_peer) start_scan();

  for (;;) {
    esp_task_wdt_reset();
    update_state_machine();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

const char* state_str(State s) {
  switch (s) {
    case State::UNPAIRED:     return "UNPAIRED";
    case State::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case State::APPROACHING:  return "APPROACHING";
    case State::IN_PROXIMITY: return "IN_PROXIMITY";
    case State::LEAVING:      return "LEAVING";
  }
  return "?";
}

bool begin(StateChangedFn cb) {
  s_cb = cb;

  // BLE init. Device name uses the same scheme as the existing voice
  // bot_id (last 4 hex of MAC) so two bots in a household are
  // distinguishable in a scanner app.
  const uint64_t mac = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "Robimon-%04X", (uint16_t)(mac & 0xFFFF));
  NimBLEDevice::init(name);

  // Cache our own address as a string for the pairing flow.
  const NimBLEAddress own = NimBLEDevice::getAddress();
  strncpy(s_own_addr_str, own.toString().c_str(), sizeof(s_own_addr_str) - 1);
  s_own_addr_str[sizeof(s_own_addr_str) - 1] = '\0';
  LOG_I(TAG, "BLE up: name='%s' addr=%s", name, s_own_addr_str);

  if (load_peer_from_nvs()) {
    char buf[18];
    mac_to_string(s_peer_mac, buf);
    LOG_I(TAG, "paired peer: %s", buf);
  } else {
    LOG_W(TAG, "no peer paired — module idle until pair-peer is run");
  }

  // Pin to core 0 so it doesn't compete with the UI on core 1.
  xTaskCreatePinnedToCore(proximity_task, "proximity", 4096, nullptr,
                           /*priority=*/1, &s_task, /*core=*/0);
  return true;
}

void set_paired_peer(const uint8_t mac[6]) {
  memcpy(s_peer_mac, mac, 6);
  char buf[18];
  mac_to_string(s_peer_mac, buf);
  s_peer_addr = NimBLEAddress(buf, BLE_ADDR_PUBLIC);
  s_has_peer  = true;
  save_peer_to_nvs();
  clear_samples();
  LOG_I(TAG, "paired with %s", buf);

  // Kick the scan if it wasn't running yet.
  start_scan();
}

void forget_paired_peer() {
  s_has_peer = false;
  config::set_string(proximity_config::NVS_KEY_PEER_MAC, "");
  clear_samples();
  s_state = State::UNPAIRED;
  LOG_I(TAG, "peer forgotten");
}

State        state()              { return s_state; }
int8_t       smoothed_rssi()      { return s_smoothed_rssi; }
bool         is_in_proximity()    { return s_state == State::IN_PROXIMITY; }
bool         has_paired_peer()    { return s_has_peer; }
const char*  own_address()        { return s_own_addr_str; }

bool get_paired_peer(uint8_t out[6]) {
  if (!s_has_peer) return false;
  memcpy(out, s_peer_mac, 6);
  return true;
}

}  // namespace robimon::services::proximity
