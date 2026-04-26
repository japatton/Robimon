// About — firmware version, board info, free memory, WiFi MAC, uptime,
// and an OTA update trigger (stub for now; OTA itself ships in a later
// stage).

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class AboutScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "about"; }

 private:
  bool     dirty_         = true;
  uint32_t last_render_ms_ = 0;
};

extern AboutScreen about_screen;

}  // namespace robimon::screens
