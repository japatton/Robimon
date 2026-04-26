#include "ha_entity_screen.h"
#include "../hal/display.h"
#include "../services/ha_client.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

HaEntityScreen ha_entity_screen;

namespace {
constexpr const char* TAG = "ha-ent";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_OFFLINE = 0xF800;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}

}  // namespace

void HaEntityScreen::on_appear() {
  // Re-read configured entity in case the user changed it in settings.
  ::robimon::services::config::get_string("ha_entity",
                                            entity_id_, sizeof(entity_id_));
  if (entity_id_[0]) {
    ::robimon::services::ha_client::track_entity(entity_id_);
  }
  dirty_ = true;
}

void HaEntityScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 500) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  using namespace ::robimon::services;

  // Empty-state: no entity configured.
  if (!entity_id_[0]) {
    draw_centered_text(g, 40, 3, COLOR_DIM, "no entity");
    draw_centered_text(g, 90, 2, COLOR_DIM, "set one in settings");
    draw_centered_text(g, 120, 2, COLOR_DIM, "ha section");
    ::robimon::hal::display::flush();
    return;
  }

  // Connection state header
  const auto cs = ha_client::state();
  const bool connected = (cs == ha_client::State::CONNECTED);
  const uint16_t state_color = connected ? COLOR_OK : COLOR_OFFLINE;
  // Friendly cloud-shaped offline indicator (just text for now; real icon
  // later). Kid-safe per spec — no IP / stack trace.
  draw_centered_text(g, 12, 2, state_color, connected ? "online" : "offline");

  // Entity card
  const auto* st = ha_client::get_state(entity_id_);
  const char* name  = (st && st->friendly_name[0]) ? st->friendly_name : entity_id_;
  const char* value = (st && st->state[0])         ? st->state         : "—";

  draw_centered_text(g, 70,  3, COLOR_FG, name);
  draw_centered_text(g, 120, 4, COLOR_FG, value);

  if (st) {
    const uint32_t age_s = (millis() - st->received_ms) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)age_s);
    draw_centered_text(g, 200, 2, COLOR_DIM, buf);
  } else if (connected) {
    draw_centered_text(g, 200, 2, COLOR_DIM, "(waiting for data)");
  }

  // Footer with entity id (small, dim)
  draw_centered_text(g, 290, 1, COLOR_DIM, entity_id_);

  ::robimon::hal::display::flush();
}

void HaEntityScreen::on_tap(int /*x*/, int /*y*/) {
  // No-op for the generic entity card; the pattern for "tap toggles light"
  // would override on_tap and call ha_client::call_service() (later stage).
}

}  // namespace robimon::screens
