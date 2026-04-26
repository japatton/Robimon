#include "power.h"
#include "board.h"
#include "../app/log.h"

#include <Wire.h>

// XPowersLib needs this define before include; we set it project-wide via
// the chip macro in pin_config-style #defines, but XPowersLib expects the
// constant in this exact form.
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

namespace robimon::hal::power {

namespace {
constexpr const char* TAG = "pmic";
XPowersAXP2101 s_pmu;
bool           s_ok = false;
}  // namespace

bool begin() {
  using namespace robimon::board;
  // XPowersLib's begin re-uses an existing Wire — we already called Wire.begin()
  // in main, so passing -1/-1 for sda/scl avoids the "bus already initialized" warning.
  if (!s_pmu.begin(Wire, PMIC_I2C_ADDR, -1, -1)) {
    LOG_E(TAG, "AXP2101 not responding @ 0x%02X", PMIC_I2C_ADDR);
    return false;
  }

  // Enable the on-chip ADC channels we read (battery V/I, VBUS V, die temp).
  // Skipping these means battery_volts() etc. return zeros.
  s_pmu.enableBattDetection();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableVbusVoltageMeasure();
  s_pmu.enableSystemVoltageMeasure();
  s_pmu.enableTemperatureMeasure();

  // Cap the charge current at a kid-friendly 500 mA — the cell connector is
  // sized for small batteries (~500-1000 mAh typical for MX1.25 wearables).
  s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  // 4.2 V termination is the default for single-cell Li-ion; explicit for clarity.
  s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

  // Enable the IRQs we care about — PWRKEY short-press is the only one we
  // act on today. Charge-done / battery-low can be added when we wire the
  // event bus.
  s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ
                | XPOWERS_AXP2101_BAT_INSERT_IRQ
                | XPOWERS_AXP2101_BAT_REMOVE_IRQ
                | XPOWERS_AXP2101_VBUS_INSERT_IRQ
                | XPOWERS_AXP2101_VBUS_REMOVE_IRQ
                | XPOWERS_AXP2101_BAT_CHG_DONE_IRQ);
  s_pmu.clearIrqStatus();

  s_ok = true;
  LOG_I(TAG, "AXP2101 up @ 0x%02X chip-id=0x%02X", PMIC_I2C_ADDR, s_pmu.getChipID());
  return true;
}

Snapshot snapshot() {
  Snapshot s{};
  if (!s_ok) {
    s.battery_percent = 0xFF;
    s.charge_state = ChargeState::UNKNOWN;
    return s;
  }

  s.battery_percent = s_pmu.getBatteryPercent();
  s.battery_volts   = s_pmu.getBattVoltage() / 1000.0f;
  s.vbus_volts      = s_pmu.getVbusVoltage() / 1000.0f;
  s.die_temp_c      = s_pmu.getTemperature();
  s.vbus_present    = s_pmu.isVbusIn();
  s.is_charging     = s_pmu.isCharging();

  // AXP2101 charge-status register decodes into one of: TRI/PRE/CC/CV/DONE/STOP.
  // XPowersLib exposes raw enums; we collapse them into our smaller set.
  switch (s_pmu.getChargerStatus()) {
    case XPOWERS_AXP2101_CHG_TRI_STATE:
    case XPOWERS_AXP2101_CHG_STOP_STATE: s.charge_state = ChargeState::NOT_CHARGING; break;
    case XPOWERS_AXP2101_CHG_PRE_STATE:  s.charge_state = ChargeState::PRE_CHARGE;   break;
    case XPOWERS_AXP2101_CHG_CC_STATE:
    case XPOWERS_AXP2101_CHG_CV_STATE:   s.charge_state = ChargeState::FAST_CHARGE;  break;
    case XPOWERS_AXP2101_CHG_DONE_STATE: s.charge_state = ChargeState::DONE;         break;
    default:                             s.charge_state = ChargeState::UNKNOWN;      break;
  }
  return s;
}

bool poll_irq_pwrkey_pressed() {
  if (!s_ok) return false;
  s_pmu.getIrqStatus();
  const bool pressed = s_pmu.isPekeyShortPressIrq();
  s_pmu.clearIrqStatus();
  return pressed;
}

bool ok() { return s_ok; }

}  // namespace robimon::hal::power
