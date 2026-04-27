// On-device setup screen. Shown while the setup portal is hosting the
// SoftAP — instructs the user to connect to the AP and visit the URL in
// their browser. Once the form is submitted, the device reboots, so this
// screen never needs to handle "setup complete" — it just sits and waits.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class SetupScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override {}
  const char* name() const override { return "setup"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern SetupScreen setup_screen;

}  // namespace robimon::screens
