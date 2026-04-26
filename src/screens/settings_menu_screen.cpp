#include "settings_menu_screen.h"
#include "wifi_setup_screen.h"
#include "time_settings_screen.h"
#include "display_settings_screen.h"
#include "about_screen.h"
#include "alarms_settings_screen.h"
#include "../hal/display.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

SettingsMenuScreen settings_menu_screen;

namespace {
constexpr const char* TAG = "set";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;

// 2-column × 4-row grid of sections. The "back" button sits below the grid.
struct Section { const char* label; };
constexpr Section SECTIONS[] = {
  { "wifi"     },
  { "ha"       },
  { "voice"    },
  { "alarms"   },
  { "display"  },
  { "time"     },
  { "PIN"      },
  { "about"    },
};
constexpr int N_SECTIONS = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

constexpr int CELL_W   = 200;
constexpr int CELL_H   = 38;
constexpr int CELL_GAP = 8;
constexpr int N_COLS   = 2;
constexpr int N_ROWS   = (N_SECTIONS + N_COLS - 1) / N_COLS;
constexpr int GRID_W   = N_COLS * CELL_W + (N_COLS - 1) * CELL_GAP;
constexpr int GRID_H   = N_ROWS * CELL_H + (N_ROWS - 1) * CELL_GAP;
constexpr int GRID_Y0  = 46;          // just below the title

constexpr int BACK_W   = 96;
constexpr int BACK_H   = 36;
constexpr int BACK_Y   = 320 - BACK_H - 8;   // canvas-local y
}  // namespace

void SettingsMenuScreen::on_appear() {
  dirty_ = true;
}

void SettingsMenuScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;

  g->fillScreen(COLOR_BG);
  const int cw = g->width();
  const int ch = g->height();

  // Title
  g->setTextColor(COLOR_FG);
  g->setTextSize(3);
  const char* title = "settings";
  const int t_w = (int)strlen(title) * 18;
  g->setCursor(cw / 2 - t_w / 2, 8);
  g->print(title);

  // Section grid
  const int grid_x0 = (cw - GRID_W) / 2;
  for (int i = 0; i < N_SECTIONS; ++i) {
    const int col = i % N_COLS;
    const int row = i / N_COLS;
    const int x = grid_x0 + col * (CELL_W + CELL_GAP);
    const int y = GRID_Y0  + row * (CELL_H + CELL_GAP);
    g->drawRoundRect(x,     y,     CELL_W,     CELL_H,     6, COLOR_FG);
    g->drawRoundRect(x + 1, y + 1, CELL_W - 2, CELL_H - 2, 5, COLOR_FG);

    g->setTextColor(COLOR_FG);
    g->setTextSize(2);
    const char* lbl = SECTIONS[i].label;
    const int lw = (int)strlen(lbl) * 12;
    g->setCursor(x + CELL_W / 2 - lw / 2, y + CELL_H / 2 - 8);
    g->print(lbl);
  }

  // Back button at bottom — well below the grid (grid bottom is GRID_Y0+GRID_H,
  // back top is BACK_Y, gap is at least 16 px).
  const int back_x = cw / 2 - BACK_W / 2;
  g->drawRoundRect(back_x,     BACK_Y,     BACK_W,     BACK_H,     6, COLOR_DIM);
  g->drawRoundRect(back_x + 1, BACK_Y + 1, BACK_W - 2, BACK_H - 2, 5, COLOR_DIM);
  g->setTextColor(COLOR_DIM);
  g->setTextSize(2);
  const char* back = "back";
  const int bw = (int)strlen(back) * 12;
  g->setCursor(cw / 2 - bw / 2, BACK_Y + BACK_H / 2 - 8);
  g->print(back);

  ::robimon::hal::display::flush();
}

void SettingsMenuScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  const int cw = g->width();
  const int ch = g->height();

  // Section grid hit test
  const int grid_x0 = (cw - GRID_W) / 2;
  for (int i = 0; i < N_SECTIONS; ++i) {
    const int col = i % N_COLS;
    const int row = i / N_COLS;
    const int x = grid_x0 + col * (CELL_W + CELL_GAP);
    const int y = GRID_Y0  + row * (CELL_H + CELL_GAP);
    if (panel_x >= x && panel_x < x + CELL_W &&
        canvas_y >= y && canvas_y < y + CELL_H) {
      LOG_I(TAG, "tapped section: %s", SECTIONS[i].label);
      const char* lbl = SECTIONS[i].label;
      if      (strcmp(lbl, "wifi")    == 0) ::robimon::ui::screen_mgr::push_modal(&wifi_setup_screen);
      else if (strcmp(lbl, "time")    == 0) ::robimon::ui::screen_mgr::push_modal(&time_settings_screen);
      else if (strcmp(lbl, "display") == 0) ::robimon::ui::screen_mgr::push_modal(&display_settings_screen);
      else if (strcmp(lbl, "about")   == 0) ::robimon::ui::screen_mgr::push_modal(&about_screen);
      else if (strcmp(lbl, "alarms")  == 0) ::robimon::ui::screen_mgr::push_modal(&alarms_settings_screen);
      // ha / voice / PIN tiles still stub (F-4 for PIN, H/I for HA/voice)
      return;
    }
  }

  // Back hit test
  const int back_x = cw / 2 - BACK_W / 2;
  if (panel_x >= back_x && panel_x < back_x + BACK_W &&
      canvas_y >= BACK_Y && canvas_y < BACK_Y + BACK_H) {
    LOG_I(TAG, "exit settings");
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }
  (void)ch;
}

}  // namespace robimon::screens
