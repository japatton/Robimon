#include "face.h"
#include "../hal/display.h"
#include "../services/voice_client.h"
#include "../services/tutorial.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace robimon::face {

// =============================================================================
// Geometry
// =============================================================================
//
// Two large pixelated eyes + a pixelated mouth + pink cheek blushes. Kawaii
// pixel-robot vibe. Expression is conveyed through a combination of:
//   - eye lid coverage (top/bottom, with inner/outer tilt for furrowed brows)
//   - mouth shape (one of a few small pixel bitmaps)
//   - cheek blushes (visible for warm moods, hidden for serious/scared)
//
// Drawing happens into the back-buffer owned by the display HAL. Coordinates
// are canvas-local: (0,0) is the top-left of the canvas band, which sits on
// the panel at (0, display::canvas_panel_y_offset()).
//
namespace {

constexpr const char* TAG = "face";

constexpr int PANEL_W        = 466;
constexpr int LEFT_EYE_CX    = 145;
constexpr int RIGHT_EYE_CX   = PANEL_W - LEFT_EYE_CX;

// Eye geometry: rounded square. Width / height in pixels (snapped to cells
// at draw time). The corner chamfer is CORNER_CELLS cells per corner —
// gives an octagonal/rounded-square silhouette.
constexpr int EYE_W_BASE     = 130;     // 13 cells wide
constexpr int EYE_H_BASE     = 130;     // 13 cells tall
constexpr int CORNER_CELLS   = 2;       // chamfer K cells from each corner

// Vertical centers within the canvas. Set in begin() from canvas height.
int s_face_cy   = 160;   // eyes
int s_mouth_cy  = 270;   // mouth (centered horizontally)

// Reserve the bottom of the canvas for screen-manager overlays (carousel
// dots). Face renderers fillRect to FACE_DRAW_BOTTOM rather than fillScreen
// — clobbering the dots between the face flush and screen_mgr's flush_band
// otherwise produces a visible flicker at 30 FPS.
constexpr int FACE_DRAW_BOTTOM = 300;

// Robot "mouth" — a vocoder-style speaker grille that doubles as the
// voice-mode button. Replaces the kawaii pixel mouth. Centered at the
// canvas position previously occupied by the mouth.
constexpr int MOUTH_BTN_W      = 130;
constexpr int MOUTH_BTN_H      = 36;
constexpr int MOUTH_BAR_COUNT  = 9;
constexpr int MOUTH_BAR_W      = 6;
constexpr int MOUTH_BAR_H      = 22;
constexpr int MOUTH_BAR_GAP    = 6;

constexpr int MOUTH_CX       = PANEL_W / 2;

// Color palette — sci-fi blue on black. RGB565.
//   0x055F = R=0, G=168, B=248 → strong electric blue-cyan; reads as "robot LED"
//   rather than the near-white cyan of the previous look.
constexpr uint16_t COLOR_BG       = 0x0000;
constexpr uint16_t COLOR_EYE      = 0x055F;
constexpr uint16_t COLOR_EYE_FLASH = 0xFFFF;   // white — listen cue accent
constexpr uint16_t COLOR_MOUTH    = 0x055F;

// Pixelation: each "logical pixel" is a CELL_PX × CELL_PX block with a 1-px
// black gap between blocks. Same cell size for eyes and mouth so the whole
// face reads as one unified pixel grid.
constexpr int CELL_PX   = 10;
constexpr int CELL_FILL = CELL_PX - 1;

// =============================================================================
// Mouth bitmaps
// =============================================================================
//
// Each mouth is a small grid of cells. cols × rows; row_bits[r] bit 0 is the
// LEFTMOST cell of that row, bit (cols-1) is the rightmost. Lit cells get
// drawn in COLOR_MOUTH; unset cells stay background.
//
struct MouthDef {
  uint8_t  cols;
  uint8_t  rows;
  uint16_t row_bits[5];
};

// Tiny flat dash — neutral / listening.
constexpr MouthDef MOUTH_FLAT = { 5, 1, {
  0b11111,
}};

// Curved smile — happy / excited.
constexpr MouthDef MOUTH_SMILE = { 7, 2, {
  0b1000001,
  0b0111110,
}};

// Inverted curve — sad / confused.
constexpr MouthDef MOUTH_FROWN = { 7, 2, {
  0b0111110,
  0b1000001,
}};

// Small open "O" — surprised.
constexpr MouthDef MOUTH_O = { 3, 3, {
  0b010,
  0b101,
  0b010,
}};

// Slight angle — thinking (looks like "hmm").
constexpr MouthDef MOUTH_HMM = { 5, 1, {
  0b11110,
}};

// Wide-open chatter shape — speaking. Will get amplitude-modulated later.
constexpr MouthDef MOUTH_SPEAK = { 5, 3, {
  0b01110,
  0b11111,
  0b01110,
}};

// Slight downturn — angry.
constexpr MouthDef MOUTH_GRIMACE = { 7, 2, {
  0b1111111,
  0b0000000,
}};

// Tiny squiggle — sleepy.
constexpr MouthDef MOUTH_DASH = { 3, 1, {
  0b111,
}};

// "No mouth" sentinel.
constexpr MouthDef MOUTH_NONE = { 0, 0, {0} };

// =============================================================================
// Eye / face params
// =============================================================================
struct EyeShape {
  float scale;
  float top_lid_outer;
  float top_lid_inner;
  float bot_lid_outer;
  float bot_lid_inner;
};
struct FaceParams {
  EyeShape left;
  EyeShape right;
  const MouthDef* mouth;
};

inline EyeShape sym(float scale, float top, float bot) {
  return { scale, top, top, bot, bot };
}
inline EyeShape sym4(float scale, float to, float ti, float bo, float bi) {
  return { scale, to, ti, bo, bi };
}

// =============================================================================
// Expression presets
// =============================================================================
const FaceParams P_NEUTRAL   = { sym(1.00f, 0.00f, 0.00f),
                                  sym(1.00f, 0.00f, 0.00f),
                                  &MOUTH_FLAT };

const FaceParams P_HAPPY     = { sym(1.00f, 0.00f, 0.00f),
                                  sym(1.00f, 0.00f, 0.00f),
                                  &MOUTH_SMILE };

const FaceParams P_SAD       = { sym4(0.95f, 0.45f, 0.10f, 0.00f, 0.00f),
                                  sym4(0.95f, 0.10f, 0.45f, 0.00f, 0.00f),
                                  &MOUTH_FROWN };

const FaceParams P_SLEEPY    = { sym(0.95f, 0.78f, 0.78f),
                                  sym(0.95f, 0.78f, 0.78f),
                                  &MOUTH_DASH };

const FaceParams P_SURPRISED = { sym(1.15f, 0.00f, 0.00f),
                                  sym(1.15f, 0.00f, 0.00f),
                                  &MOUTH_O };

const FaceParams P_ANGRY     = { sym4(0.95f, 0.10f, 0.55f, 0.00f, 0.00f),
                                  sym4(0.95f, 0.55f, 0.10f, 0.00f, 0.00f),
                                  &MOUTH_GRIMACE };

const FaceParams P_THINKING  = { sym(1.00f, 0.05f, 0.05f),
                                  sym4(0.95f, 0.55f, 0.20f, 0.10f, 0.00f),
                                  &MOUTH_HMM };

const FaceParams P_EXCITED   = { sym(1.05f, 0.00f, 0.00f),
                                  sym(1.05f, 0.00f, 0.00f),
                                  &MOUTH_SMILE };

const FaceParams P_CONFUSED  = { sym(1.05f, 0.00f, 0.00f),
                                  sym4(0.80f, 0.30f, 0.10f, 0.05f, 0.00f),
                                  &MOUTH_HMM };

const FaceParams P_LISTENING = { sym(1.05f, 0.00f, 0.00f),
                                  sym(1.05f, 0.00f, 0.00f),
                                  &MOUTH_FLAT };

const FaceParams P_SPEAKING  = { sym(1.00f, 0.10f, 0.10f),
                                  sym(1.00f, 0.10f, 0.10f),
                                  &MOUTH_SPEAK };

const FaceParams& preset(Expression e) {
  switch (e) {
    case Expression::HAPPY:     return P_HAPPY;
    case Expression::SAD:       return P_SAD;
    case Expression::SLEEPY:    return P_SLEEPY;
    case Expression::SURPRISED: return P_SURPRISED;
    case Expression::ANGRY:     return P_ANGRY;
    case Expression::THINKING:  return P_THINKING;
    case Expression::EXCITED:   return P_EXCITED;
    case Expression::CONFUSED:  return P_CONFUSED;
    case Expression::LISTENING: return P_LISTENING;
    case Expression::SPEAKING:  return P_SPEAKING;
    default:                    return P_NEUTRAL;
  }
}

const char* name_of(Expression e) {
  switch (e) {
    case Expression::HAPPY:     return "happy";
    case Expression::SAD:       return "sad";
    case Expression::SLEEPY:    return "sleepy";
    case Expression::SURPRISED: return "surprised";
    case Expression::ANGRY:     return "angry";
    case Expression::THINKING:  return "thinking";
    case Expression::EXCITED:   return "excited";
    case Expression::CONFUSED:  return "confused";
    case Expression::LISTENING: return "listening";
    case Expression::SPEAKING:  return "speaking";
    default:                    return "neutral";
  }
}

// =============================================================================
// Radial menu
// =============================================================================
// 8 expressions arranged around a circle. Center of the menu is the canvas
// center; items orbit at MENU_RADIUS. Tap on an item → set that expression
// and dismiss. Tap outside / no tap for MENU_TIMEOUT_MS → dismiss without
// changing.
struct MenuItem {
  Expression  expr;
  const char* label;
};
// Short labels so they read at TextSize 3 (18×24 px chars) within a circular
// menu button. Up to 6 chars max keeps each label under ~108 px wide.
constexpr MenuItem MENU_ITEMS[] = {
  { Expression::HAPPY,     "happy"  },
  { Expression::EXCITED,   "yay"    },
  { Expression::SURPRISED, "wow"    },
  { Expression::CONFUSED,  "huh"    },
  { Expression::ANGRY,     "angry"  },
  { Expression::SAD,       "sad"    },
  { Expression::SLEEPY,    "sleep"  },
  { Expression::THINKING,  "think"  },
};
constexpr int MENU_COUNT     = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);
constexpr int MENU_RADIUS    = 125;     // px from canvas center to item centers
constexpr int MENU_ITEM_R    = 50;      // hit-test radius around item center (generous)
constexpr uint32_t MENU_TIMEOUT_MS = 5000;

