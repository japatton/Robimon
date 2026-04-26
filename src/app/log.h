// Logging macros. Compile-time level switch via -DROBIMON_LOG_LEVEL=N.
//   0 = none, 1 = error, 2 = warn, 3 = info, 4 = debug
//
// All output goes to Serial (USB-CDC). In production the kid-facing UI
// never surfaces these; the diagnostics screen will scrape a separate
// in-memory ring buffer that we add in services/log_ring.cpp later.

#pragma once

#include <Arduino.h>

#ifndef ROBIMON_LOG_LEVEL
#define ROBIMON_LOG_LEVEL 3
#endif

namespace robimon::log {

inline void prefix(const char* level, const char* tag) {
  Serial.printf("[%9lu] %s %-6s ", (unsigned long)millis(), level, tag);
}

}  // namespace robimon::log

#define LOG_E(tag, fmt, ...) do { if (ROBIMON_LOG_LEVEL >= 1) { ::robimon::log::prefix("E", tag); Serial.printf((fmt), ##__VA_ARGS__); Serial.println(); } } while (0)
#define LOG_W(tag, fmt, ...) do { if (ROBIMON_LOG_LEVEL >= 2) { ::robimon::log::prefix("W", tag); Serial.printf((fmt), ##__VA_ARGS__); Serial.println(); } } while (0)
#define LOG_I(tag, fmt, ...) do { if (ROBIMON_LOG_LEVEL >= 3) { ::robimon::log::prefix("I", tag); Serial.printf((fmt), ##__VA_ARGS__); Serial.println(); } } while (0)
#define LOG_D(tag, fmt, ...) do { if (ROBIMON_LOG_LEVEL >= 4) { ::robimon::log::prefix("D", tag); Serial.printf((fmt), ##__VA_ARGS__); Serial.println(); } } while (0)
