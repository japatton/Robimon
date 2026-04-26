#include "wifi_setup_screen.h"
#include "text_input_screen.h"
#include "../hal/display.h"
#include "../services/wifi_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

WifiSetupScreen wifi_setup_screen;

namespace {
constexpr const char* TAG = "wifi";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_ERR = 0xF800;

// Round display safe-area: at the top and bottom of the canvas the visible
// width drops to ≈340 px; only at the middle is the full 466 visible. We
// center every element and keep widths comfortably inside that envelope.
constexpr int CW           = 466;
constexpr int CENTER_X     = CW / 2;
constexpr int LIST_W       = 340;          // safe at all canvas y
constexpr int LIST_X       = (CW - LIST_W) / 2;
constexpr int LIST_TOP     = 130;
constexpr int LIST_ROW_H   = 30;
constexpr int LIST_VISIBLE_ROWS = 5;

constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;
constexpr int SCAN_W   = 100;
constexpr int FORGET_W = 100;

// Stash SSID across the password modal callback (the callback runs after we've
// returned from on_tap, so it needs static storage).
char s_wifi_ssid_pending[33] = {0};

void on_password_entered(const char* result, bool cancelled) {
  if (cancelled) {
    LOG_I(TAG, "password entry cancelled for '%s'", s_wifi_ssid_pending);
    return;
  }
  ::robimon::services::wifi_mgr::connect(s_wifi_ssid_pending, result);
  // Force a re-render so the connecting state appears immediately.
  wifi_setup_screen.on_appear();
}

void draw_centered_text(Arduino_GFX* g, int y, int text_size,
                         uint16_t color, const char* text) {
  g->setTextColor(color);
  g->setTextSize(text_size);
  const int ch_w = 6 * text_size;
  const int w = (int)strlen(text) * ch_w;
  g->setCursor(CENTER_X - w / 2, y);
  g->print(text);
}

void draw_centered_button(Arduino_GFX* g, int y, int w, int h,
                           uint16_t color, const char* label) {
  const int x = CENTER_X - w / 2;
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  const int lw = (int)strlen(label) * 12;
  g->setCursor(CENTER_X - lw / 2, y + h / 2 - 8);
  g->print(label);
}

}  // namespace

void WifiSetupScreen::on_appear() {
  dirty_ = true;
}

