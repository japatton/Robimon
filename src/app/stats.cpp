#include "stats.h"
#include "log.h"

#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <WiFi.h>

namespace robimon::stats {

namespace {
constexpr uint32_t REPORT_INTERVAL_MS = 5000;

// std::atomic so the LVGL flush callback (Core 1) can bump this safely
// while the stats reporter (Core 0) reads it. C++20 deprecates ++ on a
// plain volatile — atomic is the correct fix anyway.
std::atomic<uint32_t> s_frame_count{0};
uint32_t              s_last_report_ms = 0;
Snapshot              s_last{};

void sample(Snapshot& out) {
  out.uptime_ms            = (uint32_t)(esp_timer_get_time() / 1000);
  out.free_heap_bytes      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  out.min_free_heap_bytes  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  out.free_psram_bytes     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  out.wifi_rssi            = WiFi.isConnected() ? WiFi.RSSI() : 0;
}

void report(const Snapshot& s) {
  LOG_I("stats",
        "up=%lus heap=%lu (min %lu) psram=%lu fps=%lu rssi=%ld",
        (unsigned long)(s.uptime_ms / 1000UL),
        (unsigned long)s.free_heap_bytes,
        (unsigned long)s.min_free_heap_bytes,
        (unsigned long)s.free_psram_bytes,
        (unsigned long)s.fps,
        (long)s.wifi_rssi);
}

}  // namespace

void begin() {
  s_last_report_ms = millis();
  sample(s_last);
}

void note_frame() {
  s_frame_count.fetch_add(1, std::memory_order_relaxed);
}

Snapshot last() {
  return s_last;
}

void print_now() {
  const uint32_t frames = s_frame_count.exchange(0, std::memory_order_relaxed);

  const uint32_t now = millis();
  const uint32_t elapsed_ms = now - s_last_report_ms;
  s_last_report_ms = now;

  Snapshot s;
  sample(s);
  s.fps = (elapsed_ms > 0) ? (frames * 1000UL / elapsed_ms) : 0;
  s_last = s;
  report(s);
}

void tick() {
  if ((millis() - s_last_report_ms) >= REPORT_INTERVAL_MS) {
    print_now();
  }
}

}  // namespace robimon::stats
