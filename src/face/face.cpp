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
// Two large smooth rounded-rectangle eyes. No mouth. No cheeks. The whole
// face vocabulary is two shapes that morph: scale, lid coverage (with slant
// for furrowed brows), and a gaze offset that drifts gently and tracks
// touches. Inspired by Cozmo / Vector / EVE — the design principle is
// that two simple shapes plus motion communicate more emotion than a
// detailed kawaii face. ("Simpler is better" — Carlos Baena, Pixar/Anki.)
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

// Eye geometry — smooth rounded rectangles. Corner radius is ~20% of the
// shorter edge, which is the proportion the Cozmo / EVE eye references
// converge on. No more cell grid: a single fillRoundRect per eye.
constexpr int EYE_W_BASE     = 130;
constexpr int EYE_H_BASE     = 130;
constexpr int EYE_CORNER_R   = 26;       // ~20 % of edge

// Maximum gaze offset (in pixels, applied to the eye center). Drift uses
// a small fraction of this; touch-glance uses the full amount.
constexpr int GAZE_MAX_DX    = 18;
constexpr int GAZE_MAX_DY    = 8;

// Vertical centers within the canvas. Set in begin() from canvas height.
int s_face_cy   = 160;

// Reserve the bottom of the canvas for screen-manager overlays (carousel
// dots). Face renderers fillRect to FACE_DRAW_BOTTOM rather than fillScreen
// — clobbering the dots between the face flush and screen_mgr's flush_band
// otherwise produces a visible flicker at 30 FPS.
constexpr int FACE_DRAW_BOTTOM = 300;

// Y-coordinate (canvas-local) where the listen-cue ring is centered, and
// the boundary between "tap on eye area = voice" vs. "tap below eyes =
// expression menu". The mouth used to live around y = 270.
constexpr int LISTEN_RING_CY   = 270;
constexpr int VOICE_TAP_Y_MAX  = 220;     // anything ≤ this in the canvas → voice

// Color palette — sci-fi blue on black. RGB565.
//   0x055F = R=0, G=168, B=248 → electric blue-cyan; reads as "robot LED".
constexpr uint16_t COLOR_BG        = 0x0000;
constexpr uint16_t COLOR_EYE       = 0x055F;
constexpr uint16_t COLOR_EYE_FLASH = 0xFFFF;   // white — listen cue accent

// =============================================================================
// Eye / face params
// =============================================================================
// Each eye is a rounded shape parameterized by independent X / Y scale, a
// vertical offset (for "looking up / down" effects), corner radius (so
// "awe" / "surprised" eyes can be more oval), and four lid depths for
// fine-grain slant when needed. Per the Cozmo expression chart, most
// expressions are conveyed by HEIGHT and POSITION changes, not slanted
// lids — so most presets touch only scale_y / y_offset and leave the
// lid params at zero. Slants stay available for sad / angry brows.
//
// No mouth field — every charming-but-robotic face reference (Cozmo, EVE,
// Vector, BB-8, WALL-E, Baymax) is mouthless. Talk affordance is the full
// eye area as a tap target; speaking emotion is conveyed via eye animation.
struct EyeShape {
  float scale_x;          // horizontal stretch (1.0 = base width)
  float scale_y;          // vertical stretch  (1.0 = base height)
  float y_offset;         // vertical shift in fractions of base height (+ = down)
  float corner_frac;      // corner radius as fraction of shorter edge (default 0.20)
  float top_lid_outer;    // residual lid masking for slant accents
  float top_lid_inner;
  float bot_lid_outer;
  float bot_lid_inner;
};
struct FaceParams {
  EyeShape left;
  EyeShape right;
};

// Helpers — most expressions only need scale_x, scale_y, y_offset.
inline EyeShape simple(float sx, float sy, float yoff = 0.0f, float cr = 0.20f) {
  return { sx, sy, yoff, cr, 0, 0, 0, 0 };
}
inline EyeShape brow(float sx, float sy, float yoff,
                     float to, float ti, float bo = 0.0f, float bi = 0.0f,
                     float cr = 0.20f) {
  return { sx, sy, yoff, cr, to, ti, bo, bi };
}

