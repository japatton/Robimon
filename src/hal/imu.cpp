#include "imu.h"
#include "board.h"
#include "../app/log.h"

#include <Wire.h>
#include <SensorQMI8658.hpp>

namespace robimon::hal::imu {

namespace {
constexpr const char* TAG = "imu";
SensorQMI8658 s_qmi;
bool          s_ok = false;
}  // namespace

bool begin() {
  using namespace robimon::board;

  // Try the strap-default address first (silkscreen says 0x6B), fall back to 0x6A.
  // SensorLib's begin signature: (Wire, addr, sda=-1, scl=-1) — pass -1 to skip
  // re-init of the I2C bus we already brought up in main.
  uint8_t addr = IMU_I2C_ADDR;
  if (!s_qmi.begin(Wire, addr, -1, -1)) {
    addr = (IMU_I2C_ADDR == 0x6B) ? 0x6A : 0x6B;
    if (!s_qmi.begin(Wire, addr, -1, -1)) {
      LOG_E(TAG, "QMI8658 not responding at 0x6A or 0x6B");
      return false;
    }
  }

  // Configure both sensors at moderate ranges suitable for a fidget-pickup-shake
  // device. ±4 g and ±256 dps with low-pass filtering keeps the data clean.
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz,
                            SensorQMI8658::LPF_MODE_0);
  s_qmi.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS,
                        SensorQMI8658::GYR_ODR_896_8Hz,
                        SensorQMI8658::LPF_MODE_3);

  s_qmi.enableAccelerometer();
  s_qmi.enableGyroscope();

  s_ok = true;
  LOG_I(TAG, "QMI8658 up @ 0x%02X (chip-id 0x%02X)", addr, s_qmi.getChipID());
  return true;
}

bool read(Sample& out) {
  if (!s_ok) return false;
  if (!s_qmi.getDataReady()) return false;

  IMUdata acc{}, gyr{};
  if (!s_qmi.getAccelerometer(acc.x, acc.y, acc.z)) return false;
  if (!s_qmi.getGyroscope(gyr.x, gyr.y, gyr.z))    return false;
  out.ax = acc.x; out.ay = acc.y; out.az = acc.z;
  out.gx = gyr.x; out.gy = gyr.y; out.gz = gyr.z;
  out.temp_c = s_qmi.getTemperature_C();
  return true;
}

void enable_motion_wake(float threshold_g) {
  if (!s_ok) return;
  // QMI8658 supports any-motion event with per-axis thresholds. INT2 is
  // already routed to GPIO 21 by the board; we just need to enable the event
  // and make sure the chip drives INT2 instead of INT1.
  // TODO: SensorLib's QMI8658 driver exposes configMotion() but the API is
  // verbose; punting on wiring this until the screen-manager idle path needs it.
  (void)threshold_g;
  pinMode(robimon::board::IMU_INT2, INPUT);
}

bool ok() { return s_ok; }

}  // namespace robimon::hal::imu
