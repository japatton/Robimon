#include "voice_client.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "../hal/audio.h"
#include "../face/face.h"
#include "../app/log.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace robimon::services::voice {

namespace {
constexpr const char* TAG = "voice";

// Capture format. 16 kHz mono int16 matches what Whisper wants natively
// and what our ES8311 mic path is configured for.
constexpr uint32_t CAPTURE_RATE     = 16000;
constexpr uint32_t RECORD_SECONDS   = 5;
constexpr size_t   RECORD_SAMPLES   = CAPTURE_RATE * RECORD_SECONDS;
constexpr size_t   RECORD_BYTES     = RECORD_SAMPLES * sizeof(int16_t);
constexpr size_t   WAV_HDR_BYTES    = 44;
constexpr size_t   REQUEST_BYTES    = WAV_HDR_BYTES + RECORD_BYTES;

constexpr uint32_t HTTP_TIMEOUT_MS  = 25000;       // shorter than WDT (45 s)
constexpr size_t   MAX_RESPONSE_BYTES = 512UL * 1024UL;   // 512 KB cap on TTS WAV
constexpr uint32_t ERROR_HOLD_MS    = 1500;        // confused face dwell

// Output of the resampler (worst case): MAX_RESPONSE_BYTES of source @ 8 kHz
// upsampled to 16 kHz = 2x the source-PCM length. We size for an 8 kHz mono
// 16-bit response that fully fills MAX_RESPONSE_BYTES.
constexpr size_t   MAX_RESAMPLED_BYTES = MAX_RESPONSE_BYTES * 2;

// State as atomic so the main loop can safely poll it without taking a lock.
// The voice task is the sole writer; the main loop and start() are readers.
std::atomic<State> s_state{State::IDLE};
TaskHandle_t       s_task              = nullptr;
uint32_t           s_error_started_ms  = 0;

// ---- Pooled buffers --------------------------------------------------------
// Allocated once in begin(); used by voice_task without per-call malloc.
// Live in PSRAM (~2.1 MB total) since the canvas already lives there and
// internal RAM is too tight.
uint8_t* s_request_buf   = nullptr;   // REQUEST_BYTES (~640 KB)
uint8_t* s_response_buf  = nullptr;   // MAX_RESPONSE_BYTES (~512 KB)
int16_t* s_resampled_buf = nullptr;   // MAX_RESAMPLED_BYTES (~1 MB)

// -----------------------------------------------------------------------------
// WAV header helpers
// -----------------------------------------------------------------------------
void wr_u32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

void build_wav_header(uint8_t* hdr, uint32_t sample_rate, uint16_t channels,
                      uint16_t bits_per_sample, uint32_t pcm_bytes) {
  // RIFF
  memcpy(hdr + 0,  "RIFF", 4);
  wr_u32_le(hdr + 4, 36 + pcm_bytes);
  memcpy(hdr + 8,  "WAVE", 4);
  // fmt
  memcpy(hdr + 12, "fmt ", 4);
  wr_u32_le(hdr + 16, 16);            // PCM format chunk size
  hdr[20] = 1; hdr[21] = 0;           // PCM
  hdr[22] = (uint8_t)(channels & 0xff); hdr[23] = (uint8_t)((channels >> 8) & 0xff);
  wr_u32_le(hdr + 24, sample_rate);
  const uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
  wr_u32_le(hdr + 28, byte_rate);
  const uint16_t block_align = channels * (bits_per_sample / 8);
  hdr[32] = (uint8_t)(block_align & 0xff); hdr[33] = (uint8_t)((block_align >> 8) & 0xff);
  hdr[34] = (uint8_t)(bits_per_sample & 0xff); hdr[35] = (uint8_t)((bits_per_sample >> 8) & 0xff);
  // data
  memcpy(hdr + 36, "data", 4);
  wr_u32_le(hdr + 40, pcm_bytes);
}

// Walk the chunks in a WAV file and pull out fmt + data. More forgiving than
// a fixed 44-byte parse — Piper sometimes emits a LIST chunk between fmt and
// data that throws off naive parsers.
bool parse_wav(const uint8_t* data, size_t len,
               uint32_t* out_sample_rate, uint16_t* out_channels,
               uint16_t* out_bits_per_sample,
               size_t* out_pcm_offset, size_t* out_pcm_bytes) {
  if (len < 12) return false;
  if (memcmp(data + 0, "RIFF", 4) != 0) return false;
  if (memcmp(data + 8, "WAVE", 4) != 0) return false;

  bool   have_fmt  = false;
  bool   have_data = false;
  size_t pos = 12;
  while (pos + 8 <= len) {
    const char id[5] = { (char)data[pos], (char)data[pos+1], (char)data[pos+2], (char)data[pos+3], 0 };
    const uint32_t sz = (uint32_t)data[pos+4]
                      | ((uint32_t)data[pos+5] << 8)
                      | ((uint32_t)data[pos+6] << 16)
                      | ((uint32_t)data[pos+7] << 24);
    pos += 8;
    if (pos + sz > len) break;

    if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
      *out_channels         = (uint16_t)(data[pos + 2] | (data[pos + 3] << 8));
      *out_sample_rate      = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8)
                            | ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
      *out_bits_per_sample  = (uint16_t)(data[pos + 14] | (data[pos + 15] << 8));
      have_fmt = true;
    } else if (memcmp(id, "data", 4) == 0) {
      *out_pcm_offset = pos;
      *out_pcm_bytes  = sz;
      have_data = true;
    }
    pos += sz;
    if (sz & 1) pos++;          // chunks are word-aligned
    if (have_fmt && have_data) return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Naive linear resampler from any rate to 16 kHz (mono int16). Writes into
// the caller-supplied dst buffer (capacity in samples). Returns the number
// of samples written, or 0 if dst is too small. Speech-quality only; source
// is expected to be already band-limited (Piper output).
// -----------------------------------------------------------------------------
size_t resample_to_16k(const int16_t* src, size_t src_count, uint32_t src_rate,
                       int16_t* dst, size_t dst_capacity) {
  if (src_rate == 16000) {
    if (src_count > dst_capacity) return 0;
    memcpy(dst, src, src_count * sizeof(int16_t));
    return src_count;
  }
  const size_t dst_count = (size_t)((uint64_t)src_count * 16000ULL / src_rate);
  if (dst_count > dst_capacity) return 0;
  for (size_t i = 0; i < dst_count; ++i) {
    const uint64_t src_pos_q16 = (uint64_t)i * src_rate * 65536ULL / 16000ULL;
    const size_t   idx  = (size_t)(src_pos_q16 >> 16);
    const uint32_t frac = (uint32_t)(src_pos_q16 & 0xFFFF);
    if (idx + 1 >= src_count) {
      dst[i] = src[src_count - 1];
    } else {
      const int32_t a = src[idx];
      const int32_t b = src[idx + 1];
      dst[i] = (int16_t)(a + ((int32_t)(b - a) * (int32_t)frac >> 16));
    }
  }
  return dst_count;
}

// -----------------------------------------------------------------------------
// Face-state plumbing
// -----------------------------------------------------------------------------
void apply_face_for_state(State s) {
  using face::Expression;
  switch (s) {
    case State::RECORDING:       face::set_expression(Expression::LISTENING, 200); break;
    case State::SENDING:         face::set_expression(Expression::THINKING,  200); break;
    case State::PLAYING:         face::set_expression(Expression::SPEAKING,  200); break;
    case State::IDLE:            face::set_expression(Expression::NEUTRAL,   350); break;
    case State::ERROR_NETWORK:
    case State::ERROR_NO_SPEECH:
    case State::ERROR_OTHER:     face::set_expression(Expression::CONFUSED,  200); break;
  }
}

// Set state + apply face. Used so callers don't have to remember to call
// apply_face_for_state() everywhere.
inline void set_state(State s) {
  s_state.store(s, std::memory_order_release);
  apply_face_for_state(s);
}

// Common cleanup before the task self-deletes: clear handle, mark IDLE/error,
// detach from WDT. The handle is cleared BEFORE state changes so any reader
// that observes IDLE finds a null handle too (avoids the start() race).
inline void task_exit(State final_state) {
  if (final_state == State::ERROR_NETWORK ||
      final_state == State::ERROR_NO_SPEECH ||
      final_state == State::ERROR_OTHER) {
    s_error_started_ms = millis();
  }
  esp_task_wdt_delete(NULL);
  s_task = nullptr;
  set_state(final_state);
  vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Voice task body
// -----------------------------------------------------------------------------
void voice_task(void*) {
  esp_task_wdt_add(NULL);

  if (!s_request_buf || !s_response_buf || !s_resampled_buf) {
    LOG_E(TAG, "voice buffers not allocated");
    task_exit(State::ERROR_OTHER);
    return;
  }

  set_state(State::RECORDING);
  build_wav_header(s_request_buf, CAPTURE_RATE, /*channels=*/1, /*bps=*/16, RECORD_BYTES);

  // Audible "ready" cue. The amp settles and the cue is fully drained before
  // we start capturing so the beep doesn't bleed into the recording.
  ::robimon::hal::audio::play_listen_cue();
  // Visual sibling cues (universal-design baseline so a kid in a noisy room
  // or with hearing differences gets the same signal): brief eye flash +
  // a depleting ring around the mouth that runs for the recording window.
  face::flash_eyes(150);
  face::start_listening_ring(RECORD_SECONDS * 1000);
  delay(60);

  {
    int16_t* pcm = (int16_t*)(s_request_buf + WAV_HDR_BYTES);
    const size_t got = ::robimon::hal::audio::capture(pcm, RECORD_SAMPLES);
    esp_task_wdt_reset();

    int32_t peak = 0;
    int64_t sum_abs = 0;
    for (size_t i = 0; i < got; ++i) {
      const int32_t v = pcm[i];
      const int32_t a = v < 0 ? -v : v;
      if (a > peak) peak = a;
      sum_abs += a;
    }
    const int32_t mean_abs = got > 0 ? (int32_t)(sum_abs / got) : 0;
    LOG_I(TAG, "captured %u/%u samples, peak=%ld mean|x|=%ld",
          (unsigned)got, (unsigned)RECORD_SAMPLES,
          (long)peak, (long)mean_abs);
  }

  set_state(State::SENDING);
  {
    char url[160] = {0};
    config::get_string("voice_url", url, sizeof(url));
    if (!url[0]) { LOG_W(TAG, "no voice URL configured"); task_exit(State::ERROR_OTHER); return; }

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
      LOG_W(TAG, "http begin failed: %s", url);
      task_exit(State::ERROR_NETWORK); return;
    }
    http.addHeader("Content-Type", "audio/wav");

    char model[64]   = {0};
    char prompt[200] = {0};
    config::get_string("voice_model",  model,  sizeof(model));
    config::get_string("voice_prompt", prompt, sizeof(prompt));
    if (model[0])  http.addHeader("X-Robimon-Model",         model);
    if (prompt[0]) http.addHeader("X-Robimon-System-Prompt", prompt);

    esp_task_wdt_reset();
    const int code = http.POST(s_request_buf, REQUEST_BYTES);
    esp_task_wdt_reset();
    LOG_I(TAG, "POST -> %d", code);

    if (code == 204) { http.end(); task_exit(State::ERROR_NO_SPEECH); return; }
    if (code != 200) { http.end(); task_exit(State::ERROR_NETWORK);   return; }

    const int content_len = http.getSize();
    if (content_len <= 0 || (size_t)content_len > MAX_RESPONSE_BYTES) {
      LOG_W(TAG, "bad content length %d (cap %u)", content_len, (unsigned)MAX_RESPONSE_BYTES);
      http.end(); task_exit(State::ERROR_OTHER); return;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t total = 0;
    const uint32_t deadline = millis() + HTTP_TIMEOUT_MS;
    while (total < (size_t)content_len) {
      if (millis() > deadline) {
        LOG_W(TAG, "response read timeout @ %u/%d", (unsigned)total, content_len);
        http.end(); task_exit(State::ERROR_NETWORK); return;
      }
      const size_t avail = stream->available();
      if (avail == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_task_wdt_reset();
        continue;
      }
      const size_t want = avail < (size_t)content_len - total ? avail : (size_t)content_len - total;
      total += stream->readBytes(s_response_buf + total, want);
    }
    http.end();
    esp_task_wdt_reset();
    LOG_I(TAG, "received %u bytes", (unsigned)total);

    uint32_t resp_rate = 0;
    uint16_t resp_ch   = 0, resp_bps = 0;
    size_t   pcm_off = 0, pcm_len = 0;
    if (!parse_wav(s_response_buf, content_len, &resp_rate, &resp_ch, &resp_bps, &pcm_off, &pcm_len)) {
      LOG_W(TAG, "bad WAV in response");
      task_exit(State::ERROR_OTHER); return;
    }
    LOG_I(TAG, "WAV: %u Hz, %u ch, %u-bit, %u PCM bytes",
          (unsigned)resp_rate, (unsigned)resp_ch, (unsigned)resp_bps, (unsigned)pcm_len);

    if (resp_bps != 16 || resp_ch != 1) {
      LOG_W(TAG, "unsupported WAV format (need mono 16-bit)");
      task_exit(State::ERROR_OTHER); return;
    }

    const int16_t* src = (const int16_t*)(s_response_buf + pcm_off);
    const size_t   src_count = pcm_len / sizeof(int16_t);
    const size_t   resampled_count = resample_to_16k(
        src, src_count, resp_rate,
        s_resampled_buf, MAX_RESAMPLED_BYTES / sizeof(int16_t));
    if (resampled_count == 0) {
      LOG_W(TAG, "resampler overflow (src %u @ %u Hz)",
            (unsigned)src_count, (unsigned)resp_rate);
      task_exit(State::ERROR_OTHER); return;
    }

    set_state(State::PLAYING);
    ::robimon::hal::audio::enable_amp(true);
    delay(10);
    ::robimon::hal::audio::play(s_resampled_buf, resampled_count);
    delay(40);
    ::robimon::hal::audio::enable_amp(false);
    esp_task_wdt_reset();
  }

  task_exit(State::IDLE);
}

}  // namespace

void begin() {
  s_state.store(State::IDLE);
  s_task = nullptr;

  // Allocate the buffer pool once. Total ~2.1 MB PSRAM, sized so a worst-case
  // voice round-trip never touches malloc on the hot path.
  if (!s_request_buf) {
    s_request_buf = (uint8_t*)heap_caps_malloc(REQUEST_BYTES, MALLOC_CAP_SPIRAM);
  }
  if (!s_response_buf) {
    s_response_buf = (uint8_t*)heap_caps_malloc(MAX_RESPONSE_BYTES, MALLOC_CAP_SPIRAM);
  }
  if (!s_resampled_buf) {
    s_resampled_buf = (int16_t*)heap_caps_malloc(MAX_RESAMPLED_BYTES, MALLOC_CAP_SPIRAM);
  }
  if (!s_request_buf || !s_response_buf || !s_resampled_buf) {
    LOG_E(TAG, "voice buffer pool alloc failed");
  } else {
    LOG_I(TAG, "voice buffers ready (req=%u resp=%u rs=%u)",
          (unsigned)REQUEST_BYTES,
          (unsigned)MAX_RESPONSE_BYTES,
          (unsigned)MAX_RESAMPLED_BYTES);
  }
}

void update(uint32_t now_ms) {
  // Auto-clear the error state after a brief dwell so the user gets visual
  // feedback ("confused face") then returns to normal.
  const State cur = s_state.load(std::memory_order_acquire);
  if ((cur == State::ERROR_NETWORK ||
       cur == State::ERROR_NO_SPEECH ||
       cur == State::ERROR_OTHER) &&
      (now_ms - s_error_started_ms) > ERROR_HOLD_MS) {
    set_state(State::IDLE);
  }
}

State        state()     { return s_state.load(std::memory_order_acquire); }
const char*  state_str() {
  switch (state()) {
    case State::IDLE:            return "idle";
    case State::RECORDING:       return "recording";
    case State::SENDING:         return "sending";
    case State::PLAYING:         return "playing";
    case State::ERROR_NETWORK:   return "error_network";
    case State::ERROR_NO_SPEECH: return "error_no_speech";
    case State::ERROR_OTHER:     return "error_other";
  }
  return "?";
}

bool start() {
  // s_state is the single source of truth: IDLE means "no task running".
  // s_task is cleared BEFORE state moves to IDLE in task_exit() so this
  // ordering can't see a stale handle.
  if (s_state.load(std::memory_order_acquire) != State::IDLE) return false;
  if (wifi_mgr::state() != wifi_mgr::State::CONNECTED) {
    LOG_W(TAG, "not connected to wifi");
    s_error_started_ms = millis();
    set_state(State::ERROR_NETWORK);
    return false;
  }
  // 16 KB stack covers HTTPClient + ArduinoString + our locals.
  xTaskCreatePinnedToCore(voice_task, "voice", 16384, nullptr,
                           /*priority=*/1, &s_task, /*core=*/0);
  return true;
}

}  // namespace robimon::services::voice
