// Power HAL — AXP2101 PMIC over I2C.
// Owns: battery percentage / voltage, charging state, USB-present state,
// die temperature, and PWRKEY events. Charging from USB happens
// automatically while the device runs; this module just observes.

#pragma once

#include <stdint.h>

namespace robimon::hal::power {

enum class ChargeState : uint8_t {
  UNKNOWN,
  NOT_CHARGING,
  PRE_CHARGE,
  FAST_CHARGE,
  DONE,
  FAULT,
};

struct Snapshot {
  uint8_t      battery_percent;   // 0..100, 0xFF if unknown
  float        battery_volts;
  float        vbus_volts;        // 0 if USB not plugged in
  float        die_temp_c;
  bool         vbus_present;
  bool         is_charging;
  ChargeState  charge_state;
};

bool begin();

// Reads current state from the PMIC. Cheap (a few I2C reads); call from
// the main loop or a dedicated stats task.
Snapshot snapshot();

// Service AXP2101 IRQ. Called when the I/O expander reports AXP_IRQ low.
// Returns true if the IRQ was a PWRKEY short-press event.
bool poll_irq_pwrkey_pressed();

bool ok();

}  // namespace robimon::hal::power
