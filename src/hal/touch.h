// Touch HAL — CST9217 capacitive touch over I2C.
// Reports up to 5 simultaneous points; we mainly use the first one.

#pragma once

#include <stdint.h>

namespace robimon::hal::touch {

struct Point {
  int16_t x;
  int16_t y;
};

bool begin();

// Reads current touch state. Returns the number of active points
// (0..5). out_points must hold at least max_points entries.
int read(Point* out_points, int max_points);

}  // namespace robimon::hal::touch
