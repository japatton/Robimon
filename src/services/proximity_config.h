// Proximity-triggered bot-to-bot conversation feature — central tunables.
//
// All thresholds, timeouts, and URLs live here so a hardware-calibration
// pass touches one file. Defaults are starting points; expect to tune
// PROXIMITY_RSSI_THRESHOLD and RELEASE_RSSI_THRESHOLD for your room.

#pragma once

#include <stdint.h>

namespace robimon::services::proximity_config {

// ---- Companion-app endpoints ----------------------------------------------
// Base URL of the existing companion server. The proximity feature pushes
// outbound POSTs to /api/proximity/{detected,lost} and /api/playback/
// {complete,error} on this host.
//
// Default points at the same host the voice flow uses; configurable via
// the "companion_url" NVS key (settable from the serial console).
constexpr const char* DEFAULT_COMPANION_URL = "http://companion.local:8765";

// ---- Inbound playback server ---------------------------------------------
// Port the bot listens on for /play and /stop POSTs from the companion
// app. Pick something out of the way — port 80 is reserved for the setup
// portal and port 8080 collides with common dev tools.
constexpr uint16_t PLAYBACK_SERVER_PORT = 8000;

// ---- BLE / proximity ------------------------------------------------------
// Trigger when smoothed peer RSSI is at or above this for DWELL_SECONDS.
// -55 dBm corresponds to roughly arm's reach in a typical living room;
// expect to recalibrate per environment. Higher (= more negative) values
// trigger sooner / at greater distance.
constexpr int8_t   PROXIMITY_RSSI_THRESHOLD = -55;

// Release when smoothed peer RSSI drops below this for RELEASE_SECONDS.
// The 15 dBm gap vs PROXIMITY_RSSI_THRESHOLD is intentional hysteresis —
// without it, a bot at threshold would oscillate between IN_PROXIMITY
// and OUT_OF_RANGE on every RSSI bounce.
constexpr int8_t   RELEASE_RSSI_THRESHOLD   = -70;

// Minimum dwell time above PROXIMITY_RSSI_THRESHOLD before we transition
// from APPROACHING to IN_PROXIMITY. Filters out fly-by detections (peer
// passing through a doorway).
constexpr uint32_t PROXIMITY_DWELL_SECONDS  = 3;

// Minimum dwell time below RELEASE_RSSI_THRESHOLD before we transition
// from LEAVING back to OUT_OF_RANGE. Smooths over momentary signal dips.
constexpr uint32_t PROXIMITY_RELEASE_SECONDS = 5;

// Number of recent RSSI samples averaged for the smoothed value. Raw RSSI
// can bounce 10+ dBm between consecutive ads from the same peer; a 5-deep
// moving average is enough to stabilize threshold checks without adding
// much detection lag.
constexpr uint8_t  RSSI_SAMPLE_WINDOW = 5;

// BLE scan / advertise rates. WiFi+BLE coexistence on the same 2.4 GHz
// radio means BLE only gets a fraction of the requested airtime — at
// 50 % duty + 200 ms adv interval we still saw single-digit samples per
// 3 s dwell. 40 % duty + 200 ms adv keeps detection reasonable without
// fully starving WiFi.
constexpr uint8_t  BLE_SCAN_DUTY_CYCLE_PERCENT = 40;
constexpr uint16_t BLE_SCAN_INTERVAL_MS        = 500;
constexpr uint16_t BLE_SCAN_WINDOW_MS =
    (uint16_t)(BLE_SCAN_INTERVAL_MS * BLE_SCAN_DUTY_CYCLE_PERCENT / 100);
constexpr uint16_t BLE_ADV_INTERVAL_MS         = 200;

// How long a smoothed RSSI sample stays "valid" before we treat the
// peer as out of range. Generous (3 s) because BLE+WiFi coexistence
// makes individual sample reception unreliable — a single good sample
// needs to bridge the dwell window even if the next several are
// dropped. Trade-off: peer-actually-gone takes this long to detect
// before the LEAVING release dwell starts counting.
constexpr uint32_t SAMPLE_STALE_MS = 3000;

// Absolute watchdog — even with the "fresh sample required to demote"
// policy, force-demote APPROACHING back toward OUT_OF_RANGE after this
// long with no samples at all. Without this, a peer that vanishes
// silently latches us into APPROACHING forever.
constexpr uint32_t ABSOLUTE_STALE_MS = 12000;

// Same idea but longer for IN_PROXIMITY -> LEAVING. Once we're already
// IN_PROXIMITY we know the peer was definitely close; sample droughts
// here are usually transient (BLE+WiFi coex storms during HA WS
// reconnects, weak-WiFi retransmit bursts) rather than the peer
// actually leaving. The scan auto-rearm cycle recovers within ~10 s,
// so 30 s gives 2-3 chances to catch a fresh sample before
// firing the spurious LEAVING -> /api/proximity/lost POST that the
// companion would then have to undo.
constexpr uint32_t IN_PROXIMITY_STALE_MS = 30000;

// ---- Conversation session safety ------------------------------------------
// If no new /play arrives within this many seconds after we ack a
// playback_complete, treat the session as abandoned (companion crashed,
// network hiccup) and reset to idle. Defense-in-depth — the companion
// should normally drive end-of-session itself.
constexpr uint32_t SESSION_IDLE_TIMEOUT_SECONDS = 30;

// Max turns per session. After this many /play calls we ignore further
// ones for the session. Pairs with the companion's own end-of-session
// logic; this is a runaway-LLM safety net.
constexpr uint8_t  SESSION_MAX_TURNS = 20;

// ---- Outbound HTTP --------------------------------------------------------
// Timeout bumped from 2 s to 8 s to outlast the slowest expected server-
// side handler work. Even though the server now returns 200 immediately
// from playback_complete + dispatches in a worker thread, leaving margin
// here costs nothing. RETRIES=0 because POSTs aren't idempotent on the
// wire (server has its own (session_id, turn_index) dedup as defense in
// depth) — retrying on a request the server actually completed was the
// duplicate-playback_complete root cause.
constexpr uint32_t OUTBOUND_HTTP_TIMEOUT_MS = 8000;
constexpr uint8_t  OUTBOUND_HTTP_RETRIES    = 0;

// NVS key for the paired peer's BLE address (12 hex chars, no separators).
constexpr const char* NVS_KEY_PEER_MAC = "peer_mac";
// NVS key for the companion-app base URL override.
constexpr const char* NVS_KEY_COMPANION_URL = "companion_url";

}  // namespace robimon::services::proximity_config
