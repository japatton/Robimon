// IMU HAL — QMI8658 6-axis (3-axis accel + 3-axis gyro) over I2C.
// INT2 is wired direct to ESP32 GPIO 21, which is what we use for
// wake-on-motion. INT1 lives on the I/O expander and is currently unused.

#pragma once

#include <stdint.h>

namespace robimon::hal::imu {

struct Sample {
  float ax, ay, az;   // accel, g
  float gx, gy, gz;   // gyro,  deg/s
  float temp_c;
};

bool begin();

// Read the latest accel + gyro. Returns false if a sample isn't ready.
bool read(Sample& out);

// Configures any-motion detection and routes the event to INT2 (GPIO 21).
// Once enabled, GPIO 21 going LOW is a wake signal; the host should
// service it by reading qmi.getStatusRegister() (TODO: expose).
void enable_motion_wake(float threshold_g = 0.20f);

bool ok();

}  // namespace robimon::hal::imu
