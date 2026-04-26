// Home Assistant WebSocket client.
//
// Connects to a HA instance, authenticates with a long-lived access token,
// subscribes to entity state updates, and caches them locally so screens
// can read state synchronously without waiting on the network.
//
// Auto-reconnects with backoff if the WS drops. Screens treat missing-or-
// stale state as "offline" and render a friendly indicator (per the kid-
// safe spec — never a stack trace).
//
// Stage H ships with WebSocket support only. MQTT fallback (per the spec)
// is deferred — HA WebSocket reliably works on the same LAN, which is the
// only setup the user has today.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace robimon::services::ha_client {

enum class State : uint8_t {
  DISCONNECTED,
  CONNECTING,
  AUTHENTICATING,
  CONNECTED,
  FAILED,
};

constexpr int    MAX_TRACKED_ENTITIES = 8;
constexpr size_t MAX_ENTITY_ID_LEN    = 63;
constexpr size_t MAX_STATE_LEN        = 31;
constexpr size_t MAX_NAME_LEN         = 31;

struct EntityState {
  char     entity_id[MAX_ENTITY_ID_LEN + 1];
  char     friendly_name[MAX_NAME_LEN + 1];
  char     state[MAX_STATE_LEN + 1];
  uint32_t received_ms;       // millis() when this state was received
  bool     valid;
};

void begin();
void update(uint32_t now_ms);

State        state();
const char*  state_str();
const char*  url();

bool connect(const char* url, const char* token);
void disconnect();
void forget();
bool auto_connect();

// Tracking: ask the client to keep state for this entity. Cached state can be
// read via get_state() any time. Calling track_entity() for an entity already
// tracked is a no-op.
bool track_entity(const char* entity_id);

// Returns nullptr if not tracked or no state received yet.
const EntityState* get_state(const char* entity_id);

}  // namespace robimon::services::ha_client