enum class FaceMode : uint8_t { IDLE, MENU, FLASH };

// Brief on-screen flash for confirming gestures that don't yet have a
// destination UI (long-press → settings; swipe → screen change). Pure visual
// "we saw your gesture" feedback; goes away after FLASH_MS.
constexpr uint32_t FLASH_MS = 350;

// =============================================================================
// State
// =============================================================================
FaceParams  s_from{};
FaceParams  s_to{};
FaceParams  s_current{};
Expression  s_target_expr   = Expression::NEUTRAL;
uint32_t    s_tween_start_ms = 0;
uint32_t    s_tween_dur_ms   = 250;
uint32_t    s_last_frame_ms  = 0;
uint32_t    s_next_blink_ms  = 0;
uint32_t    s_blink_start_ms = 0;
bool        s_demo_on = false;
uint32_t    s_demo_next_ms = 0;
uint8_t     s_demo_idx = 0;

FaceMode    s_mode = FaceMode::IDLE;
uint32_t    s_menu_open_ms = 0;
uint32_t    s_flash_start_ms = 0;
const char* s_flash_text = "";

// Persistent label overlay (e.g., alarm name). Empty string when none.
char        s_label[28] = {0};

// Voice-flow visual hooks. flash_eyes() draws white eyes for a beat;
// start_listening_ring() draws a depleting arc around the mouth for the
// supplied window. Both are owned by the face renderer so screens
// upstream don't have to re-implement timing logic.
uint32_t    s_eye_flash_until_ms    = 0;
uint32_t    s_listen_ring_start_ms  = 0;
uint32_t    s_listen_ring_window_ms = 0;