// =============================================================================
// Expression presets — patterned on the Cozmo expression sheet
// =============================================================================
// Default = simple(1, 1, 0). Where the chart shows shorter eyes, scale_y
// drops. Where it shows positional shifts (looking up / down), y_offset is
// non-zero. Slant only appears in sad/angry/glee where the chart has it.

const FaceParams P_NEUTRAL   = { simple(1.00f, 1.00f),
                                  simple(1.00f, 1.00f) };

// Cozmo "Happy / Glee" = short eyes (a bowed-up bottom). Shrunk vertically,
// slight downward shift so the bowed bottom reads naturally.
const FaceParams P_HAPPY     = { simple(1.00f, 0.55f, 0.10f),
                                  simple(1.00f, 0.55f, 0.10f) };

// Cozmo "Sad (looking down)" = moderate-height eyes with mild outward droop.
// Slant is gentle (0.25 outer top vs 0.05 inner) to avoid sharp diagonals.
const FaceParams P_SAD       = { brow(0.95f, 0.85f, +0.05f, 0.25f, 0.05f),
                                  brow(0.95f, 0.85f, +0.05f, 0.05f, 0.25f) };

// Cozmo "Sleepy Eyes" = very short eyes near the bottom of the area.
const FaceParams P_SLEEPY    = { simple(0.95f, 0.30f, +0.25f),
                                  simple(0.95f, 0.30f, +0.25f) };

// Cozmo "Awe / Surprised" = larger, more circular eyes.
const FaceParams P_SURPRISED = { simple(1.10f, 1.15f, 0.0f, 0.40f),
                                  simple(1.10f, 1.15f, 0.0f, 0.40f) };

// Cozmo "Angry" = moderate height with a "v" inward brow.
const FaceParams P_ANGRY     = { brow(0.95f, 0.75f, 0.0f, 0.05f, 0.35f),
                                  brow(0.95f, 0.75f, 0.0f, 0.35f, 0.05f) };

// Cozmo "Squint / Focused" = short and slightly sloped — a determined look.
const FaceParams P_THINKING  = { brow(1.00f, 0.55f, 0.0f, 0.10f, 0.20f),
                                  simple(0.95f, 0.55f, 0.0f) };

// Like Happy but eye is bigger overall — "yay!"
const FaceParams P_EXCITED   = { simple(1.05f, 0.65f, 0.10f),
                                  simple(1.05f, 0.65f, 0.10f) };

// Cozmo "Skeptic / Worried" = asymmetric eye size (one raised brow).
const FaceParams P_CONFUSED  = { simple(1.05f, 1.00f),
                                  simple(0.80f, 0.65f, +0.10f) };

// Listening = larger, attentive — full eye open, slightly bigger.
const FaceParams P_LISTENING = { simple(1.10f, 1.10f),
                                  simple(1.10f, 1.10f) };

// Speaking = relaxed slight squint.
const FaceParams P_SPEAKING  = { simple(1.00f, 0.90f),
                                  simple(1.00f, 0.90f) };

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
  out.scale_x       = lerp(a.scale_x,       b.scale_x,       t);
  out.scale_y       = lerp(a.scale_y,       b.scale_y,       t);
  out.y_offset      = lerp(a.y_offset,      b.y_offset,      t);
  out.corner_frac   = lerp(a.corner_frac,   b.corner_frac,   t);
  out.top_lid_outer = lerp(a.top_lid_outer, b.top_lid_outer, t);
  out.top_lid_inner = lerp(a.top_lid_inner, b.top_lid_inner, t);
  out.bot_lid_outer = lerp(a.bot_lid_outer, b.bot_lid_outer, t);
  out.bot_lid_inner = lerp(a.bot_lid_inner, b.bot_lid_inner, t);
}
void interp(FaceParams& out, const FaceParams& a, const FaceParams& b, float t) {
  interp_eye(out.left,  a.left,  b.left,  t);
  interp_eye(out.right, a.right, b.right, t);
}

