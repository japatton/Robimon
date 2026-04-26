// Settings menu — top-level list of sections. Pushed on top of the
// carousel after PIN success. Each section is a stub for now; they get
// filled in over stages F-2..F-4. Tapping a section logs which one was
// chosen and (later) pushes that section's screen.
//
// Layout: vertical list, scrollable later if more than 6 sections. For
// stage F-1 we pick a tight grid that fits without scrolling.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class SettingsMenuScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "settings"; }

 private:
  bool     dirty_ = true;
  uint32_t last_render_ms_ = 0;
};

extern SettingsMenuScreen settings_menu_screen;

}  // namespace robimon::screens
