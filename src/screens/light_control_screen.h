// Light control modal — pushed when the user taps a light entity card.
// Shows on/off, brightness slider, and color-temp slider. Other capabilities
// (HSV/RGB color, effects) are deferred — picking colors on a small round
// touchscreen needs more design work and the tap-to-toggle + brightness +
// warm/cool covers ~90 % of common light interactions.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class LightControlScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "light"; }

  // Configured before push.
  char entity_id_[64] = {0};

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern LightControlScreen light_control_screen;

}  // namespace robimon::screens
