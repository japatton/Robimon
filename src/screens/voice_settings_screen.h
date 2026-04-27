// Voice/Ollama settings — URL of the local companion's /chat endpoint,
// Ollama model name, and system-prompt override. Push from settings →
// voice tile.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class VoiceSettingsScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "voice"; }

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;
};

extern VoiceSettingsScreen voice_settings_screen;

}  // namespace robimon::screens
