#include "ha_entity_screen.h"
#include "light_control_screen.h"
#include "../hal/display.h"
#include "../services/ha_client.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

namespace robimon::screens {

HaEntityScreen ha_entity_screen;

namespace {
constexpr const char* TAG = "ha-ent";

constexpr uint16_t COLOR_BG       = 0x0000;
constexpr uint16_t COLOR_DIM      = 0x01F6;
constexpr uint16_t COLOR_CYAN     = 0x055F;   // off / inactive
constexpr uint16_t COLOR_WARM_ON  = 0xFEE0;   // warm amber for "on" state
constexpr uint16_t COLOR_OFFLINE  = 0xF800;
constexpr uint16_t COLOR_FLASH    = 0xFFFF;   // brief tap-confirm

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;

// Icon center / radius. Lives in the upper half of the canvas.
constexpr int ICON_CY = 120;
constexpr int ICON_R  = 56;

constexpr uint32_t FLASH_MS = 250;

uint32_t s_flash_until_ms = 0;

// ---------------------------------------------------------------------------
// String / domain helpers
// ---------------------------------------------------------------------------
void split_domain(const char* entity_id, char* out, size_t max) {
  out[0] = '\0';
  if (!entity_id) return;
  const char* dot = strchr(entity_id, '.');
  if (!dot) return;
  const size_t n = (size_t)(dot - entity_id);
  if (n + 1 >= max) return;
  memcpy(out, entity_id, n);
  out[n] = '\0';
}

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

// "is_on" heuristic — covers the common HA states.
bool state_is_on(const char* state) {
  if (!state || !*state) return false;
  return (strcmp(state, "on")        == 0)
      || (strcmp(state, "playing")   == 0)
      || (strcmp(state, "open")      == 0)
      || (strcmp(state, "active")    == 0)
      || (strcmp(state, "home")      == 0);
}

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color,
                        const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}

// ---------------------------------------------------------------------------
// Icon primitives — each domain gets a procedural icon. Drawn centered at
// (CENTER_X, ICON_CY) with overall size ICON_R. Color = "on" warm amber or
// "off" dim cyan; the kid reads state from color + shape, no words needed.
// ---------------------------------------------------------------------------
void draw_bulb(Arduino_GFX* g, uint16_t c) {
  g->fillCircle(CENTER_X, ICON_CY - 6, ICON_R - 6, c);
  // Screw base
  g->fillRect(CENTER_X - 18, ICON_CY + ICON_R - 14, 36, 14, c);
  g->fillRect(CENTER_X - 14, ICON_CY + ICON_R, 28, 6, c);
}

void draw_power(Arduino_GFX* g, uint16_t c) {
  // Power glyph — a ring with a vertical break + line through the top
  for (int t = -2; t <= 2; ++t) g->drawCircle(CENTER_X, ICON_CY, ICON_R - 8 + t, c);
  // mask the top break
  g->fillRect(CENTER_X - 6, ICON_CY - ICON_R, 12, ICON_R - 14, COLOR_BG);
  g->fillRect(CENTER_X - 5, ICON_CY - ICON_R + 4, 10, ICON_R - 12, c);
}

void draw_fan(Arduino_GFX* g, uint16_t c) {
  // Three blades pointing outward at 120°. Hub in center.
  for (int i = 0; i < 3; ++i) {
    const float a = -1.5707963f + i * 2.0944f;
    const float ax = cosf(a), ay = sinf(a);
    const int r = ICON_R - 6;
    const int tip_x = CENTER_X + (int)(ax * r);
    const int tip_y = ICON_CY  + (int)(ay * r);
    // Two side points perpendicular to the blade direction
    const int side1_x = CENTER_X + (int)((-ay) * 18);
    const int side1_y = ICON_CY  + (int)(( ax) * 18);
    const int side2_x = CENTER_X + (int)(( ay) * 18);
    const int side2_y = ICON_CY  + (int)((-ax) * 18);
    g->fillTriangle(side1_x, side1_y, side2_x, side2_y, tip_x, tip_y, c);
  }
  g->fillCircle(CENTER_X, ICON_CY, 14, COLOR_BG);
  g->fillCircle(CENTER_X, ICON_CY, 10, c);
}

void draw_speaker(Arduino_GFX* g, uint16_t c) {
  // Trapezoid speaker body
  const int sx = CENTER_X - 14, sy = ICON_CY - 24;
  g->fillRect(sx - 18, ICON_CY - 12, 18, 24, c);
  g->fillTriangle(sx, sy, sx, ICON_CY + 24, sx + 22, ICON_CY, c);
  // Sound arcs on the right
  g->drawCircle(CENTER_X + 18, ICON_CY, 10, c);
  g->drawCircle(CENTER_X + 18, ICON_CY, 18, c);
  g->drawCircle(CENTER_X + 18, ICON_CY,  6, c);
}

void draw_blinds(Arduino_GFX* g, uint16_t c) {
  for (int i = 0; i < 5; ++i) {
    const int y = ICON_CY - 30 + i * 14;
    g->fillRect(CENTER_X - 40, y, 80, 6, c);
  }
}

void draw_play(Arduino_GFX* g, uint16_t c) {
  g->fillTriangle(CENTER_X - 20, ICON_CY - 32,
                   CENTER_X - 20, ICON_CY + 32,
                   CENTER_X + 32, ICON_CY, c);
}

void draw_gear(Arduino_GFX* g, uint16_t c) {
  // Outer "teeth" via 8 small rectangles around a center circle
  for (int i = 0; i < 8; ++i) {
    const float a = i * (3.14159265f / 4.0f);
    const float ax = cosf(a), ay = sinf(a);
    const int x = CENTER_X + (int)(ax * (ICON_R - 4));
    const int y = ICON_CY  + (int)(ay * (ICON_R - 4));
    g->fillRect(x - 6, y - 6, 12, 12, c);
  }
  g->fillCircle(CENTER_X, ICON_CY, ICON_R - 10, c);
  g->fillCircle(CENTER_X, ICON_CY, ICON_R - 26, COLOR_BG);
}

void draw_sparkle(Arduino_GFX* g, uint16_t c) {
  // A 4-point star — used for "scene"
  g->fillTriangle(CENTER_X, ICON_CY - ICON_R + 6,
                   CENTER_X - 10, ICON_CY,
                   CENTER_X + 10, ICON_CY, c);
  g->fillTriangle(CENTER_X, ICON_CY + ICON_R - 6,
                   CENTER_X - 10, ICON_CY,
                   CENTER_X + 10, ICON_CY, c);
  g->fillTriangle(CENTER_X - ICON_R + 6, ICON_CY,
                   CENTER_X, ICON_CY - 10,
                   CENTER_X, ICON_CY + 10, c);
  g->fillTriangle(CENTER_X + ICON_R - 6, ICON_CY,
                   CENTER_X, ICON_CY - 10,
                   CENTER_X, ICON_CY + 10, c);
}

void draw_question(Arduino_GFX* g, uint16_t c) {
  // Generic "unknown entity type" — a big circle with "?" inside
  g->drawCircle(CENTER_X, ICON_CY, ICON_R - 6, c);
  g->drawCircle(CENTER_X, ICON_CY, ICON_R - 7, c);
  g->setTextColor(c);
  g->setTextSize(6);
  g->setCursor(CENTER_X - 18, ICON_CY - 22);
  g->print('?');
}

void draw_offline(Arduino_GFX* g) {
  // Big circle with diagonal slash — "no signal"
  for (int t = -2; t <= 2; ++t)
    g->drawCircle(CENTER_X, ICON_CY, ICON_R - 6 + t, COLOR_OFFLINE);
  // diagonal slash
  for (int t = -2; t <= 2; ++t) {
    g->drawLine(CENTER_X - 36, ICON_CY - 36 + t,
                CENTER_X + 36 + t, ICON_CY + 36, COLOR_OFFLINE);
  }
}

void draw_icon_for(const char* domain, uint16_t color) {
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  if      (!domain || !*domain)                       draw_question(g, color);
  else if (strcmp(domain, "light")          == 0)    draw_bulb(g, color);
  else if (strcmp(domain, "switch")         == 0)    draw_power(g, color);
  else if (strcmp(domain, "input_boolean")  == 0)    draw_power(g, color);
  else if (strcmp(domain, "fan")            == 0)    draw_fan(g, color);
  else if (strcmp(domain, "media_player")   == 0)    draw_speaker(g, color);
  else if (strcmp(domain, "cover")          == 0)    draw_blinds(g, color);
  else if (strcmp(domain, "script")         == 0)    draw_play(g, color);
  else if (strcmp(domain, "automation")     == 0)    draw_gear(g, color);
  else if (strcmp(domain, "scene")          == 0)    draw_sparkle(g, color);
  else                                                draw_question(g, color);
}

// ---------------------------------------------------------------------------
// Friendly-name → first-word fallback. If HA gave us "Bedroom Lamp",
// keep it. If it only gave "light.bedroom_lamp", strip the domain and
// turn underscores into spaces.
// ---------------------------------------------------------------------------
void make_display_name(const char* friendly, const char* entity_id,
                       char* out, size_t max) {
  if (friendly && friendly[0]) {
    strncpy(out, friendly, max - 1);
    out[max - 1] = '\0';
    return;
  }
  const char* dot = entity_id ? strchr(entity_id, '.') : nullptr;
  const char* src = dot ? dot + 1 : entity_id;
  if (!src) { out[0] = '\0'; return; }
  size_t i = 0;
  for (; src[i] && i < max - 1; ++i) {
    out[i] = (src[i] == '_') ? ' ' : src[i];
  }
  out[i] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void HaEntityScreen::on_appear() {
  ::robimon::services::config::get_string("ha_entity",
                                            entity_id_, sizeof(entity_id_));
  if (entity_id_[0]) {
    ::robimon::services::ha_client::track_entity(entity_id_);
  }
  dirty_ = true;
}

void HaEntityScreen::update(uint32_t now_ms) {
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

  // Empty state — no entity configured yet. Question-mark icon + tiny
  // helper text. No "settings" navigation jargon; the kid will get it
  // from the icon, the parent will get it from the text.
  if (!entity_id_[0]) {
    draw_icon_for(nullptr, COLOR_DIM);
    draw_centered_text(g, 220, 3, COLOR_DIM, "no device set");
    draw_centered_text(g, 260, 2, COLOR_DIM, "ask a grown-up");
    ::robimon::hal::display::flush();
    return;
  }

  const auto cs = ha_client::state();
  const bool connected = (cs == ha_client::State::CONNECTED);

  // Disconnected — single signal: offline icon + word.
  if (!connected) {
    draw_offline(g);
    draw_centered_text(g, 220, 3, COLOR_OFFLINE, "offline");
    ::robimon::hal::display::flush();
    return;
  }

  // Connected. Pull state, derive on/off, pick icon color.
  const auto* st = ha_client::get_state(entity_id_);
  const char* state    = (st && st->state[0])         ? st->state : "";
  const char* friendly = (st && st->friendly_name[0]) ? st->friendly_name : nullptr;
  const bool is_on = state_is_on(state);

  char domain[32];
  split_domain(entity_id_, domain, sizeof(domain));

  uint16_t icon_color =
      (s_flash_until_ms != 0) ? COLOR_FLASH :
      is_on                   ? COLOR_WARM_ON :
                                COLOR_DIM;
  draw_icon_for(domain, icon_color);

  // Big state word (size 4) — the visual centerpiece. Shows actual state
  // for sensors / numerics; "on" / "off" for booleans. If there's no
  // state yet (waiting for first event), render "…" small.
  const char* state_word = state[0] ? state : "…";
  uint16_t state_color = (s_flash_until_ms != 0) ? COLOR_FLASH
                                                  : (is_on ? COLOR_WARM_ON : COLOR_CYAN);
  draw_centered_text(g, 200, 4, state_color, state_word);

  // Device name (size 2, dim) — small caption below the state.
  char display_name[40];
  make_display_name(friendly, entity_id_, display_name, sizeof(display_name));
  // Hard truncate so a long name doesn't blow off the canvas. ~30 chars
  // at TextSize 2 (12 px / char) = ~360 px; safe inside 466.
  if (strlen(display_name) > 30) display_name[30] = '\0';
  draw_centered_text(g, 252, 2, COLOR_DIM, display_name);

  ::robimon::hal::display::flush();
}

// ---------------------------------------------------------------------------
// Tap = whole canvas. Lights still get the brightness/color modal; other
// toggleable domains call_service the default action; non-toggleable
// (sensor) domains do nothing.
// ---------------------------------------------------------------------------
void HaEntityScreen::on_tap(int /*x*/, int /*y*/) {
  using namespace ::robimon::services;

  if (!entity_id_[0]) return;
  if (ha_client::state() != ha_client::State::CONNECTED) {
    LOG_W(TAG, "tap ignored — HA not connected");
    return;
  }

  char domain[32]; split_domain(entity_id_, domain, sizeof(domain));

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
  s_flash_until_ms = millis() + FLASH_MS;
  dirty_ = true;
}

}  // namespace robimon::screens
