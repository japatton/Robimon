// TCA9554PWR I/O expander wrapper.
// Aggregates the RTC alarm, AXP2101 IRQ, IMU INT1, GPS reset, and
// the SYS_OUT power-down gate behind one I2C address (0x20). Inputs
// are polled; we don't currently wire the expander's own INT line.

#pragma once

#include <stdint.h>

namespace robimon::hal::ioexp {

// Names match the schematic's silkscreen. P0..P7 in datasheet order.
enum class Pin : uint8_t {
  EXIO0 = 0,  // test pad EX0 — spare
  EXIO1 = 1,  // test pad EX1 — spare
  EXIO2 = 2,  // test pad EX2 — spare
  RTC_INT,    // EXIO3 — input from PCF85063 alarm
  SYS_OUT,    // EXIO4 — output to PWR-on gate (drive low to power-down)
  AXP_IRQ,    // EXIO5 — input from AXP2101 IRQ
  IMU_INT1,   // EXIO6 — input from QMI8658 INT1
  GPS_RST,    // EXIO7 — output to LC76G GNSS reset (active low)
};

bool begin();

// Set or read a pin. Reads return false on bus error.
bool write(Pin p, bool high);
bool read(Pin p);

// Read all 8 input bits in one shot. Returns 0xFF on bus error.
uint8_t read_all();

// Diagnostics: returns true if begin() succeeded and the chip is responding.
bool ok();

}  // namespace robimon::hal::ioexp
