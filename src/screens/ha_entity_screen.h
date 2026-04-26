// Carousel HA screen — generic entity card. Reads the configured default
// entity from NVS at on_appear() and renders its friendly name + state +
// freshness. The pattern (one screen per entity) is what gets cloned for
// the chore chart, light controls, etc.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class HaEntityScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override;
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "ha-entity"; }

 private:
  bool      dirty_           = true;
  uint32_t  last_render_ms_  = 0;
  char      entity_id_[80]   = {0};
};

extern HaEntityScreen ha_entity_screen;

}  // namespace robimon::screens
