// LVGL v8.4 configuration for Robimon.
// Trimmed from the upstream template — everything we don't need is stripped to keep flash usage low.
// If you flip a feature on, document why in the comment next to it.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// ---- Color depth & display ----
// 466x466 AMOLED, 16-bit RGB565. Byte-swap on, because Arduino_GFX expects BE-ordered RGB565 over QSPI.
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
#define LV_COLOR_SCREEN_TRANSP 0

// ---- Memory ----
// Use ESP heap allocator so LVGL allocations land in PSRAM when needed.
// (Draw buffers themselves are explicitly heap_caps_malloc'd in the display HAL.)
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE   "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC(s)  heap_caps_malloc((s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(p)   heap_caps_free(p)
#define LV_MEM_CUSTOM_REALLOC(p, s) heap_caps_realloc((p), (s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEMCPY_MEMSET_STD 1

// ---- HAL settings ----
// We provide our own tick via esp_timer; LVGL just reads it through lv_tick_inc().
#define LV_TICK_CUSTOM 0
// Rendered FPS target — soft hint to LVGL only; actual FPS measured by app/stats.cpp.
#define LV_DISP_DEF_REFR_PERIOD 16   // 16 ms ≈ 60 Hz
#define LV_INDEV_DEF_READ_PERIOD 20

// ---- Drawing ----
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_DISP_ROT_MAX_BUF (32 * 1024)
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL 0

// ---- Logging — route LVGL log to the same place as our LOG_* macros.
// We hook lv_log_register_print_cb at boot to forward to Serial.
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0
#define LV_LOG_TRACE_MEM        0
#define LV_LOG_TRACE_TIMER      0
#define LV_LOG_TRACE_INDEV      0
#define LV_LOG_TRACE_DISP_REFR  0
#define LV_LOG_TRACE_EVENT      0
#define LV_LOG_TRACE_OBJ_CREATE 0
#define LV_LOG_TRACE_LAYOUT     0
#define LV_LOG_TRACE_ANIM       0

// ---- Asserts (cheap to keep on; useful for catching draw-buffer mistakes) ----
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

// ---- Features we use ----
#define LV_USE_USER_DATA 1
#define LV_USE_PERF_MONITOR 0   // we draw our own stats, off by default
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_OBJ_PROPERTY 0

// ---- Fonts: Montserrat at the sizes we need; turn off the rest. ----
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
#define LV_USE_FONT_PLACEHOLDER 1

// ---- Text & i18n ----
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

// ---- Widgets we use; everything else off to keep flash small. ----
#define LV_USE_ARC      1
#define LV_USE_BAR      1
#define LV_USE_BTN      1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS   1   // face renderer uses canvas for vector primitives
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG      1
#define LV_USE_LABEL    1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT  1
#define LV_USE_LINE     1
#define LV_USE_ROLLER   1
#define LV_USE_SLIDER   1
#define LV_USE_SWITCH   1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE    1

#define LV_USE_KEYBOARD 1   // on-screen keyboard for settings (URLs, tokens, PIN)
#define LV_USE_LIST     1
#define LV_USE_MENU     1
#define LV_USE_MSGBOX   1
#define LV_USE_TILEVIEW 1   // candidate for swipe navigation
#define LV_USE_WIN      0
#define LV_USE_SPAN     1
#define LV_USE_SPINBOX  1
#define LV_USE_SPINNER  1
#define LV_USE_TABVIEW  0
#define LV_USE_CHART    1   // diagnostics screen plots
#define LV_USE_CALENDAR 1   // alarm settings
#define LV_USE_COLORWHEEL 0
#define LV_USE_LED      0
#define LV_USE_METER    0
#define LV_USE_QRCODE   0
#define LV_USE_IMGFONT  0
#define LV_USE_ANIMIMG  1

// ---- Themes ----
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO  0

// ---- Layouts ----
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// ---- File system / image decoders — off until we need them. ----
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_PNG  0
#define LV_USE_BMP  0
#define LV_USE_SJPG 0
#define LV_USE_GIF  0
#define LV_USE_QRCODE 0

// ---- Demos: off. We have our own UI. ----
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif // LV_CONF_H