// =============================================================================
// Drawing
// =============================================================================
//
// Eye is rendered as a stack of 1-px-wide vertical strips. Per strip we
// compute the local lid depth (linearly interpolated outer→inner across
// the eye width) and a quarter-circle "corner offset" that rounds the
// four corners. Slanted lids (sad / angry brows) emerge from the top/bot
// depth interpolation; the corner rounding is layered on top so the lid
// edges curve smoothly into the corners instead of meeting them at hard
// diagonal lines.
//
// Cost: ~2 × eye_width = ~260 short fillRect calls per frame. Each is
// roughly a memset of ~50 bytes into the PSRAM canvas. Negligible at
// 30 FPS.
//
// -O3 on the inner per-strip loop — hottest renderer in the project, runs
// twice per frame at 30 FPS. -O3 unrolls the loop and inlines the math
// helpers; ~10-15 % win on this function specifically without bloating
// the rest of the binary (which stays at the project default -O2).
#pragma GCC push_options
#pragma GCC optimize("O3")
void draw_smooth_eye(int cx, int cy, int w, int h, bool is_left,
                     float corner_frac,
                     float top_outer, float top_inner,
                     float bot_outer, float bot_inner) {
  if (w < 8 || h < 8) return;
  Arduino_GFX* g = robimon::hal::display::gfx();

  const int x0 = cx - w / 2;
  const int y0 = cy - h / 2;
  const int y1 = y0 + h;
  // Corner radius from per-expression fraction of the shorter edge — so
  // "surprised" / "awe" can crank corners up to 0.40+ for a more circular
  // shape, while "neutral" stays at the chart's ~20 % proportion.
  const int short_edge = w < h ? w : h;
  int radius = (int)(short_edge * corner_frac);
  if (radius > w / 2) radius = w / 2;
  if (radius > h / 2) radius = h / 2;
  if (radius < 1)     radius = 1;

  const uint16_t fill = (millis() < s_eye_flash_until_ms)
                          ? COLOR_EYE_FLASH : COLOR_EYE;

  // For the LEFT eye, "outer" is the left side (x=x0) and "inner" is the
  // right side (x=x1-1). For RIGHT eye, mirrored.
  const float top_left_frac  = is_left ? top_outer : top_inner;
  const float top_right_frac = is_left ? top_inner : top_outer;
  const float bot_left_frac  = is_left ? bot_outer : bot_inner;
  const float bot_right_frac = is_left ? bot_inner : bot_outer;

  for (int dx = 0; dx < w; ++dx) {
    const int x = x0 + dx;
    const float t = (float)dx / (float)(w - 1);
    const float top_lid = top_left_frac + (top_right_frac - top_left_frac) * t;
    const float bot_lid = bot_left_frac + (bot_right_frac - bot_left_frac) * t;

    // Quarter-circle corner offset — pushes the top edge down (and the
    // bottom edge up) at the left/right edges of the eye so the corners
    // round smoothly. dx_from_corner is 0 at the very edge and `radius`
    // at the boundary of the corner zone; offset is full at edge, zero
    // at boundary.
    const int edge_dist = (dx < w - 1 - dx) ? dx : (w - 1 - dx);
    int corner = 0;
    if (edge_dist < radius) {
      const int rd = radius - edge_dist;
      const int rsq = radius * radius - rd * rd;
      // sqrt of small int — use float once.
      corner = radius - (int)sqrtf((float)rsq);
    }

    int y_top = y0 + (int)(top_lid * h) + corner;
    int y_bot = y1 - (int)(bot_lid * h) - corner;
    if (y_bot <= y_top) continue;
    g->fillRect(x, y_top, 1, y_bot - y_top, fill);
  }
}
#pragma GCC pop_options

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

// (Robot mouth removed — every charming-but-robotic face reference in the
// design research is mouthless. Eyes carry all expression. Voice tap
// affordance is now the eye area itself; see on_tap().)

// Gaze state — drift target + smoothed current value applied to the eye
// centers in render_face. Defined here so render_face below sees them at
// anonymous-namespace scope. Updated by apply_gaze (idle drift) and
// glance_at (touch tracking).
int      s_gaze_offset_x = 0;
int      s_gaze_offset_y = 0;
float    s_gaze_target_x   = 0.0f;
float    s_gaze_target_y   = 0.0f;
float    s_gaze_current_x  = 0.0f;
float    s_gaze_current_y  = 0.0f;
uint32_t s_next_drift_ms   = 0;
uint32_t s_glance_until_ms = 0;

constexpr float GAZE_DRIFT_FRAC   = 0.30f;
constexpr float GAZE_GLANCE_FRAC  = 1.00f;
constexpr float GAZE_LERP         = 0.12f;
constexpr uint32_t GLANCE_MS      = 700;