// (Double-tap detection lives in the gesture detector now — it emits a
// DOUBLE_TAP event after a TAP when a quick second tap follows. main.cpp
// translates that into face::dismiss_menu() + voice::start(). Keeping the
// face::on_tap path instant means single-tap menu open isn't laggy.)

// =============================================================================
// Helpers
// =============================================================================
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float ease_out_cubic(float t) { return 1.0f - powf(1.0f - t, 3.0f); }
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

void interp_eye(EyeShape& out, const EyeShape& a, const EyeShape& b, float t) {
  out.scale         = lerp(a.scale,         b.scale,         t);
  out.top_lid_outer = lerp(a.top_lid_outer, b.top_lid_outer, t);
  out.top_lid_inner = lerp(a.top_lid_inner, b.top_lid_inner, t);
  out.bot_lid_outer = lerp(a.bot_lid_outer, b.bot_lid_outer, t);
  out.bot_lid_inner = lerp(a.bot_lid_inner, b.bot_lid_inner, t);
}
void interp(FaceParams& out, const FaceParams& a, const FaceParams& b, float t) {
  interp_eye(out.left,  a.left,  b.left,  t);
  interp_eye(out.right, a.right, b.right, t);
  // Mouth switches discretely at the midpoint (no smooth bitmap tween).
  out.mouth = (t < 0.5f) ? a.mouth : b.mouth;
}

