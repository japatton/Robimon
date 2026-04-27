#include "audio.h"
#include "board.h"
#include "../app/log.h"

extern "C" {
#include "codec/es8311.h"
}
#include "codec/es7210.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace robimon::hal::audio {

namespace {
constexpr const char* TAG = "audio";

I2SClass        s_i2s;
es8311_handle_t s_codec      = nullptr;
uint32_t        s_sample_rate = DEFAULT_SAMPLE_RATE;
bool            s_ok          = false;
bool            s_amp_on      = false;

// ES7210 mic capture uses the same ESP_I2S peripheral as ES8311 playback —
// we can't run both simultaneously (shared BCLK + WS pins) but switching
// between modes is cheap. ES7210 needs its own MCLK pin (GPIO 16) since
// the board wires the two chips' MCLKs separately.
constexpr int  ES7210_MCLK_PIN = 16;
bool s_es7210_ready = false;
bool s_capture_mode = false;

// ---- Alarm playback state -------------------------------------------------
TaskHandle_t        s_alarm_task = nullptr;
std::atomic<bool>   s_alarm_stop{false};
std::atomic<bool>   s_alarm_running{false};

esp_err_t init_codec(uint32_t sample_rate) {
  // ES8311 lives on Wire (I2C port 0). The es8311 driver uses the low-level
  // i2cWrite/Read helpers under the hood, so it inherits whatever Wire was
  // brought up with in main.
  s_codec = es8311_create(0, ES8311_ADDRESS_0);
  if (!s_codec) return ESP_FAIL;

  const es8311_clock_config_t clk = {
    .mclk_inverted        = false,
    .sclk_inverted        = false,
    .mclk_from_mclk_pin   = true,
    .mclk_frequency       = sample_rate * 256,   // standard ES8311 MCLK ratio
    .sample_frequency     = (int)sample_rate,
  };
  esp_err_t r;
  r = es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (r != ESP_OK) return r;

  r = es8311_sample_frequency_config(s_codec, clk.mclk_frequency, clk.sample_frequency);
  if (r != ESP_OK) return r;

  // Use the analog mic path on the ES8311 (digital_mic = false). Routing the
  // dual mic array via ES7210 happens in a later stage.
  r = es8311_microphone_config(s_codec, false);
  if (r != ESP_OK) return r;
  es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_18DB);

  // Boot at the project's spec cap (70 %). Anything quieter and voice TTS is
  // hard to hear at 1-2 m on the small NS4150B speaker. Per-call boosts
  // (test tone, alarm) still bump to MAX_VOLUME_PERCENT and restore.
  int actually_set = 0;
  es8311_voice_volume_set(s_codec, MAX_VOLUME_PERCENT, &actually_set);

  return ESP_OK;
}

}  // namespace

