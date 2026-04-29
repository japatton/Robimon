#include "proximity_monitor.h"
#include "proximity_config.h"
#include "config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <atomic>
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

// ---- Diagnostic counters (NimBLE host task writes, our task reads + zeros) -
// Atomics rather than under the ring mux: keeps the scan-callback critical
// section as short as possible (BLE host task will block ad processing while
// we hold the lock).
std::atomic<uint32_t> s_adverts_seen{0};   // every onResult fires this
std::atomic<uint32_t> s_peer_matches{0};   // matched peer MAC
std::atomic<uint32_t> s_pushed_samples{0}; // matched + passed sentinel filter

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
// We pin our own address type to BLE_OWN_ADDR_PUBLIC at init (see begin()),
// and store the peer with the same type, so NimBLEAddress::operator== works
// here without the bytes-only workaround that the early bring-up needed.
// Keep the bytes-only fallback as belt-and-suspenders behind a quick type
// check — the comparison cost is identical.
// BLE-spec value for "RSSI not available" — host stacks (NimBLE included)
// surface this from HCI events that lack the rssi field. Casting it to
// int8_t leaves it as 127, which trivially passes any "rssi >= threshold"
// check and was the source of phantom proximity events at boot.
constexpr int RSSI_SENTINEL_NOT_AVAILABLE = 127;

