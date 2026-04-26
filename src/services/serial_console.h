// Tiny line-buffered serial console over USB-CDC. Lets the user paste long
// values (HA tokens, URLs, system prompts) without typing them on the
// on-screen keyboard.
//
// Commands:
//   help
//   set <key> <value>     // value may contain spaces
//   get <key>             // ha_token is masked when printed
//   forget <key>          // clears the NVS key
//   list                  // lists known keys
//   ha-reconnect          // forces HA to retry with current creds
//   status                // prints WiFi + HA status
//
// Tokens echo back on the terminal — that's fine since the user is the only
// one looking at their session. If they want no-echo entry, they can type
// `secret` first to enter a future no-echo mode (not implemented yet).

#pragma once

#include <stdint.h>

namespace robimon::services::serial_console {

void begin();
void update(uint32_t now_ms);

}  // namespace robimon::services::serial_console
