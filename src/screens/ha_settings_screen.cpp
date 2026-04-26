#include "ha_settings_screen.h"
#include "text_input_screen.h"
#include "../hal/display.h"
#include "../services/ha_client.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

HaSettingsScreen ha_settings_screen;

namespace {
constexpr const char* TAG = "ha-set";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_ERR = 0xF800;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;

constexpr const char* KEY_DEFAULT_ENTITY = "ha_entity";

constexpr int ROW_H = 36;
constexpr int FIELD_W = 260;
constexpr int LABEL_W = 90;
constexpr int FIELD_X = (CW - FIELD_W) / 2;
constexpr int LABEL_X = FIELD_X - LABEL_W - 6;
constexpr int URL_Y    = 50;
constexpr int TOKEN_Y  = 92;
constexpr int ENTITY_Y = 134;
constexpr int FORGET_W = 100;
constexpr int FORGET_Y = 234;

// Stash the field being edited so the text-input callback knows which value
// to write back. Static storage outlives our call.
enum class EditField : uint8_t { NONE, URL, TOKEN, ENTITY };
EditField s_pending = EditField::NONE;

void on_text_done(const char* result, bool cancelled) {
  if (cancelled) { s_pending = EditField::NONE; return; }
  using namespace ::robimon::services;
  switch (s_pending) {
    case EditField::URL:
      // Token might already be saved; only call connect() once we have both.
      ::robimon::services::config::set_string("ha_url", result);
      if (ha_client::url()[0] || result[0]) {
        char tok[256] = {0};
        ::robimon::services::config::get_string("ha_token", tok, sizeof(tok));
        if (tok[0]) ha_client::connect(result, tok);
      }
      break;
    case EditField::TOKEN:
      ::robimon::services::config::set_string("ha_token", result);
      {
        char url[160] = {0};
        ::robimon::services::config::get_string("ha_url", url, sizeof(url));
        if (url[0]) ha_client::connect(url, result);
      }
      break;
    case EditField::ENTITY:
      ::robimon::services::config::set_string(KEY_DEFAULT_ENTITY, result);
      ha_client::track_entity(result);
      break;
    default:
      break;
  }
  s_pending = EditField::NONE;
  ha_settings_screen.on_appear();
}

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}
void draw_field_row(Arduino_GFX* g, int y, const char* label, const char* value, bool masked) {
  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  g->setCursor(LABEL_X, y + ROW_H / 2 - 8);
  g->print(label);
  g->drawRoundRect(FIELD_X, y, FIELD_W, ROW_H - 4, 6, COLOR_FG);
  g->setTextColor(COLOR_FG);
  g->setCursor(FIELD_X + 8, y + ROW_H / 2 - 8 - 1);
  if (masked && value[0]) {
    char buf[20];
    int n = (int)strlen(value);
    if (n > 14) n = 14;
    for (int i = 0; i < n; ++i) buf[i] = '*';
    buf[n] = '\0';
    g->print(buf);
  } else if (value[0]) {
    char trimmed[24];
    strncpy(trimmed, value, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    g->print(trimmed);
  } else {
    g->setTextColor(COLOR_DIM);
    g->print("(tap to set)");
  }
}
void draw_centered_button(Arduino_GFX* g, int y, int w, int h, uint16_t color, const char* label) {
  const int x = CENTER_X - w / 2;
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  g->setCursor(CENTER_X - (int)strlen(label) * 12 / 2, y + h / 2 - 8);
  g->print(label);
}

}  // namespace

void HaSettingsScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 3, COLOR_FG, "ha");

  using namespace ::robimon::services;
  const auto st = ha_client::state();
  const uint16_t state_color = (st == ha_client::State::CONNECTED) ? COLOR_OK
                              : (st == ha_client::State::FAILED)   ? COLOR_ERR
                              :                                       COLOR_DIM;
  draw_centered_text(g, 32, 2, state_color, ha_client::state_str());

  // Read saved values for display.
  char url[160] = {0};
  char token[256] = {0};
  char entity[80] = {0};
  ::robimon::services::config::get_string("ha_url",   url,   sizeof(url));
  ::robimon::services::config::get_string("ha_token", token, sizeof(token));
  ::robimon::services::config::get_string(KEY_DEFAULT_ENTITY, entity, sizeof(entity));

  draw_field_row(g, URL_Y,    "url",    url,    false);
  draw_field_row(g, TOKEN_Y,  "token",  token,  /*masked=*/true);
  draw_field_row(g, ENTITY_Y, "entity", entity, false);

  // Forget button (clears credentials)
  draw_centered_button(g, FORGET_Y, FORGET_W, BUTTON_H, COLOR_ERR, "forget");

  // Back at bottom
  draw_centered_button(g, 320 - BUTTON_H - 8, BACK_W, BUTTON_H, COLOR_DIM, "back");
  ::robimon::hal::display::flush();
}

void HaSettingsScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  using namespace ::robimon::services;

  // Back
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  // Forget
  if (panel_x >= CENTER_X - FORGET_W / 2 && panel_x < CENTER_X + FORGET_W / 2 &&
      canvas_y >= FORGET_Y && canvas_y < FORGET_Y + BUTTON_H) {
    LOG_I(TAG, "forget HA creds");
    ha_client::forget();
    ::robimon::services::config::set_string(KEY_DEFAULT_ENTITY, "");
    dirty_ = true;
    return;
  }

  // Field rows -> push text input
  auto field_hit = [&](int y) {
    return panel_x >= FIELD_X && panel_x < FIELD_X + FIELD_W &&
           canvas_y >= y && canvas_y < y + ROW_H - 4;
  };
  if (field_hit(URL_Y)) {
    s_pending = EditField::URL;
    char url[160] = {0};
    ::robimon::services::config::get_string("ha_url", url, sizeof(url));
    text_input::show("HA URL (e.g. ws://hass.local:8123)", url, /*masked=*/false,
                     /*max=*/95, &on_text_done);
    return;
  }
  if (field_hit(TOKEN_Y)) {
    s_pending = EditField::TOKEN;
    char tok[256] = {0};
    ::robimon::services::config::get_string("ha_token", tok, sizeof(tok));
    text_input::show("long-lived access token", tok, /*masked=*/true,
                     /*max=*/95, &on_text_done);
    return;
  }
  if (field_hit(ENTITY_Y)) {
    s_pending = EditField::ENTITY;
    char ent[80] = {0};
    ::robimon::services::config::get_string(KEY_DEFAULT_ENTITY, ent, sizeof(ent));
    text_input::show("entity (e.g. sun.sun)", ent, /*masked=*/false,
                     /*max=*/63, &on_text_done);
    return;
  }
}

}  // namespace robimon::screens
