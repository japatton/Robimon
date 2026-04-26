// Generic text input modal with an on-screen QWERTY keyboard. Used by the
// WiFi password entry, and (later) by HA URL/token, Ollama URL, and the
// system prompt editor.
//
// Usage:
//
//   text_input::show("password for MyWifi", /*initial=*/"", /*masked=*/true,
//                     /*max_len=*/63,
//                     [](const char* result, bool cancelled) {
//                       if (!cancelled) wifi_mgr::connect(saved_ssid, result);
//                     });
//
// The screen pushes itself as a modal; on done/cancel it pops itself and
// invokes the callback exactly once. The callback runs synchronously in the
// gesture-handling loop so it's fine to do quick I/O (e.g., wifi_mgr::connect).

#pragma once

#include "../ui/screen_mgr.h"

#include <stddef.h>

namespace robimon::screens::text_input {

using Callback = void(*)(const char* result, bool cancelled);

void show(const char* title, const char* initial, bool masked,
          size_t max_len, Callback cb);

}  // namespace robimon::screens::text_input
