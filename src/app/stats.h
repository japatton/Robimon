// Runtime stats reporter: free heap, free PSRAM, FPS, uptime, WiFi RSSI.
// Published over Serial periodically and surfaced on the diagnostics screen.

#pragma once

#include <stdint.h>

namespace robimon::stats {

struct Snapshot {
  uint32_t uptime_ms;
  uint32_t free_heap_bytes;
  uint32_t min_free_heap_bytes;
  uint32_t free_psram_bytes;
  uint32_t fps;          // averaged over the last sampling window
  int32_t  wifi_rssi;    // 0 if WiFi not connected
};

// Call once from app setup.
void begin();

// Call from the LVGL flush callback (or any per-frame point) to count frames.
void note_frame();

// Returns the most recent snapshot.
Snapshot last();

// Forces an immediate stats print to Serial. Otherwise stats print every 5 s.
void print_now();

// Auto-throttled tick; call from the main loop. Prints every 5 s.
void tick();

}  // namespace robimon::stats
