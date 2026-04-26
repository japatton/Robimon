// 4-digit PIN entry screen. Modal — pushed on top of whatever screen the
// long-press came from. On success, pushes the settings menu. On
// repeated failure (3 wrong tries), pops itself and returns to the
// carousel.
//
// Layout: masked PIN display at top ("• • • •" filling left→right as
// digits are entered), then a 3×4 grid of buttons for 0-9, ←, ✓.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class PinPadScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "pin"; }

 private:
  char     entered_[5] = {0};      // up to 4 digits + NUL
  uint8_t  entered_len_ = 0;
  uint8_t  fail_count_  = 0;
  bool     dirty_       = true;
  uint32_t flash_until_ms_ = 0;
  bool     flash_error_ = false;   // true=red error flash, false=normal
  uint32_t last_render_ms_ = 0;

  void try_submit();
  void render();
};

extern PinPadScreen pin_pad_screen;

}  // namespace robimon::screens