bool begin(uint32_t sample_rate) {
  using namespace robimon::board;

  s_sample_rate = sample_rate;

  // PA gate as output, default OFF. Bring the I2S clocks up first, then the
  // amp, otherwise the codec's idle-output bias can pop the speaker.
  pinMode(AUDIO_PA_CTRL, OUTPUT);
  digitalWrite(AUDIO_PA_CTRL, LOW);
  s_amp_on = false;

  s_i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!s_i2s.begin(I2S_MODE_STD, sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    LOG_E(TAG, "I2S begin failed");
    return false;
  }

  if (init_codec(sample_rate) != ESP_OK) {
    LOG_E(TAG, "ES8311 codec init failed");
    return false;
  }

  // Initialize ES7210 over I2C — it doesn't take I2S yet (that happens in
  // capture()) but the codec's analog/clock config gets done here.
  audio_hal_codec_config_t adc_cfg = {};
  adc_cfg.adc_input  = AUDIO_HAL_ADC_INPUT_ALL;
  adc_cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  adc_cfg.i2s_iface.mode    = AUDIO_HAL_MODE_SLAVE;
  adc_cfg.i2s_iface.fmt     = AUDIO_HAL_I2S_NORMAL;
  adc_cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  adc_cfg.i2s_iface.bits    = AUDIO_HAL_BIT_LENGTH_16BITS;

  esp_err_t rv = es7210_adc_init(&Wire, &adc_cfg);
  rv |= es7210_adc_config_i2s(adc_cfg.codec_mode, &adc_cfg.i2s_iface);
  // MIC1 + MIC2 are the dual mic array on this board. Push to max gain
  // (37.5 dB) — Waveshare's reference example uses the same. Distortion is
  // easier to diagnose than silence; we can dial back later if voice clips.
  rv |= es7210_adc_set_gain(
      (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
      (es7210_gain_value_t)GAIN_37_5DB);
  // MIC3/4 are reserved for AEC loopback (future); leave at 0 dB.
  rv |= es7210_adc_set_gain(
      (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
      (es7210_gain_value_t)GAIN_0DB);
  rv |= es7210_adc_ctrl_state(adc_cfg.codec_mode, AUDIO_HAL_CTRL_START);
  s_es7210_ready = (rv == ESP_OK);
  LOG_I(TAG, "ES7210 init: %s", s_es7210_ready ? "ok" : "FAILED");

  s_ok = true;
  LOG_I(TAG, "ES8311 + I2S up @ %lu Hz", (unsigned long)sample_rate);
  return true;
}

// ---- I2S mode switching ----------------------------------------------------
// We can't have both ES8311 (playback) and ES7210 (capture) drive the I2S bus
// at the same time — they share BCLK + WS on this board. So we toggle the
// MCLK pin (and reinit ESP_I2S) between the two chips when switching modes.
// ES8311 MCLK = GPIO 42; ES7210 MCLK = GPIO 16.
namespace {
void enter_capture_mode() {
  if (s_capture_mode) return;
  s_i2s.end();
  s_i2s.setPins(robimon::board::I2S_BCLK, robimon::board::I2S_WS,
                robimon::board::I2S_DOUT, robimon::board::I2S_DIN,
                ES7210_MCLK_PIN);
  s_i2s.begin(I2S_MODE_STD, s_sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
              I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  s_capture_mode = true;
}

void exit_capture_mode() {
  if (!s_capture_mode) return;
  s_i2s.end();
  s_i2s.setPins(robimon::board::I2S_BCLK, robimon::board::I2S_WS,
                robimon::board::I2S_DOUT, robimon::board::I2S_DIN,
                robimon::board::I2S_MCLK);
  s_i2s.begin(I2S_MODE_STD, s_sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
              I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  s_capture_mode = false;
}
}  // namespace

void set_volume_percent(uint8_t percent) {
  if (!s_ok) return;
  if (percent > MAX_VOLUME_PERCENT) percent = MAX_VOLUME_PERCENT;
  // ES8311 volume is 0..100 (driver maps to register internally).
  int actually_set = 0;
  es8311_voice_volume_set(s_codec, percent, &actually_set);
}

void enable_amp(bool on) {
  digitalWrite(robimon::board::AUDIO_PA_CTRL, on ? HIGH : LOW);
  s_amp_on = on;
}

size_t play(const int16_t* samples, size_t count) {
  if (!s_ok || !samples || count == 0) return 0;
  // I2S runs stereo; duplicate the mono sample to both channels.
  // For very large buffers we'd stream; Stage B keeps it simple.
  static constexpr size_t CHUNK = 256;
  int16_t stereo[CHUNK * 2];
  size_t written = 0;
  while (written < count) {
    const size_t n = (count - written) > CHUNK ? CHUNK : (count - written);
    for (size_t i = 0; i < n; ++i) {
      stereo[i * 2 + 0] = samples[written + i];
      stereo[i * 2 + 1] = samples[written + i];
    }
    s_i2s.write((uint8_t*)stereo, n * 4);   // 4 bytes per stereo frame
    written += n;
  }
  return written;
}

size_t capture(int16_t* samples, size_t max_count) {
  if (!s_ok || !samples || max_count == 0) return 0;

  enter_capture_mode();

  // ES7210 with MIC1 + MIC2 produces stereo I2S frames (MIC1 on left,
  // MIC2 on right); we keep just MIC1.
  static constexpr size_t CHUNK_FRAMES = 256;
  int16_t stereo[CHUNK_FRAMES * 2];
  size_t got = 0;
  while (got < max_count) {
    const size_t want_frames = (max_count - got) > CHUNK_FRAMES
                                ? CHUNK_FRAMES : (max_count - got);
    const size_t bytes = s_i2s.readBytes((char*)stereo, want_frames * 4);
    if (bytes == 0) break;
    const size_t frames = bytes / 4;
    for (size_t i = 0; i < frames; ++i) samples[got + i] = stereo[i * 2 + 0];
    got += frames;
  }

  exit_capture_mode();
  return got;
}

// Generates one tone (sine wave w/ short fade in/out) into the supplied
// buffer. Returns the number of samples written.
size_t generate_tone(int16_t* buf, uint32_t freq_hz, uint32_t ms, float amplitude) {
  if (!buf) return 0;
  const size_t samples = (size_t)((uint64_t)s_sample_rate * ms / 1000UL);
  const float dt = 1.0f / (float)s_sample_rate;
  const size_t fade = (size_t)((uint64_t)s_sample_rate * 8 / 1000UL);
  for (size_t i = 0; i < samples; ++i) {
    const float t = (float)i * dt;
    float env = 1.0f;
    if (i < fade)               env = (float)i / (float)fade;
    if (i >= samples - fade)    env = (float)(samples - i) / (float)fade;
    buf[i] = (int16_t)(sinf(2.0f * 3.14159265f * (float)freq_hz * t)
                       * 32767.0f * amplitude * env);
  }
  return samples;
}

// Background task: pre-computes a low+high two-tone chime and loops it
// until s_alarm_stop is set. Pinned to core 0 so it doesn't compete with
// the UI/render loop on core 1.
void alarm_task(void*) {
  constexpr uint32_t LO_HZ   = 600;
  constexpr uint32_t HI_HZ   = 880;
  constexpr uint32_t TONE_MS = 180;
  constexpr uint32_t MID_GAP_MS  = 40;     // silence between the two tones
  constexpr uint32_t REST_MS     = 380;    // silence between repetitions
  constexpr float    AMPLITUDE   = 0.55f;

  const size_t lo_samples = (size_t)((uint64_t)s_sample_rate * TONE_MS / 1000UL);
  const size_t hi_samples = lo_samples;
  int16_t* lo_buf = (int16_t*)malloc(lo_samples * sizeof(int16_t));
  int16_t* hi_buf = (int16_t*)malloc(hi_samples * sizeof(int16_t));
  if (!lo_buf || !hi_buf) {
    if (lo_buf) free(lo_buf);
    if (hi_buf) free(hi_buf);
    s_alarm_running = false;
    s_alarm_task = nullptr;
    vTaskDelete(NULL);
    return;
  }
  generate_tone(lo_buf, LO_HZ, TONE_MS, AMPLITUDE);
  generate_tone(hi_buf, HI_HZ, TONE_MS, AMPLITUDE);

  enable_amp(true);
  vTaskDelay(pdMS_TO_TICKS(8));   // let the amp settle so the first sample doesn't pop

  while (!s_alarm_stop.load()) {
    play(lo_buf, lo_samples);
    if (s_alarm_stop.load()) break;
    vTaskDelay(pdMS_TO_TICKS(MID_GAP_MS));
    if (s_alarm_stop.load()) break;
    play(hi_buf, hi_samples);
    if (s_alarm_stop.load()) break;
    // Rest period — broken into small slices so we can react to stop quickly.
    for (uint32_t waited = 0; waited < REST_MS && !s_alarm_stop.load(); waited += 30) {
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }

  enable_amp(false);
  free(lo_buf);
  free(hi_buf);
  s_alarm_running = false;
  s_alarm_task = nullptr;
  vTaskDelete(NULL);
}

void start_alarm() {
  if (!s_ok) return;
  if (s_alarm_running.load()) return;       // already playing
  s_alarm_stop = false;
  s_alarm_running = true;
  // Pin to core 0 so the I2S blocking writes don't compete with the UI on
  // core 1. Stack 4 KB is plenty (we just call play() in a loop).
  xTaskCreatePinnedToCore(alarm_task, "alarm_audio", 4096, nullptr,
                           /*priority=*/1, &s_alarm_task, /*core=*/0);
}

void stop_alarm() {
  if (!s_alarm_running.load()) return;
  s_alarm_stop = true;
  // Brief join — task should self-terminate within ~1 s (worst case waiting
  // for the current play() to drain).
  for (int i = 0; i < 200 && s_alarm_running.load(); ++i) {
    delay(10);
  }
}

bool alarm_is_playing() { return s_alarm_running.load(); }

void play_listen_cue() {
  if (!s_ok) return;

  // Two-stage chirp (low → high) so it reads as "go now" rather than a
  // single ambiguous blip. NS4150B soft-start needs ~15-20 ms before the
  // first sample lands cleanly, and ESP_I2S write returns once samples are
  // queued in DMA — not when they've finished playing — so we drain
  // generously after the last tone.
  constexpr uint32_t lo_hz     = 800;
  constexpr uint32_t hi_hz     = 1200;
  constexpr uint32_t tone_ms   = 90;
  constexpr float    amplitude = 0.85f;

  const size_t samples = (size_t)((uint64_t)s_sample_rate * tone_ms / 1000UL);
  int16_t* lo_buf = (int16_t*)malloc(samples * sizeof(int16_t));
  int16_t* hi_buf = (int16_t*)malloc(samples * sizeof(int16_t));
  if (!lo_buf || !hi_buf) {
    if (lo_buf) free(lo_buf);
    if (hi_buf) free(hi_buf);
    return;
  }
  generate_tone(lo_buf, lo_hz, tone_ms, amplitude);
  generate_tone(hi_buf, hi_hz, tone_ms, amplitude);

  int saved_vol = 0;
  es8311_voice_volume_get(s_codec, &saved_vol);
  int set_to = 0;
  es8311_voice_volume_set(s_codec, MAX_VOLUME_PERCENT, &set_to);

  enable_amp(true);
  delay(25);                 // amp soft-start
  play(lo_buf, samples);
  play(hi_buf, samples);
  delay(tone_ms + 60);       // drain DMA before muting amp
  enable_amp(false);

  es8311_voice_volume_set(s_codec, saved_vol, &set_to);
  free(lo_buf);
  free(hi_buf);
}

void play_test_tone() {
  if (!s_ok) return;

  // 600 Hz sine, 500 ms, 70 % amplitude. Loud enough to hear clearly across
  // the room; well below "startle a kid" loud. The codec volume is also
  // pushed to the project max (70 %) for the duration, then restored.
  constexpr uint32_t freq_hz = 600;
  constexpr uint32_t ms = 500;
  constexpr float    amplitude = 0.70f;

  const size_t samples = (size_t)((uint64_t)s_sample_rate * ms / 1000UL);
  int16_t* buf = (int16_t*)malloc(samples * sizeof(int16_t));
  if (!buf) return;

  const float dt = 1.0f / (float)s_sample_rate;
  // Linear ramp in/out over the first/last 8 ms suppresses the click at
  // the edges (the "step" from 0 to full amplitude pops the speaker).
  const size_t fade = (size_t)((uint64_t)s_sample_rate * 8 / 1000UL);
  for (size_t i = 0; i < samples; ++i) {
    const float t = (float)i * dt;
    float env = 1.0f;
    if (i < fade)               env = (float)i / (float)fade;
    if (i >= samples - fade)    env = (float)(samples - i) / (float)fade;
    buf[i] = (int16_t)(sinf(2.0f * 3.14159265f * (float)freq_hz * t)
                       * 32767.0f * amplitude * env);
  }

  // Save and bump codec volume for the test, then restore.
  int saved_vol = 0;
  es8311_voice_volume_get(s_codec, &saved_vol);
  int set_to = 0;
  es8311_voice_volume_set(s_codec, MAX_VOLUME_PERCENT, &set_to);

  enable_amp(true);
  delay(10);            // let the amp settle before pushing samples → less popping
  play(buf, samples);
  delay(40);            // drain the I2S DMA before muting the amp
  enable_amp(false);

  es8311_voice_volume_set(s_codec, saved_vol, &set_to);
  free(buf);
}

bool ok() { return s_ok; }

}  // namespace robimon::hal::audio
