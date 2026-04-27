// Bot-to-bot proximity monitor.
//
// Scans for the paired peer's BLE address and tracks smoothed RSSI. When
// the peer is consistently nearby for PROXIMITY_DWELL_SECONDS, fires
// IN_PROXIMITY; when it's been away for PROXIMITY_RELEASE_SECONDS, fires
// OUT_OF_RANGE. Other state transitions (APPROACHING, LEAVING) are
// observable via the same callback for diagnostics + UI.
//
// This module does NOT make HTTP calls. The callback is the only output;
// the caller decides what to do (companion-app POST, UI hint, etc.).
//
// Pairing is out of scope here — see serial_console::cmd_pair_peer for
// how the peer MAC gets registered. ProximityMonitor reads the stored
// MAC from NVS at begin() and stays in UNPAIRED if none is set.

#pragma once

#include <stdint.h>

namespace robimon::services::proximity {

enum class State : uint8_t {
  UNPAIRED,        // no peer MAC stored — module is idle
  OUT_OF_RANGE,    // peer not seen, or smoothed RSSI < release threshold for the dwell
  APPROACHING,     // smoothed RSSI just crossed PROXIMITY_RSSI_THRESHOLD; dwell timer running
  IN_PROXIMITY,    // dwell satisfied — companion app should know about this
  LEAVING,         // smoothed RSSI just dropped below RELEASE_RSSI_THRESHOLD; release timer running
};

const char* state_str(State s);

// Single user callback. Fired only on state transitions, never on every
// sample. smoothed_rssi is meaningful for APPROACHING / IN_PROXIMITY /
// LEAVING; for UNPAIRED / OUT_OF_RANGE it may be a stale value.
using StateChangedFn = void (*)(State new_state, int8_t smoothed_rssi);

// Initialize the module. Reads the paired peer MAC from NVS (key
// NVS_KEY_PEER_MAC, 12 hex chars) and starts the BLE scan + advertise
// task on core 0. Safe to call before WiFi is up — BLE shares the radio
// with WiFi but they coexist.
//
// Returns false if NimBLE init fails. UNPAIRED on missing peer is NOT
// a failure — it logs a warning and runs in idle.
bool begin(StateChangedFn cb);

// Replace the paired peer at runtime. mac is 6 raw bytes (big-endian
// per the standard hex notation, e.g., AA:BB:CC:DD:EE:FF → bytes
// {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}). Persists to NVS.
void set_paired_peer(const uint8_t mac[6]);

// Forget the paired peer (clears NVS). Module returns to UNPAIRED.
void forget_paired_peer();

// Current observable state.
State  state();
int8_t smoothed_rssi();

// True when state() == IN_PROXIMITY. Convenience for callers (UI, etc.)
// that just want a bool.
bool   is_in_proximity();

// True when a peer MAC is stored.
bool   has_paired_peer();

// Copies the paired peer's MAC into out (6 bytes). Returns false if
// UNPAIRED. Used by the serial console to display current pairing state.
bool   get_paired_peer(uint8_t out[6]);

// Returns this device's own BLE address as a "AA:BB:CC:DD:EE:FF" string.
// Useful for the pairing flow — user reads it from one bot's serial
// console and types it into the other bot's pair-peer command.
const char* own_address();

}  // namespace robimon::services::proximity
