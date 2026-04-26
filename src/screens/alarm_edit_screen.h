// Alarm edit screen — edits one alarm. Pushed by alarms_settings_screen with
// either an existing alarm index (edit) or -1 (add new).

#pragma once

#include "../ui/screen_mgr.h"
#include "../services/alarm_mgr.h"

namespace robimon::screens {

class AlarmEditScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "alarm-edit"; }

  // Configured before push:
  int                                edit_idx_ = -1;   // -1 = new alarm
  ::robimon::services::alarm_mgr::Alarm staging_ = {};

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;

  void render();
};

extern AlarmEditScreen alarm_edit_screen;

}  // namespace robimon::screens
