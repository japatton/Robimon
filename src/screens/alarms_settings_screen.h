// Alarms settings — list of configured alarms with add/edit/remove.
// Pushes alarm_edit_screen for editing a specific alarm or adding a new one.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class AlarmsSettingsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "alarms"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern AlarmsSettingsScreen alarms_settings_screen;

}  // namespace robimon::screens
