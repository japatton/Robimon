// HA settings — URL, token, default-entity-id, connection status.
// Push from settings menu's "ha" tile.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class HaSettingsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "ha"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern HaSettingsScreen ha_settings_screen;

}  // namespace robimon::screens
