// AlarmsScreen — placeholder for stage E. Renders a header and the list
// of configured alarms (none for now). Gets the real add/edit/dismiss UI
// when the alarm manager lands in stage G.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class AlarmsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "alarms"; }

 private:
  bool     dirty_           = true;
  uint32_t last_render_ms_  = 0;
  uint32_t appeared_ms_     = 0;   // when this screen became visible
};

extern AlarmsScreen alarms_screen;

}  // namespace robimon::screens
