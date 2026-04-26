#include "io_expander.h"
#include "board.h"
#include "../app/log.h"

#include <Wire.h>

namespace robimon::hal::ioexp {

namespace {
constexpr const char* TAG = "ioexp";

// TCA9554 register map
constexpr uint8_t REG_INPUT      = 0x00;
constexpr uint8_t REG_OUTPUT     = 0x01;
constexpr uint8_t REG_POLARITY   = 0x02;
constexpr uint8_t REG_CONFIG     = 0x03;  // 1 = input, 0 = output

// Initial direction: SYS_OUT (P4) and GPS_RST (P7) are outputs; the rest are inputs.
// Bit pattern: P7..P0
//   1 = input,  0 = output
//   inputs: P0,P1,P2,P3,P5,P6 → 0b01101111
constexpr uint8_t INITIAL_CONFIG = 0b01101111;

// Initial output state for the two output pins:
//   SYS_OUT (P4) HIGH — keeps AXP2101 powered on
//   GPS_RST (P7) HIGH — releases GNSS module from reset (we don't use GNSS today,
//                       but holding it in reset would draw a tiny current spike on every wakeup)
constexpr uint8_t INITIAL_OUTPUT = (1 << 4) | (1 << 7);

bool s_ok = false;
uint8_t s_output_shadow = INITIAL_OUTPUT;

bool write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(robimon::board::IOEXP_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

uint8_t read_reg(uint8_t reg) {
  Wire.beginTransmission(robimon::board::IOEXP_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)robimon::board::IOEXP_I2C_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

}  // namespace

bool begin() {
  if (!write_reg(REG_OUTPUT, INITIAL_OUTPUT)) {
    LOG_E(TAG, "TCA9554 not responding @ 0x%02X", robimon::board::IOEXP_I2C_ADDR);
    return false;
  }
  if (!write_reg(REG_POLARITY, 0x00)) return false;
  if (!write_reg(REG_CONFIG, INITIAL_CONFIG)) return false;
  s_output_shadow = INITIAL_OUTPUT;
  s_ok = true;
  LOG_I(TAG, "TCA9554 up @ 0x%02X cfg=0x%02X out=0x%02X",
        robimon::board::IOEXP_I2C_ADDR, INITIAL_CONFIG, INITIAL_OUTPUT);
  return true;
}

bool write(Pin p, bool high) {
  if (!s_ok) return false;
  const uint8_t bit = 1u << static_cast<uint8_t>(p);
  if (high) s_output_shadow |= bit;
  else      s_output_shadow &= ~bit;
  return write_reg(REG_OUTPUT, s_output_shadow);
}

bool read(Pin p) {
  if (!s_ok) return false;
  const uint8_t v = read_reg(REG_INPUT);
  if (v == 0xFF) return false;  // bus error or all-high; caller can disambiguate via read_all()
  return (v >> static_cast<uint8_t>(p)) & 1u;
}

uint8_t read_all() {
  if (!s_ok) return 0xFF;
  return read_reg(REG_INPUT);
}

bool ok() { return s_ok; }

}  // namespace robimon::hal::ioexp
