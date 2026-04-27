// First-run setup portal. When the device boots without a "configured"
// flag in NVS, main.cpp routes here instead of the normal carousel +
// services. We host a SoftAP, a DNS server that answers every query
// with the AP IP (so phones detect a captive portal), and a minimal
// HTTP server that serves a single HTML page collecting:
//   - WiFi SSID + password (live scan)
//   - Timezone (preset list)
//   - HA URL + long-lived token + default entity (optional)
// Submit → save to NVS → mark configured → reboot into normal mode.

#pragma once

#include <stdint.h>

namespace robimon::services::setup_portal {

constexpr const char* AP_SSID = "Robimon-Setup";
constexpr const char* AP_IP   = "192.168.4.1";

void begin();
void update(uint32_t now_ms);

bool is_running();

}  // namespace robimon::services::setup_portal