// =============================================================================
// Drawing
// =============================================================================
//
// Eye is a rounded-square (octagonal) shape: a rectangle with the K corner
// cells chamfered off via a Manhattan-distance corner cut. Lids cover the
// top/bottom with linear inner-vs-outer interpolation, same as before.
//
void draw_pixelated_eye(int cx, int cy, int w, int h, bool is_left,
                         float top_outer, float top_inner,
                         float bot_outer, float bot_inner) {
  if (w < CELL_PX || h < CELL_PX) return;

  Arduino_GFX* g = robimon::hal::display::gfx();
  const int half_w = w / 2;
  const int half_h = h / 2;
  const int x0 = cx - half_w;
  const int x1 = cx + half_w;
  const int y0 = cy - half_h;
  const int y1 = cy + half_h;
  const int corner_px = CORNER_CELLS * CELL_PX;

  // Snap grid origin to multiples of CELL_PX.
  const int gx0 = x0 - (x0 % CELL_PX + CELL_PX) % CELL_PX;
  const int gy0 = y0 - (y0 % CELL_PX + CELL_PX) % CELL_PX;

  for (int gy = gy0; gy < y1; gy += CELL_PX) {
    const int cell_cy = gy + CELL_PX / 2;
    if (cell_cy < y0 || cell_cy >= y1) continue;
    const int dy_top = cell_cy - y0;
    const int dy_bot = y1 - cell_cy;
    const int dy_edge = (dy_top < dy_bot) ? dy_top : dy_bot;

    for (int gx = gx0; gx < x1; gx += CELL_PX) {
      const int cell_cx = gx + CELL_PX / 2;
      if (cell_cx < x0 || cell_cx >= x1) continue;
      const int dx_left  = cell_cx - x0;
      const int dx_right = x1 - cell_cx;
      const int dx_edge  = (dx_left < dx_right) ? dx_left : dx_right;

      // Manhattan corner chamfer: skip cells where the sum of distances to
      // the two nearest edges is less than the chamfer threshold.
      if (dx_edge < corner_px && dy_edge < corner_px &&
          (dx_edge + dy_edge) < corner_px) continue;

      // Lid coverage at this x.
      float x_norm = (float)(cell_cx - x0) / (float)w;
      if (!is_left) x_norm = 1.0f - x_norm;
      const float top_depth = (top_outer + (top_inner - top_outer) * x_norm) * h;
      const float bot_depth = (bot_outer + (bot_inner - bot_outer) * x_norm) * h;
      if (dy_top < top_depth)         continue;
      if (dy_top > h - bot_depth)     continue;

      const uint16_t color = (millis() < s_eye_flash_until_ms)
                              ? COLOR_EYE_FLASH : COLOR_EYE;
      g->fillRect(gx, gy, CELL_FILL, CELL_FILL, color);
    }
  }
}