class ScanCB : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    s_adverts_seen.fetch_add(1, std::memory_order_relaxed);
    if (!s_has_peer) return;
    const NimBLEAddress& addr = dev->getAddress();
    bool match = (addr == s_peer_addr);
    if (!match) {
      // Type may have flipped under us (some NimBLE builds randomize on
      // session start). Compare raw bytes as a fallback so we don't miss
      // adverts because of a stack-internal type mismatch.
      const uint8_t* native = addr.getBase()->val;   // little-endian
      match = true;
      for (int i = 0; i < 6; ++i) {
        if (native[i] != s_peer_mac[5 - i]) { match = false; break; }
      }
    }
    if (!match) return;
    s_peer_matches.fetch_add(1, std::memory_order_relaxed);

    const int rssi = dev->getRSSI();
    // BLE RSSI is signed dBm and in practice always negative for real
    // signals. Drop the 127 "not available" sentinel and any other
    // non-negative value — these are stack-internal placeholders, not
    // real measurements.
    if (rssi >= 0 || rssi == RSSI_SENTINEL_NOT_AVAILABLE) return;
    push_sample((int8_t)rssi);
    s_pushed_samples.fetch_add(1, std::memory_order_relaxed);
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

  // No sample seen recently? Treat as out-of-range. SAMPLE_STALE_MS is
  // generous (3 s) — BLE+WiFi coexistence on this chip makes adverts
  // bouncy, and a single good sample needs to bridge the dwell window
  // even if the next several are dropped.
  const uint32_t stale_ms = now_ms - s_last_sample_ms;
  if (stale_ms > SAMPLE_STALE_MS) {
    s_smoothed_rssi = -127;
    clear_samples();
  } else {
    s_smoothed_rssi = compute_smoothed();
  }

  // Did we receive a fresh sample THIS tick? Used below to differentiate
  // "sample says peer is gone" from "we just haven't heard anything yet".
  // BLE+WiFi coexistence makes individual samples bouncy, so we don't
  // want to bail out of APPROACHING / IN_PROXIMITY just because no
  // adverts came through in one window.
  const bool fresh = (stale_ms <= SAMPLE_STALE_MS);

  // Absolute-staleness watchdog: if it's been ABSOLUTE_STALE_MS with NO
  // samples whatsoever, the peer has actually gone away (powered off,
  // walked out of range during a Wi-Fi-noise gap). Without this the
  // "fresh sample required to demote" rule latches IN_PROXIMITY forever.
  const bool absolute_stale = (stale_ms > ABSOLUTE_STALE_MS);

  switch (s_state) {
    case State::UNPAIRED:
      // Just paired? Wait for samples.
      transition(State::OUT_OF_RANGE, now_ms);
      break;

    case State::OUT_OF_RANGE:
      if (fresh && s_smoothed_rssi >= PROXIMITY_RSSI_THRESHOLD) {
        transition(State::APPROACHING, now_ms);
      }
      break;

    case State::APPROACHING:
      // Promotion to IN_PROXIMITY requires BOTH the dwell window to have
      // elapsed AND a fresh sample still meeting threshold. Without the
      // fresh+strong gate, a single transient sample at boot would tip
      // OUT_OF_RANGE -> APPROACHING and then dwell alone would latch us
      // into IN_PROXIMITY even though the peer was never really there.
      // Demote on a fresh below-release sample (real signal, peer moved
      // away) or on absolute staleness (peer vanished silently).
      if (absolute_stale) {
        transition(State::OUT_OF_RANGE, now_ms);
      } else if (fresh && s_smoothed_rssi < RELEASE_RSSI_THRESHOLD) {
        transition(State::OUT_OF_RANGE, now_ms);
      } else if (fresh && s_smoothed_rssi >= PROXIMITY_RSSI_THRESHOLD &&
                 (now_ms - s_state_started_ms) >= PROXIMITY_DWELL_SECONDS * 1000UL) {
        transition(State::IN_PROXIMITY, now_ms);
      }
      break;

    case State::IN_PROXIMITY:
      // Only fall back to LEAVING on a fresh sample below release
      // (peer actually moved away) or after IN_PROXIMITY_STALE_MS of
      // no samples at all (peer truly vanished). The longer stale
      // window vs APPROACHING is intentional: scan-starvation droughts
      // during IN_PROXIMITY were causing spurious LEAVING -> back-to-
      // IN_PROXIMITY flickers, each firing a /api/proximity/lost POST
      // that the companion immediately had to undo.
      if ((stale_ms > IN_PROXIMITY_STALE_MS) ||
          (fresh && s_smoothed_rssi < RELEASE_RSSI_THRESHOLD)) {
        transition(State::LEAVING, now_ms);
      }
      break;

    case State::LEAVING:
      if (fresh && s_smoothed_rssi >= PROXIMITY_RSSI_THRESHOLD) {
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

// ---- Periodic stats log --------------------------------------------------
// Emit one line every 5 s with state, smoothed RSSI, and the three counters
// (atomically read + zeroed). Three numbers tell you exactly where a missing
// detection is breaking down:
//   adverts=0       -> BLE scan isn't running at all
//   adverts>0,
//   matches=0       -> scan is alive but peer MAC isn't in range / paired
//                      address is wrong
//   matches>0,
//   pushed=0        -> peer is being seen but every reading is the 127
//                      "not available" sentinel (stack/coexistence quirk)
//   pushed>0        -> samples reaching the ring; state machine is in charge
constexpr uint32_t PROX_STATS_INTERVAL_MS = 5000;

void emit_stats_line() {
  const uint32_t adv     = s_adverts_seen.exchange(0, std::memory_order_relaxed);
  const uint32_t matches = s_peer_matches.exchange(0, std::memory_order_relaxed);
  const uint32_t pushed  = s_pushed_samples.exchange(0, std::memory_order_relaxed);
  uint8_t ring_count;
  portENTER_CRITICAL(&s_ring_mux);
  ring_count = s_ring_count;
  portEXIT_CRITICAL(&s_ring_mux);
  LOG_I(TAG, "stats state=%s smoothed=%d ring=%u adv=%lu match=%lu push=%lu",
        state_str(s_state), (int)s_smoothed_rssi, (unsigned)ring_count,
        (unsigned long)adv, (unsigned long)matches, (unsigned long)pushed);

  // Self-heal the BLE scan. WiFi+BLE coexistence on the ESP32-S3 doesn't
  // just preempt scan on hard events (WiFi reconnect, captive portal) —
  // it also progressively starves it under sustained WiFi load (HA
  // WebSocket churn, weak-signal retransmits). On the bench, a healthy
  // scan window shows ~30 adverts; a starved one shows 1-4 (controller
  // dribble, not real reception). So treat any window under MIN_HEALTHY
  // as broken and kick start() again. Cheap (one HCI command);
  // idempotent if the scan is actually running. The next window's
  // counter tells us whether the re-arm worked.
  constexpr uint32_t MIN_HEALTHY_ADV_PER_WINDOW = 10;
  if (s_has_peer && adv < MIN_HEALTHY_ADV_PER_WINDOW) {
    LOG_W(TAG, "scan starved (adv=%lu in 5s) — re-arming", (unsigned long)adv);
    start_scan();
  }
}

// ---- Task body -----------------------------------------------------------
void proximity_task(void*) {
  esp_task_wdt_add(NULL);

  start_advertising();
  if (s_has_peer) start_scan();

  uint32_t last_stats_ms = millis();
  for (;;) {
    esp_task_wdt_reset();
    update_state_machine();
    const uint32_t now = millis();
    if ((now - last_stats_ms) >= PROX_STATS_INTERVAL_MS) {
      emit_stats_line();
      last_stats_ms = now;
    }
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

  // BLE init. Device name suffix is the LOW 16 bits of the BLE address
  // (computed after init below) so the scanner-shown name and the
  // bot_id used by the companion server line up exactly.
  NimBLEDevice::init("Robimon");
  // Pin to public address type — the chip's BLE address derives from the
  // efuse MAC and is stable across reboots. Without this, NimBLE may
  // pick a random/RPA address per session and our peer-comparison fails
  // silently. The bytes-only fallback in the scan callback is belt-and-
  // suspenders for the rare case it still flips.
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

  // Cache our own address (derived from the BT MAC, which is base+2 from
  // the efuse) and update the advertised device name with the matching
  // 4-hex suffix so scanner apps and the companion server's bot_id agree.
  const NimBLEAddress own = NimBLEDevice::getAddress();
  strncpy(s_own_addr_str, own.toString().c_str(), sizeof(s_own_addr_str) - 1);
  s_own_addr_str[sizeof(s_own_addr_str) - 1] = '\0';
  const uint8_t* own_native = own.getBase()->val;   // little-endian
  // Display-order low 2 bytes = native[1], native[0]
  char name[24];
  snprintf(name, sizeof(name), "Robimon-%02X%02X", own_native[1], own_native[0]);
  NimBLEDevice::setDeviceName(name);
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
