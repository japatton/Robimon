// Single source of truth for ESP32-S3 GPIO assignments on the
// Waveshare ESP32-S3-Touch-AMOLED-1.75 (standard, non-C variant).
// Cross-reference: docs/pinmap.md. If you edit either, edit both.

#pragma once

#include <stdint.h>

namespace robimon::board {

// ---- LCD (CO5300, QSPI) ---------------------------------------------------
constexpr int      LCD_WIDTH        = 466;
constexpr int      LCD_HEIGHT       = 466;
constexpr int      LCD_QSPI_D0      = 4;
constexpr int      LCD_QSPI_D1      = 5;
constexpr int      LCD_QSPI_D2      = 6;
constexpr int      LCD_QSPI_D3      = 7;
constexpr int      LCD_QSPI_SCLK    = 38;
constexpr int      LCD_CS           = 12;
constexpr int      LCD_RESET        = 39;
constexpr int      LCD_TE           = 13;   // tearing-effect sync; not in stock examples but present per schematic
// CO5300 has a 6-pixel column window offset baked into its address registers.
// Arduino_CO5300 takes this as the col_offset1 ctor parameter; pass 6.
constexpr int      LCD_COL_OFFSET   = 6;

// ---- Touch (CST9217) ------------------------------------------------------
constexpr int      TP_INT           = 11;
constexpr int      TP_RESET         = 40;
constexpr uint8_t  TP_I2C_ADDR      = 0x5A;

// ---- Shared I2C bus -------------------------------------------------------
constexpr int      I2C_SDA          = 15;
constexpr int      I2C_SCL          = 14;
constexpr uint32_t I2C_FREQ_HZ      = 400000;

// ---- I2S audio (single peripheral, full-duplex; codec out + mic in share BCLK/WS)
constexpr int      I2S_MCLK         = 42;   // pin_config.h's I2S_MCK_IO 16 macro is vestigial; ES8311 example uses 42
constexpr int      I2S_BCLK         = 9;
constexpr int      I2S_WS           = 45;   // LRCK
constexpr int      I2S_DOUT         = 8;    // ESP32 -> ES8311 (speaker)
constexpr int      I2S_DIN          = 10;   // ES8311/ES7210 -> ESP32 (mic)
constexpr int      AUDIO_PA_CTRL    = 46;   // NS4150B amp enable; HIGH = unmute
constexpr uint8_t  ES8311_I2C_ADDR  = 0x18;
constexpr uint8_t  ES7210_I2C_ADDR  = 0x40;

// ---- IMU (QMI8658) --------------------------------------------------------
constexpr uint8_t  IMU_I2C_ADDR     = 0x6B;  // silkscreen labels U3 as 0x6B; probe 0x6A as fallback
constexpr int      IMU_INT2         = 21;    // direct ESP32 GPIO -> wake-on-motion path
// IMU_INT1 lives on TCA9554 EXIO6 (see ioexp::Pin::IMU_INT1)

// ---- RTC (PCF85063) -------------------------------------------------------
constexpr uint8_t  RTC_I2C_ADDR     = 0x51;
// RTC alarm INT lives on TCA9554 EXIO3 (see ioexp::Pin::RTC_INT)

// ---- PMIC (AXP2101) -------------------------------------------------------
constexpr uint8_t  PMIC_I2C_ADDR    = 0x34;
// PMIC IRQ lives on TCA9554 EXIO5 (see ioexp::Pin::AXP_IRQ)

// ---- I/O expander (TCA9554PWR) -------------------------------------------
constexpr uint8_t  IOEXP_I2C_ADDR   = 0x20;  // default A0/A1/A2 = 0/0/0

// ---- microSD / TF (1-bit SDMMC) ------------------------------------------
constexpr int      SD_CLK           = 2;
constexpr int      SD_CMD           = 1;
constexpr int      SD_DATA0         = 3;
constexpr int      SD_CS            = 41;   // only used in SPI fallback mode

// ---- Buttons --------------------------------------------------------------
constexpr int      BTN_BOOT         = 0;    // direct GPIO; doubles as user button at runtime
// PWR button (Key2) routes through the AXP2101 PWRKEY input; events arrive over I2C.

// ---- Expansion header H2 (silkscreen IO18 IO17 IO16 RXD TXD 3V3 GND VBUS)
constexpr int      H2_IO18          = 18;   // shared with onboard GNSS RX (depopulate R16 to free)
constexpr int      H2_IO17          = 17;   // shared with onboard GNSS TX (depopulate R15 to free)
constexpr int      H2_IO16          = 16;   // always-free GPIO

// ---- Onboard GNSS (LC76G) -- not in product spec; here for completeness
constexpr int      GNSS_UART_TX     = H2_IO18;  // ESP32 -> LC76G
constexpr int      GNSS_UART_RX     = H2_IO17;  // LC76G -> ESP32
// GNSS reset lives on TCA9554 EXIO7

}  // namespace robimon::board