// Draw a pixel bitmap centered at (center_x, center_y).
void draw_bitmap_centered(int center_x, int center_y,
                           int cols, int rows,
                           const uint16_t* row_bits,
                           uint16_t color) {
  if (cols == 0 || rows == 0) return;
  Arduino_GFX* g = robimon::hal::display::gfx();
  const int origin_x = center_x - (cols * CELL_PX) / 2;
  const int origin_y = center_y - (rows * CELL_PX) / 2;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      if ((row_bits[row] >> col) & 1) {
        g->fillRect(origin_x + col * CELL_PX,
                    origin_y + row * CELL_PX,
                    CELL_FILL, CELL_FILL, color);
      }
    }
  }
}

void draw_mouth(const MouthDef* m) {
  if (!m || m->cols == 0) return;
  draw_bitmap_centered(MOUTH_CX, s_mouth_cy, m->cols, m->rows, m->row_bits, COLOR_MOUTH);
}

// Compute the panel-space center of a menu item by index.
// Items are placed around the canvas center; we convert to panel coords by
// adding the canvas-vs-panel y offset for the caller.
void menu_item_center_canvas(int idx, int* out_cx, int* out_cy) {
  Arduino_GFX* g = robimon::hal::display::gfx();
  const int center_x = g->width()  / 2;
  const int center_y = g->height() / 2;
  // Start at top (-90°) so the first item sits "12 o'clock"; sweep clockwise.
  const float angle = -1.5707963f + (2.0f * 3.14159265f * idx / MENU_COUNT);
  *out_cx = center_x + (int)(MENU_RADIUS * cosf(angle));
  *out_cy = center_y + (int)(MENU_RADIUS * sinf(angle));
}

void draw_menu() {
  Arduino_GFX* g = robimon::hal::display::gfx();
  g->fillRect(0, 0, g->width(), FACE_DRAW_BOTTOM, COLOR_BG);

  const int center_x = g->width()  / 2;
  const int center_y = g->height() / 2;

  // Center hint — "how do I feel?" reads as a question to the kid rather
  // than a command. TextSize 2 (12×16 px chars).
  g->setTextColor(COLOR_EYE);
  g->setTextSize(2);
  const char* hint = "how do I feel?";
  g->setCursor(center_x - (int)(strlen(hint) * 6), center_y - 8);
  g->print(hint);

  // Items: outline ring + centered label at TextSize 3 (18×24 px chars).
  for (int i = 0; i < MENU_COUNT; ++i) {
    int cx, cy;
    menu_item_center_canvas(i, &cx, &cy);
    // Subtle ring outline (2 px stroke) — frames the hit target without
    // dominating visually now that the labels are large.
    g->drawCircle(cx, cy,     MENU_ITEM_R,     COLOR_EYE);
    g->drawCircle(cx, cy,     MENU_ITEM_R - 1, COLOR_EYE);

    g->setTextColor(COLOR_EYE);
    g->setTextSize(3);
    const char* lbl = MENU_ITEMS[i].label;
    const int text_w = (int)strlen(lbl) * 18;   // 18 px per char at size 3
    g->setCursor(cx - text_w / 2, cy - 12);
    g->print(lbl);
  }
}

