// Time settings — modal screen pushed from settings.
//
// Shows the current local time + sync status + active timezone, then a list
// of timezone presets. Tap a preset to switch; the choice is saved to NVS
// and applied immediately. Manual time entry fallback comes later (for now,
// if there's no NTP, the time just shows as "not synced").

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class TimeSettingsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "time"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern TimeSettingsScreen time_settings_screen;

}  // namespace robimon::screens
