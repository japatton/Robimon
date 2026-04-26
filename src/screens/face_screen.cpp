#include "face_screen.h"
#include "../face/face.h"

namespace robimon::screens {

FaceScreen face_screen;

void FaceScreen::update(uint32_t /*now_ms*/) {
  ::robimon::face::update();
}

void FaceScreen::on_tap(int panel_x, int panel_y) {
  ::robimon::face::on_tap(panel_x, panel_y);
}

}  // namespace robimon::screens