// Robot mouth: cyan rounded frame with 9 vertical bars inside (vocoder /
// speaker grille). Centered on the canvas at s_mouth_cy. The whole frame
// is the hit target for voice mode — see on_tap().
void draw_robot_mouth(Arduino_GFX* g) {
  const int x = (g->width() - MOUTH_BTN_W) / 2;
  const int y = s_mouth_cy - MOUTH_BTN_H / 2;

  // Outer frame (2 px stroke)
  g->drawRoundRect(x,     y,     MOUTH_BTN_W,     MOUTH_BTN_H,     8, COLOR_EYE);
  g->drawRoundRect(x + 1, y + 1, MOUTH_BTN_W - 2, MOUTH_BTN_H - 2, 7, COLOR_EYE);

  // Vertical bars centered inside the frame
  const int bars_total_w = MOUTH_BAR_COUNT * MOUTH_BAR_W
                         + (MOUTH_BAR_COUNT - 1) * MOUTH_BAR_GAP;
  const int bars_x = x + (MOUTH_BTN_W - bars_total_w) / 2;
  const int bars_y = y + (MOUTH_BTN_H - MOUTH_BAR_H) / 2;
  for (int i = 0; i < MOUTH_BAR_COUNT; ++i) {
    g->fillRect(bars_x + i * (MOUTH_BAR_W + MOUTH_BAR_GAP),
                bars_y, MOUTH_BAR_W, MOUTH_BAR_H, COLOR_EYE);
  }
}

void render_face(const FaceParams& p) {
  Arduino_GFX* g = robimon::hal::display::gfx();
  // Skip the top 46 px so the screen-manager caption overlay (y 6..42) is
  // preserved across face frames. Without this the face's 30 FPS clears
  // would clobber the caption between renders.
  g->fillRect(0, 46, g->width(), FACE_DRAW_BOTTOM - 46, COLOR_BG);

  const int wl = (int)(EYE_W_BASE * p.left.scale);
  const int hl = (int)(EYE_H_BASE * p.left.scale);
  const int wr = (int)(EYE_W_BASE * p.right.scale);
  const int hr = (int)(EYE_H_BASE * p.right.scale);

  draw_pixelated_eye(LEFT_EYE_CX,  s_face_cy, wl, hl, /*is_left=*/true,
                     p.left.top_lid_outer,  p.left.top_lid_inner,
                     p.left.bot_lid_outer,  p.left.bot_lid_inner);
  draw_pixelated_eye(RIGHT_EYE_CX, s_face_cy, wr, hr, /*is_left=*/false,
                     p.right.top_lid_outer, p.right.top_lid_inner,
                     p.right.bot_lid_outer, p.right.bot_lid_inner);

  // Robot mouth + voice button. When an alarm label is showing, the label
  // takes the mouth area's space so the two don't fight.
  if (s_label[0]) {
    g->setTextSize(3);
    g->setTextColor(COLOR_EYE);
    const int lw = (int)strlen(s_label) * 18;
    g->setCursor(g->width() / 2 - lw / 2, s_mouth_cy - 12);
    g->print(s_label);
  } else {
    draw_robot_mouth(g);

    // Listening countdown — a depleting arc above the mouth so the kid
    // can see how long they have left to talk. A top-half semicircle
    // (rather than a full ring) keeps the arc inside the canvas band
    // and clear of the bottom dot indicator. Sweeps from 9-o'clock to
    // 3-o'clock; depletes by shortening from the right side first.
    if (s_listen_ring_window_ms) {
      const uint32_t now = millis();
      const uint32_t elapsed = (now > s_listen_ring_start_ms)
                                ? now - s_listen_ring_start_ms : 0;
      if (elapsed >= s_listen_ring_window_ms) {
        s_listen_ring_window_ms = 0;
      } else {
        const float frac = 1.0f - (float)elapsed / (float)s_listen_ring_window_ms;
        const float sweep = frac * 180.0f;
        const int   r1 = MOUTH_BTN_W / 2 + 6;
        const int   r2 = r1 - 4;
        g->fillArc(g->width() / 2, s_mouth_cy, r1, r2,
                   -180.0f, -180.0f + sweep, COLOR_EYE);
      }
    }
  }
}

