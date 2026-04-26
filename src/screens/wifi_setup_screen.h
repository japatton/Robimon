// WiFi setup — modal screen pushed from settings.
//
// Shows current connection status (SSID + IP if connected, otherwise the
// WiFi state machine status), a "scan" button, the list of nearby networks
// from the most recent scan, and a "back" button. Tapping a network opens
// the text-input modal for the password (or connects directly if open).

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class WifiSetupScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "wifi"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
  // SSID we're currently going to connect to once the password modal returns.
  // Has to be a static-lifetime buffer because the text_input callback runs
  // after we've popped from the call stack.
  char      pending_ssid_[33] = {0};
};

extern WifiSetupScreen wifi_setup_screen;

}  // namespace robimon::screens
