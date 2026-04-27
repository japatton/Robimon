#include "voice_settings_screen.h"
#include "text_input_screen.h"
#include "../hal/display.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <HTTPClient.h>
#include <string.h>

namespace robimon::screens {

VoiceSettingsScreen voice_settings_screen;

namespace {
constexpr const char* TAG = "voi-set";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_ERR = 0xF800;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;
constexpr int TEST_W   = 96;

constexpr int ROW_H   = 36;
constexpr int FIELD_W = 260;
constexpr int LABEL_W = 90;
constexpr int FIELD_X = (CW - FIELD_W) / 2;
constexpr int LABEL_X = FIELD_X - LABEL_W - 6;
constexpr int URL_Y    = 50;
constexpr int MODEL_Y  = 92;
constexpr int PROMPT_Y = 134;
constexpr int RESULT_Y = 210;
constexpr int TEST_Y   = 240;

// NVS keys
constexpr const char* K_URL    = "voice_url";
constexpr const char* K_MODEL  = "voice_model";
constexpr const char* K_PROMPT = "voice_prompt";

enum class EditField : uint8_t { NONE, URL, MODEL, PROMPT };
EditField s_pending = EditField::NONE;

// Last test-connection result (sticky on the screen until next render).
char     s_last_result[64] = {0};
uint16_t s_last_result_color = COLOR_DIM;

void on_text_done(const char* result, bool cancelled) {
  if (cancelled) { s_pending = EditField::NONE; return; }
  using namespace ::robimon::services;
  switch (s_pending) {
    case EditField::URL:    config::set_string(K_URL,    result); break;
    case EditField::MODEL:  config::set_string(K_MODEL,  result); break;
    case EditField::PROMPT: config::set_string(K_PROMPT, result); break;
    default: break;
  }
  s_pending = EditField::NONE;
  voice_settings_screen.on_appear();
}

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}
void draw_field_row(Arduino_GFX* g, int y, const char* label, const char* value) {
  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  g->setCursor(LABEL_X, y + ROW_H / 2 - 8);
  g->print(label);
  g->drawRoundRect(FIELD_X, y, FIELD_W, ROW_H - 4, 6, COLOR_FG);
  g->setTextColor(COLOR_FG);
  g->setCursor(FIELD_X + 8, y + ROW_H / 2 - 8 - 1);
  if (value && value[0]) {
    char trimmed[24];
    strncpy(trimmed, value, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    g->print(trimmed);
  } else {
    g->setTextColor(COLOR_DIM);
    g->print("(tap to set)");
  }
}
void draw_button(Arduino_GFX* g, int x, int y, int w, int h, uint16_t color, const char* label) {
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  g->setCursor(x + w / 2 - (int)strlen(label) * 12 / 2, y + h / 2 - 8);
  g->print(label);
}

// Hits GET <url replacing /chat with /health>; returns true on 200.
// Any URL, any errors → false. We don't surface stack traces; the kid-safe
// rule extends here too — only "ok" / "failed" / brief reason.
bool ping_companion_url(const char* url, char* out_msg, size_t max) {
  if (!url || !*url) {
    snprintf(out_msg, max, "no URL set");
    return false;
  }
  // Build /health URL: strip trailing /chat if present, append /health.
  char host_url[200];
  strncpy(host_url, url, sizeof(host_url) - 1);
  host_url[sizeof(host_url) - 1] = '\0';
  char* tail = strstr(host_url, "/chat");
  if (tail) *tail = '\0';
  // Trim a trailing slash if present so we don't double up.
  size_t L = strlen(host_url);
  if (L > 0 && host_url[L - 1] == '/') host_url[L - 1] = '\0';

  char health_url[220];
  snprintf(health_url, sizeof(health_url), "%s/health", host_url);

  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(health_url)) {
    snprintf(out_msg, max, "bad URL");
    return false;
  }
  const int code = http.GET();
  http.end();
  if (code == 200) { snprintf(out_msg, max, "ok"); return true; }
  snprintf(out_msg, max, "failed (%d)", code);
  return false;
}

}  // namespace

void VoiceSettingsScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 3, COLOR_FG, "voice");

  using namespace ::robimon::services;
  char url[160] = {0}, model[64] = {0}, prompt[200] = {0};
  config::get_string(K_URL,    url,    sizeof(url));
  config::get_string(K_MODEL,  model,  sizeof(model));
  config::get_string(K_PROMPT, prompt, sizeof(prompt));

  draw_field_row(g, URL_Y,    "URL",    url);
  draw_field_row(g, MODEL_Y,  "model",  model);
  draw_field_row(g, PROMPT_Y, "prompt", prompt);

  // Last test-result line
  if (s_last_result[0]) {
    g->setTextColor(s_last_result_color);
    g->setTextSize(2);
    const int w = (int)strlen(s_last_result) * 12;
    g->setCursor(CENTER_X - w / 2, RESULT_Y);
    g->print(s_last_result);
  }

  // Test + back at bottom row
  const int bottom_y = 320 - BUTTON_H - 8;
  const int total = TEST_W + 12 + BACK_W;
  const int test_x = CENTER_X - total / 2;
  const int back_x = test_x + TEST_W + 12;
  draw_button(g, test_x, bottom_y, TEST_W, BUTTON_H, COLOR_FG,  "test");
  draw_button(g, back_x, bottom_y, BACK_W, BUTTON_H, COLOR_DIM, "back");

  ::robimon::hal::display::flush();
}

void VoiceSettingsScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  using namespace ::robimon::services;

  auto field_hit = [&](int y) {
    return panel_x >= FIELD_X && panel_x < FIELD_X + FIELD_W &&
           canvas_y >= y && canvas_y < y + ROW_H - 4;
  };

  if (field_hit(URL_Y)) {
    s_pending = EditField::URL;
    char url[160] = {0};
    config::get_string(K_URL, url, sizeof(url));
    text_input::show("companion URL (e.g. http://host:8765/chat)",
                     url, /*masked=*/false, /*max=*/95, &on_text_done);
    return;
  }
  if (field_hit(MODEL_Y)) {
    s_pending = EditField::MODEL;
    char m[64] = {0};
    config::get_string(K_MODEL, m, sizeof(m));
    text_input::show("ollama model (e.g. llama3.1:8b)",
                     m, /*masked=*/false, /*max=*/63, &on_text_done);
    return;
  }
  if (field_hit(PROMPT_Y)) {
    s_pending = EditField::PROMPT;
    char p[200] = {0};
    config::get_string(K_PROMPT, p, sizeof(p));
    text_input::show("system prompt (blank = default)",
                     p, /*masked=*/false, /*max=*/95, &on_text_done);
    return;
  }

  // Bottom row
  const int bottom_y = 320 - BUTTON_H - 8;
  const int total = TEST_W + 12 + BACK_W;
  const int test_x = CENTER_X - total / 2;
  const int back_x = test_x + TEST_W + 12;

  if (panel_x >= test_x && panel_x < test_x + TEST_W &&
      canvas_y >= bottom_y && canvas_y < bottom_y + BUTTON_H) {
    char url[160] = {0};
    config::get_string(K_URL, url, sizeof(url));
    snprintf(s_last_result, sizeof(s_last_result), "testing...");
    s_last_result_color = COLOR_DIM;
    dirty_ = true;
    update(millis());   // force-render the "testing..." text right away
    char msg[64];
    const bool ok = ping_companion_url(url, msg, sizeof(msg));
    snprintf(s_last_result, sizeof(s_last_result), "%s", msg);
    s_last_result_color = ok ? COLOR_OK : COLOR_ERR;
    dirty_ = true;
    LOG_I(TAG, "test: %s -> %s", url, msg);
    return;
  }
  if (panel_x >= back_x && panel_x < back_x + BACK_W &&
      canvas_y >= bottom_y && canvas_y < bottom_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }
}

}  // namespace robimon::screens