void draw_flash() {
  Arduino_GFX* g = robimon::hal::display::gfx();
  g->fillRect(0, 0, g->width(), FACE_DRAW_BOTTOM, COLOR_BG);
  g->setTextColor(COLOR_EYE);
  g->setTextSize(4);   // 24×32 px chars
  const int text_w = (int)strlen(s_flash_text) * 24;
  g->setCursor(g->width() / 2 - text_w / 2, g->height() / 2 - 16);
  g->print(s_flash_text);
}

void render(const FaceParams& p) {
  Arduino_GFX* g = robimon::hal::display::gfx();
  if (!g) return;

  switch (s_mode) {
    case FaceMode::MENU:  draw_menu();      break;
    case FaceMode::FLASH: draw_flash();     break;
    case FaceMode::IDLE:
    default:              render_face(p);   break;
  }

  robimon::hal::display::flush();
}

// =============================================================================
// Idle behaviors
// =============================================================================
void schedule_next_blink() {
  s_next_blink_ms = millis() + 2000 + (uint32_t)random(0, 4001);
}

void apply_blink(FaceParams& p, uint32_t now) {
  if (s_blink_start_ms == 0) {
    if (now >= s_next_blink_ms) s_blink_start_ms = now;
    return;
  }
  const uint32_t bt = now - s_blink_start_ms;
  float blink_amt = 0;
  if      (bt < 90)  blink_amt = (float)bt / 90.0f;
  else if (bt < 150) blink_amt = 1.0f;
  else if (bt < 260) blink_amt = 1.0f - (float)(bt - 150) / 110.0f;
  else { s_blink_start_ms = 0; schedule_next_blink(); return; }

  p.left.top_lid_outer  = clamp01(p.left.top_lid_outer  + blink_amt);
  p.left.top_lid_inner  = clamp01(p.left.top_lid_inner  + blink_amt);
  p.right.top_lid_outer = clamp01(p.right.top_lid_outer + blink_amt);
  p.right.top_lid_inner = clamp01(p.right.top_lid_inner + blink_amt);
}

