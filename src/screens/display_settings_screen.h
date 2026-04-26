// Display settings — brightness slider + idle dim/sleep timeout pickers.
// Brightness applies live as you adjust; timeouts get persisted to NVS and
// are read by the (forthcoming) idle manager.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class DisplaySettingsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "display"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern DisplaySettingsScreen display_settings_screen;

}  // namespace robimon::screens