void WifiSetupScreen::update(uint32_t now_ms) {
  // Re-render every 200 ms so connect/scan progress visibly updates.
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;

  g->fillScreen(COLOR_BG);

  // ---- Header (centered) ---------------------------------------------------
  draw_centered_text(g, 8, /*size=*/3, COLOR_FG, "wifi");

  using namespace ::robimon::services;
  const auto state = wifi_mgr::state();
  const uint16_t state_color = (state == wifi_mgr::State::CONNECTED) ? COLOR_OK
                              : (state == wifi_mgr::State::FAILED)   ? COLOR_ERR
                              :                                         COLOR_DIM;
  draw_centered_text(g, 44, /*size=*/2, state_color, wifi_mgr::state_str());

  if (state == wifi_mgr::State::CONNECTED) {
    draw_centered_text(g, 66, 2, COLOR_FG,  wifi_mgr::current_ssid());
    draw_centered_text(g, 86, 2, COLOR_DIM, wifi_mgr::current_ip());
    // forget button below status
    draw_centered_button(g, 108, FORGET_W, BUTTON_H, COLOR_ERR, "forget");
  } else {
    if (wifi_mgr::current_ssid()[0]) {
      draw_centered_text(g, 66, 2, COLOR_DIM, wifi_mgr::current_ssid());
    } else {
      draw_centered_text(g, 66, 2, COLOR_DIM, "(no network)");
    }
    // scan button below status
    draw_centered_button(g, 92, SCAN_W, BUTTON_H, COLOR_FG,
                         wifi_mgr::scan_in_progress() ? "..." : "scan");
  }

  // ---- Network list --------------------------------------------------------
  const int n = wifi_mgr::scan_count();
  if (state != wifi_mgr::State::CONNECTED && n > 0) {
    g->setTextSize(2);
    const auto* results = wifi_mgr::scan_results();
    const int show = (n < LIST_VISIBLE_ROWS) ? n : LIST_VISIBLE_ROWS;
    for (int i = 0; i < show; ++i) {
      const int y = LIST_TOP + i * LIST_ROW_H;
      const auto& r = results[i];
      g->drawRoundRect(LIST_X,     y,     LIST_W,     LIST_ROW_H - 2, 4, COLOR_FG);
      g->setTextColor(COLOR_FG);
      char trimmed[18];
      strncpy(trimmed, r.ssid, sizeof(trimmed) - 1);
      trimmed[sizeof(trimmed) - 1] = '\0';
      g->setCursor(LIST_X + 8, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(trimmed);
      // Lock indicator + RSSI on the right side
      if (r.secure) {
        g->setCursor(LIST_X + LIST_W - 64, y + LIST_ROW_H / 2 - 8 - 1);
        g->print("L");
      }
      char rssi_buf[8];
      snprintf(rssi_buf, sizeof(rssi_buf), "%d", (int)r.rssi);
      g->setCursor(LIST_X + LIST_W - 44, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(rssi_buf);
    }
  } else if (state != wifi_mgr::State::CONNECTED && wifi_mgr::scan_in_progress()) {
    draw_centered_text(g, 150, 2, COLOR_DIM, "scanning...");
  }

  // ---- Back at very bottom (where the round display is widest near center) -
  draw_centered_button(g, 320 - BUTTON_H - 8, BACK_W, BUTTON_H, COLOR_DIM, "back");

  ::robimon::hal::display::flush();
}

void WifiSetupScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  // Back button
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  using namespace ::robimon::services;
  const auto state = wifi_mgr::state();

  if (state == wifi_mgr::State::CONNECTED) {
    // Forget
    if (panel_x >= CENTER_X - FORGET_W / 2 && panel_x < CENTER_X + FORGET_W / 2 &&
        canvas_y >= 108 && canvas_y < 108 + BUTTON_H) {
      LOG_I(TAG, "forget");
      wifi_mgr::forget();
      dirty_ = true;
      return;
    }
  } else {
    // Scan
    if (panel_x >= CENTER_X - SCAN_W / 2 && panel_x < CENTER_X + SCAN_W / 2 &&
        canvas_y >= 92 && canvas_y < 92 + BUTTON_H) {
      LOG_I(TAG, "scan triggered");
      wifi_mgr::start_scan();
      dirty_ = true;
      return;
    }
  }

  // Network list
  const int n = wifi_mgr::scan_count();
  if (n > 0) {
    const auto* results = wifi_mgr::scan_results();
    const int show = (n < LIST_VISIBLE_ROWS) ? n : LIST_VISIBLE_ROWS;
    for (int i = 0; i < show; ++i) {
      const int y = LIST_TOP + i * LIST_ROW_H;
      if (panel_x >= LIST_X && panel_x < LIST_X + LIST_W &&
          canvas_y >= y     && canvas_y < y + LIST_ROW_H - 2) {
        const auto& r = results[i];
        LOG_I(TAG, "tapped network '%s' (secure=%d)", r.ssid, (int)r.secure);
        if (!r.secure) {
          wifi_mgr::connect(r.ssid, "");
          dirty_ = true;
        } else {
          strncpy(s_wifi_ssid_pending, r.ssid, sizeof(s_wifi_ssid_pending) - 1);
          s_wifi_ssid_pending[sizeof(s_wifi_ssid_pending) - 1] = '\0';
          char prompt[64];
          snprintf(prompt, sizeof(prompt), "password for %s", r.ssid);
          text_input::show(prompt, /*initial=*/"", /*masked=*/true,
                           /*max_len=*/63, &on_password_entered);
        }
        return;
      }
    }
  }
}

}  // namespace robimon::screens
