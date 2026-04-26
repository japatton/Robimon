#include "audio.h"
#include "board.h"
#include "../app/log.h"

extern "C" {
#include "codec/es8311.h"
}

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

namespace robimon::hal::audio {

namespace {
constexpr const char* TAG = "audio";

I2SClass        s_i2s;
es8311_handle_t s_codec      = nullptr;
uint32_t        s_sample_rate = DEFAULT_SAMPLE_RATE;
bool            s_ok          = false;
bool            s_amp_on      = false;

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

  // Start at low volume — the project spec says voice playback is capped at
  // ~70 %, but boot-time we don't know what's about to play. 30 % is a kid-safe
  // floor; set_volume_percent() ramps it up as the app needs.
  int actually_set = 0;
  es8311_voice_volume_set(s_codec, 30, &actually_set);

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

  s_ok = true;
  LOG_I(TAG, "ES8311 + I2S up @ %lu Hz", (unsigned long)sample_rate);
  return true;
}

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
  // Read stereo and keep just the left channel (the analog mic feeds L on this codec config).
  static constexpr size_t CHUNK = 256;
  int16_t stereo[CHUNK * 2];
  size_t got = 0;
  while (got < max_count) {
    const size_t want = (max_count - got) > CHUNK ? CHUNK : (max_count - got);
    // ESP_I2S exposes readBytes(char*, size_t) for bulk reads; the bare
    // read() returns a single byte and isn't what we want here.
    const size_t bytes = s_i2s.readBytes((char*)stereo, want * 4);
    if (bytes == 0) break;
    const size_t frames = bytes / 4;
    for (size_t i = 0; i < frames; ++i) samples[got + i] = stereo[i * 2 + 0];
    got += frames;
  }
  return got;
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