void schedule_next_drift(uint32_t now) {
  s_next_drift_ms = now + 2000 + (uint32_t)random(0, 3001);
  const float ax = GAZE_MAX_DX * GAZE_DRIFT_FRAC;
  const float ay = GAZE_MAX_DY * GAZE_DRIFT_FRAC;
  s_gaze_target_x = ((float)random(-1000, 1001) / 1000.0f) * ax;
  s_gaze_target_y = ((float)random(-1000, 1001) / 1000.0f) * ay;
}

void apply_gaze(uint32_t now) {
  if (s_glance_until_ms != 0 && now >= s_glance_until_ms) {
    s_glance_until_ms = 0;
    schedule_next_drift(now);
  }
  if (s_glance_until_ms == 0 && now >= s_next_drift_ms) {
    schedule_next_drift(now);
  }
  s_gaze_current_x += (s_gaze_target_x - s_gaze_current_x) * GAZE_LERP;
  s_gaze_current_y += (s_gaze_target_y - s_gaze_current_y) * GAZE_LERP;
  s_gaze_offset_x = (int)s_gaze_current_x;
  s_gaze_offset_y = (int)s_gaze_current_y;
}

void render_face(const FaceParams& p) {
  Arduino_GFX* g = robimon::hal::display::gfx();
  // Skip the top 46 px so the screen-manager caption overlay (y 6..42) is
  // preserved across face frames. Without this the face's 30 FPS clears
  // would clobber the caption between renders.
  g->fillRect(0, 46, g->width(), FACE_DRAW_BOTTOM - 46, COLOR_BG);

  const int wl = (int)(EYE_W_BASE * p.left.scale_x);
  const int hl = (int)(EYE_H_BASE * p.left.scale_y);
  const int wr = (int)(EYE_W_BASE * p.right.scale_x);
  const int hr = (int)(EYE_H_BASE * p.right.scale_y);

  // y_offset is in fractions of the BASE eye height — so a 0.10 offset
  // moves the eye down by 13 px regardless of how much the eye has been
  // shrunk vertically. This keeps "looking down" looking the same amount
  // whether eyes are small or large.
  const int gx = s_gaze_offset_x;
  const int gy = s_gaze_offset_y;
  const int yo_left  = (int)(p.left.y_offset  * EYE_H_BASE);
  const int yo_right = (int)(p.right.y_offset * EYE_H_BASE);

  draw_smooth_eye(LEFT_EYE_CX  + gx, s_face_cy + gy + yo_left,  wl, hl,
                  /*is_left=*/true,  p.left.corner_frac,
                  p.left.top_lid_outer,  p.left.top_lid_inner,
                  p.left.bot_lid_outer,  p.left.bot_lid_inner);
  draw_smooth_eye(RIGHT_EYE_CX + gx, s_face_cy + gy + yo_right, wr, hr,
                  /*is_left=*/false, p.right.corner_frac,
                  p.right.top_lid_outer, p.right.top_lid_inner,
                  p.right.bot_lid_outer, p.right.bot_lid_inner);

  // Alarm label takes precedence over voice cues — alarm is highest-priority
  // anyway and shows the alarm name where the mouth used to be.
  if (s_label[0]) {
    g->setTextSize(3);
    g->setTextColor(COLOR_EYE);
    const int lw = (int)strlen(s_label) * 18;
    g->setCursor(g->width() / 2 - lw / 2, LISTEN_RING_CY - 12);
    g->print(s_label);
    return;
  }

  // Listening countdown — depleting top-half arc centered below the eyes
  // (where the mouth used to be). The face has no mouth now, so this is the
  // only voice-flow visual that lives in the lower half of the canvas.
  if (s_listen_ring_window_ms) {
    const uint32_t now = millis();
    const uint32_t elapsed = (now > s_listen_ring_start_ms)
                              ? now - s_listen_ring_start_ms : 0;
    if (elapsed >= s_listen_ring_window_ms) {
      s_listen_ring_window_ms = 0;
    } else {
      const float frac = 1.0f - (float)elapsed / (float)s_listen_ring_window_ms;
      const float sweep = frac * 180.0f;
      const int   r1 = 80;
      const int   r2 = r1 - 4;
      g->fillArc(g->width() / 2, LISTEN_RING_CY, r1, r2,
                 -180.0f, -180.0f + sweep, COLOR_EYE);
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
    case FaceMode::MENU:
      draw_menu();
      // Menu items orbit at radius 125 — they reach y ~10 at top and y
      // ~285 at bottom. Plus the radial-item radius 50, the visible
      // band is roughly y 0..320. Push the full canvas.
      robimon::hal::display::flush();
      break;
    case FaceMode::FLASH:
      draw_flash();
      robimon::hal::display::flush();
      break;
    case FaceMode::IDLE:
    default:
      render_face(p);
      // Face content lives in y 46..300 — exactly between the caption
      // band (y 6..42) and the dot indicator (y 304..316), both of
      // which the screen-manager owns and pushes via its own
      // flush_band calls. Pushing only the face band saves ~20 % of
      // QSPI bandwidth at 30 FPS (was 30 ms full-canvas flush; now
      // ~24 ms for the 254-row band).
      robimon::hal::display::flush_band(46, FACE_DRAW_BOTTOM - 46);
      break;
  }
}

// =============================================================================
// Idle behaviors — blink, breath, gaze drift, gaze tracking
// =============================================================================
//
// Per the design research (Cozmo / Vector references), idle micro-motion is
// the single load-bearing element of "the bot is alive." The trio:
//   - Blink: jittered every ~3-7 s, ~310 ms total with a held-closed plateau
//   - Breath: subtle ±2 % scale sine over 4 s
//   - Gaze drift: small offset that wanders to a new random target every
//                 2-5 s, smoothed so the eyes appear to track imaginary
//                 things rather than teleport.
//
// Gaze tracking on touch: when on_tap fires, glance_at(panel_x, panel_y) sets
// the drift target to point at where the kid touched, with full amplitude.
// After ~700 ms the glance fades and ordinary drift resumes.

void schedule_next_blink() {
  // 3-7 s jitter (tighter floor than the previous 2-6 s, slower-blinking
  // bots feel calmer and less twitchy).
  s_next_blink_ms = millis() + 3000 + (uint32_t)random(0, 4001);
}

void apply_blink(FaceParams& p, uint32_t now) {
  if (s_blink_start_ms == 0) {
    if (now >= s_next_blink_ms) s_blink_start_ms = now;
    return;
  }
  // 310 ms total: 90 ms close, 100 ms held, 120 ms open. The held-closed
  // plateau is what makes a blink feel like a blink rather than a flicker
  // — Cozmo, Vector, and EVE all hold for ~80-120 ms.
  const uint32_t bt = now - s_blink_start_ms;
  float blink_amt = 0;
  if      (bt < 90)  blink_amt = (float)bt / 90.0f;
  else if (bt < 190) blink_amt = 1.0f;
  else if (bt < 310) blink_amt = 1.0f - (float)(bt - 190) / 120.0f;
  else { s_blink_start_ms = 0; schedule_next_blink(); return; }

  p.left.top_lid_outer  = clamp01(p.left.top_lid_outer  + blink_amt);
  p.left.top_lid_inner  = clamp01(p.left.top_lid_inner  + blink_amt);
  p.right.top_lid_outer = clamp01(p.right.top_lid_outer + blink_amt);
  p.right.top_lid_inner = clamp01(p.right.top_lid_inner + blink_amt);
}

void apply_breath(FaceParams& p, uint32_t now) {
  const float k = 1.0f + sinf((float)now * (2.0f * 3.14159265f / 4000.0f)) * 0.020f;
  p.left.scale_x  *= k;  p.left.scale_y  *= k;
  p.right.scale_x *= k;  p.right.scale_y *= k;
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
  // Eyes a bit above center vertically; the lower half of the canvas is
  // empty space (no mouth) used by the listening ring + alarm label.
  const int H = g->height();
  s_face_cy = H * 5 / 12;

  s_current = P_NEUTRAL;
  s_from    = P_NEUTRAL;
  s_to      = P_NEUTRAL;
  s_target_expr = Expression::NEUTRAL;

  randomSeed((uint32_t)esp_timer_get_time());
  schedule_next_blink();
  schedule_next_drift(millis());

  render(s_current);
  s_last_frame_ms = millis();
  LOG_I(TAG, "face up — eyes y=%d (canvas %dx%d)", s_face_cy, g->width(), H);
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

  // Track mode transitions out of MENU / FLASH so we can wipe the canvas
  // once. The menu's items orbit the canvas at radius 125 (y range ~10–285),
  // which extends above and below the per-frame face fillRect (y 46..300).
  // Without an explicit clear on dismiss, the menu's outer edges leak
  // through into subsequent face frames.
  static FaceMode s_prev_mode = FaceMode::IDLE;
  const FaceMode entering = s_mode;

  // Auto-dismiss the menu after the timeout.
  if (s_mode == FaceMode::MENU && (now - s_menu_open_ms) > MENU_TIMEOUT_MS) {
    s_mode = FaceMode::IDLE;
    LOG_I(TAG, "menu auto-dismissed");
  }
  // Auto-clear the gesture flash.
  if (s_mode == FaceMode::FLASH && (now - s_flash_start_ms) > FLASH_MS) {
    s_mode = FaceMode::IDLE;
  }

  if (s_prev_mode != FaceMode::IDLE && s_mode == FaceMode::IDLE) {
    Arduino_GFX* g = robimon::hal::display::gfx();
    if (g) {
      // Clear the entire canvas — the menu wheel orbits the eye center
      // at radius ~125 and can draw below FACE_DRAW_BOTTOM (300). Then
      // push once so the panel reflects the cleared state; the per-frame
      // render_face flush_band only covers y 46..300, so without this
      // wipe the dot-band gap rows (y 300..303, y 316..) keep menu pixels.
      g->fillScreen(COLOR_BG);
      robimon::hal::display::flush();
    }
  }
  s_prev_mode = entering;

  FaceParams base;
  if (s_tween_dur_ms == 0) base = s_to;
  else {
    const uint32_t elapsed = now - s_tween_start_ms;
    if (elapsed >= s_tween_dur_ms) base = s_to;
    else interp(base, s_from, s_to, ease_out_cubic((float)elapsed / (float)s_tween_dur_ms));
  }

  apply_breath(base, now);
  apply_blink(base, now);
  apply_gaze(now);
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
    // Eye-area = voice ("look me in the eye, I'm listening"). Below-eyes =
    // expression menu. The split lives at VOICE_TAP_Y_MAX which sits where
    // the mouth used to be (canvas y ≈ 220). Suppressed when an alarm
    // label is showing — the alarm has priority.
    if (!s_label[0] && canvas_y >= 0 && canvas_y < VOICE_TAP_Y_MAX) {
      LOG_I(TAG, "face tapped (eye area) -> voice");
      ::robimon::services::voice::start();
      ::robimon::services::tutorial::on_mouth_tap();
      return;
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

void glance_at(int panel_x, int panel_y) {
  // Convert panel coords to a normalized eye-offset target. The eye center
  // is at LEFT_EYE_CX/RIGHT_EYE_CX horizontally and s_face_cy (canvas-local)
  // vertically. Use the canvas geometric center as the reference so glances
  // look natural for taps anywhere on the screen.
  const int canvas_y = panel_y - robimon::hal::display::canvas_panel_y_offset();
  const float dx = (float)(panel_x - PANEL_W / 2);
  const float dy = (float)(canvas_y - s_face_cy);
  // Map ~1/3 of the canvas extent to full gaze deflection.
  constexpr float SCALE_X = (float)PANEL_W / 6.0f;   // ≈ 78 px → full
  constexpr float SCALE_Y = 80.0f;                    // canvas height / 4
  float fx = dx / SCALE_X; if (fx > 1) fx = 1; if (fx < -1) fx = -1;
  float fy = dy / SCALE_Y; if (fy > 1) fy = 1; if (fy < -1) fy = -1;
  s_gaze_target_x   = fx * GAZE_MAX_DX * GAZE_GLANCE_FRAC;
  s_gaze_target_y   = fy * GAZE_MAX_DY * GAZE_GLANCE_FRAC;
  s_glance_until_ms = millis() + GLANCE_MS;
}

void enable_demo_cycle(bool on) {
  s_demo_on = on;
  if (on) s_demo_next_ms = millis() + 1500;
}

const char* current_name() { return name_of(s_target_expr); }

}  // namespace robimon::face
