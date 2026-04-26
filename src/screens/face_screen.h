// FaceScreen — thin wrapper around the face module so the screen manager
// can host it as one of the swipeable screens.

#pragma once

#include "../ui/screen_mgr.h"

namespace robimon::screens {

class FaceScreen : public ::robimon::ui::Screen {
 public:
  void update(uint32_t now_ms) override;
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "face"; }
};

extern FaceScreen face_screen;   // singleton; wire via &face_screen

}  // namespace robimon::screens