void apply_breath(FaceParams& p, uint32_t now) {
  const float k = 1.0f + sinf((float)now * (2.0f * 3.14159265f / 4000.0f)) * 0.020f;
  p.left.scale  *= k;
  p.right.scale *= k;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================
bool begin() {
  Arduino_GFX* g = robimon::hal::display::gfx();
  if (!g) {
    LOG_E(TAG, "display not initialized");
    return false;
  }
  // Layout the face elements vertically within the canvas.
  const int H = g->height();
  s_face_cy   = H * 5 / 12;          // eyes a bit above center
  s_mouth_cy  = H * 10 / 12;         // mouth near the bottom

  s_current = P_NEUTRAL;
  s_from    = P_NEUTRAL;
  s_to      = P_NEUTRAL;
  s_target_expr = Expression::NEUTRAL;

  randomSeed((uint32_t)esp_timer_get_time());
  schedule_next_blink();

  render(s_current);
  s_last_frame_ms = millis();
  LOG_I(TAG, "face up — eyes y=%d, mouth y=%d (canvas %dx%d)",
        s_face_cy, s_mouth_cy, g->width(), H);
  return true;
}

void set_expression(Expression e, uint16_t tween_ms) {
  if (e == s_target_expr && tween_ms != 0) return;
  s_from = s_current;
  s_to = preset(e);
  s_target_expr = e;
  s_tween_start_ms = millis();
  s_tween_dur_ms = tween_ms == 0 ? 1 : tween_ms;
  LOG_I(TAG, "expr -> %s (%u ms)", name_of(e), (unsigned)tween_ms);
}

void update() {
  const uint32_t now = millis();
  if ((now - s_last_frame_ms) < 33) return;
  s_last_frame_ms = now;

  // Auto-dismiss the menu after the timeout.
  if (s_mode == FaceMode::MENU && (now - s_menu_open_ms) > MENU_TIMEOUT_MS) {
    s_mode = FaceMode::IDLE;
    LOG_I(TAG, "menu auto-dismissed");
  }
  // Auto-clear the gesture flash.
  if (s_mode == FaceMode::FLASH && (now - s_flash_start_ms) > FLASH_MS) {
    s_mode = FaceMode::IDLE;
  }

  FaceParams base;
  if (s_tween_dur_ms == 0) base = s_to;
  else {
    const uint32_t elapsed = now - s_tween_start_ms;
    if (elapsed >= s_tween_dur_ms) base = s_to;
    else interp(base, s_from, s_to, ease_out_cubic((float)elapsed / (float)s_tween_dur_ms));
  }

  apply_breath(base, now);
  apply_blink(base, now);
  s_current = base;
  render(s_current);

  if (s_demo_on && s_mode == FaceMode::IDLE && now >= s_demo_next_ms) {
    static const Expression seq[] = {
      Expression::NEUTRAL,  Expression::HAPPY,    Expression::SAD,
      Expression::SLEEPY,   Expression::SURPRISED, Expression::ANGRY,
      Expression::THINKING, Expression::EXCITED,  Expression::CONFUSED,
    };
    s_demo_idx = (s_demo_idx + 1) % (sizeof(seq) / sizeof(seq[0]));
    set_expression(seq[s_demo_idx], 350);
    s_demo_next_ms = now + 3500;
  }
}

void on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - robimon::hal::display::canvas_panel_y_offset();

  if (s_mode == FaceMode::IDLE) {
    // Robot mouth hit-test takes priority over menu-open. Don't trigger
    // when an alarm label is showing (mouth isn't drawn then).
    if (!s_label[0]) {
      const int btn_x = (PANEL_W - MOUTH_BTN_W) / 2;
      const int btn_y = s_mouth_cy - MOUTH_BTN_H / 2;
      if (panel_x >= btn_x && panel_x < btn_x + MOUTH_BTN_W &&
          canvas_y >= btn_y && canvas_y < btn_y + MOUTH_BTN_H) {
        LOG_I(TAG, "mouth tapped -> voice");
        ::robimon::services::voice::start();
        ::robimon::services::tutorial::on_mouth_tap();
        return;
      }
    }

    s_mode = FaceMode::MENU;
    s_menu_open_ms = millis();
    s_demo_on = false;
    LOG_I(TAG, "menu opened");
    return;
  }

  if (s_mode == FaceMode::MENU) {
    for (int i = 0; i < MENU_COUNT; ++i) {
      int cx, cy;
      menu_item_center_canvas(i, &cx, &cy);
      const int dx = panel_x - cx;
      const int dy = canvas_y - cy;
      if (dx * dx + dy * dy <= MENU_ITEM_R * MENU_ITEM_R) {
        LOG_I(TAG, "menu pick: %s", MENU_ITEMS[i].label);
        set_expression(MENU_ITEMS[i].expr, 350);
        s_mode = FaceMode::IDLE;
        return;
      }
    }
    s_mode = FaceMode::IDLE;
    LOG_I(TAG, "menu dismissed (tap outside)");
    return;
  }
}

void dismiss_menu() {
  if (s_mode == FaceMode::MENU) {
    s_mode = FaceMode::IDLE;
    LOG_I(TAG, "menu dismissed (double-tap)");
  }
}

bool menu_is_open() { return s_mode == FaceMode::MENU; }

void flash_text(const char* text) {
  s_flash_text = text ? text : "";
  s_flash_start_ms = millis();
  s_mode = FaceMode::FLASH;
}

void set_label(const char* text) {
  if (!text) { s_label[0] = '\0'; return; }
  strncpy(s_label, text, sizeof(s_label) - 1);
  s_label[sizeof(s_label) - 1] = '\0';
}
void clear_label() { s_label[0] = '\0'; }

void flash_eyes(uint32_t duration_ms) {
  s_eye_flash_until_ms = millis() + duration_ms;
}

void start_listening_ring(uint32_t window_ms) {
  s_listen_ring_start_ms  = millis();
  s_listen_ring_window_ms = window_ms;
}

void enable_demo_cycle(bool on) {
  s_demo_on = on;
  if (on) s_demo_next_ms = millis() + 1500;
}

const char* current_name() { return name_of(s_target_expr); }

}  // namespace robimon::face
