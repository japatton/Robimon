#include "ha_entity_screen.h"
#include "light_control_screen.h"
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

constexpr uint16_t COLOR_BG       = 0x0000;
constexpr uint16_t COLOR_FG       = 0x055F;
constexpr uint16_t COLOR_DIM      = 0x01F6;
constexpr uint16_t COLOR_OK       = 0x07E0;
constexpr uint16_t COLOR_OFFLINE  = 0xF800;
constexpr uint16_t COLOR_FLASH    = 0xFFFF;   // white briefly to confirm tap

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;

constexpr uint32_t FLASH_MS = 250;

uint32_t s_flash_until_ms = 0;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}

// Returns the part of entity_id before the dot, or empty string if none.
// Caller-supplied buffer must hold at least 32 chars.
void split_domain(const char* entity_id, char* out_domain, size_t max) {
  out_domain[0] = '\0';
  if (!entity_id) return;
  const char* dot = strchr(entity_id, '.');
  if (!dot) return;
  const size_t n = (size_t)(dot - entity_id);
  if (n + 1 >= max) return;
  memcpy(out_domain, entity_id, n);
  out_domain[n] = '\0';
}

// For each domain, the service that "tap to control" should send. nullptr if
// we don't have a sensible default for that domain (sensors, etc.).
const char* default_service_for(const char* domain) {
  if (!domain || !*domain) return nullptr;
  if (strcmp(domain, "light")          == 0) return "toggle";
  if (strcmp(domain, "switch")         == 0) return "toggle";
  if (strcmp(domain, "fan")            == 0) return "toggle";
  if (strcmp(domain, "input_boolean")  == 0) return "toggle";
  if (strcmp(domain, "media_player")   == 0) return "media_play_pause";
  if (strcmp(domain, "cover")          == 0) return "toggle";
  if (strcmp(domain, "automation")     == 0) return "trigger";
  if (strcmp(domain, "script")         == 0) return "turn_on";
  if (strcmp(domain, "scene")          == 0) return "turn_on";
  return nullptr;
}

}  // namespace

void HaEntityScreen::on_appear() {
  ::robimon::services::config::get_string("ha_entity",
                                            entity_id_, sizeof(entity_id_));
  if (entity_id_[0]) {
    ::robimon::services::ha_client::track_entity(entity_id_);
  }
  dirty_ = true;
}

void HaEntityScreen::update(uint32_t now_ms) {
  // Auto-clear the tap-confirm flash.
  if (s_flash_until_ms != 0 && now_ms >= s_flash_until_ms) {
    s_flash_until_ms = 0;
    dirty_ = true;
  }
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
  draw_centered_text(g, 12, 2, state_color, connected ? "online" : "offline");

  // Entity card
  const auto* st = ha_client::get_state(entity_id_);
  const char* name  = (st && st->friendly_name[0]) ? st->friendly_name : entity_id_;
  const char* value = (st && st->state[0])         ? st->state         : "—";

  // Brief tap-confirm flash overrides the value color for ~250 ms.
  const uint16_t value_color = (s_flash_until_ms != 0) ? COLOR_FLASH : COLOR_FG;

  draw_centered_text(g, 70,  3, COLOR_FG,    name);
  draw_centered_text(g, 120, 4, value_color, value);

  if (st) {
    const uint32_t age_s = (millis() - st->received_ms) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lus ago", (unsigned long)age_s);
    draw_centered_text(g, 200, 2, COLOR_DIM, buf);
  } else if (connected) {
    draw_centered_text(g, 200, 2, COLOR_DIM, "(waiting for data)");
  }

  // Hint at the bottom: if this domain is controllable, tell the user.
  char domain[32]; split_domain(entity_id_, domain, sizeof(domain));
  const char* svc = default_service_for(domain);
  if (connected) {
    if (strcmp(domain, "light") == 0) {
      draw_centered_text(g, 240, 2, COLOR_DIM, "tap to control");
    } else if (svc) {
      char hint[40];
      snprintf(hint, sizeof(hint), "tap to %s", svc);
      draw_centered_text(g, 240, 2, COLOR_DIM, hint);
    }
  }

  // Footer with entity id (small, dim)
  draw_centered_text(g, 290, 1, COLOR_DIM, entity_id_);

  ::robimon::hal::display::flush();
}

void HaEntityScreen::on_tap(int /*x*/, int /*y*/) {
  using namespace ::robimon::services;

  if (!entity_id_[0]) return;
  if (ha_client::state() != ha_client::State::CONNECTED) {
    LOG_W(TAG, "tap ignored — HA not connected");
    return;
  }

  char domain[32]; split_domain(entity_id_, domain, sizeof(domain));

  // Lights get a richer control modal (on/off + brightness + color temp)
  // instead of a one-shot toggle. Other domains still get tap-to-toggle.
  if (strcmp(domain, "light") == 0) {
    strncpy(light_control_screen.entity_id_, entity_id_,
            sizeof(light_control_screen.entity_id_) - 1);
    light_control_screen.entity_id_[sizeof(light_control_screen.entity_id_) - 1] = '\0';
    ::robimon::ui::screen_mgr::push_modal(&light_control_screen);
    return;
  }

  const char* svc = default_service_for(domain);
  if (!svc) {
    LOG_I(TAG, "tap on %s — no default service for domain '%s'", entity_id_, domain);
    return;
  }
  if (!ha_client::call_service(domain, svc, entity_id_)) {
    LOG_W(TAG, "call_service failed");
    return;
  }
  // Brief on-screen confirmation. The state value color flashes white;
  // the actual new state will arrive via the state_changed subscription
  // and replace it within a few hundred ms anyway.
  s_flash_until_ms = millis() + FLASH_MS;
  dirty_ = true;
}

}  // namespace robimon::screens
