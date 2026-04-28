#include "playback_server.h"
#include "proximity_config.h"
#include "conversation_session.h"
#include "companion_client.h"
#include "proximity_monitor.h"
#include "../hal/audio.h"
#include "../app/log.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>

namespace robimon::services::playback_server {

namespace {
constexpr const char* TAG = "play";

// Sized for the same WAV envelope the voice flow uses — Piper at 22 kHz
// for ~3-4 s comes in around ~150 KB; 512 KB cap is plenty and matches
// voice_client's MAX_RESPONSE_BYTES.
constexpr size_t MAX_INBOUND_BYTES   = 512UL * 1024UL;
constexpr size_t MAX_RESAMPLED_BYTES = MAX_INBOUND_BYTES * 2UL;   // worst case 8→16 kHz

AsyncWebServer*  s_web        = nullptr;
TaskHandle_t     s_play_task  = nullptr;

// Inbound + resample buffers (PSRAM).
uint8_t*         s_in_buf       = nullptr;
size_t           s_in_size      = 0;
int16_t*         s_resamp_buf   = nullptr;

// Per-play metadata staged by the body handler for the play worker.
char             s_pending_session[40] = {0};
uint8_t          s_pending_turn        = 0;

// Synchronization. Both are binary semaphores (not mutexes) — the body
// handler runs on the AsyncTCP task and the play worker runs on a
// pinned core-0 task; mutex priority inheritance asserts when the giver
// isn't the taker.
//   s_buf_avail — counts as "buffer available for next /play". Taken by
//                 body handler before accepting a body, given by play
//                 worker after playback completes.
//   s_play_sem  — body handler signals play worker after staging.
SemaphoreHandle_t s_buf_avail = nullptr;
SemaphoreHandle_t s_play_sem  = nullptr;

volatile bool s_playing = false;

// ---------------------------------------------------------------------------
// WAV parser (mirrors voice_client's; kept local so this module compiles
// without leaking voice internals).
// ---------------------------------------------------------------------------
bool parse_wav(const uint8_t* data, size_t len,
               uint32_t* sample_rate, uint16_t* channels,
               uint16_t* bits, size_t* pcm_offset, size_t* pcm_bytes) {
  if (len < 12) return false;
  if (memcmp(data + 0, "RIFF", 4) != 0) return false;
  if (memcmp(data + 8, "WAVE", 4) != 0) return false;

  bool have_fmt = false, have_data = false;
  size_t pos = 12;
  while (pos + 8 <= len) {
    const char id[5] = { (char)data[pos], (char)data[pos+1],
                          (char)data[pos+2], (char)data[pos+3], 0 };
    const uint32_t sz = (uint32_t)data[pos+4]
                      | ((uint32_t)data[pos+5] << 8)
                      | ((uint32_t)data[pos+6] << 16)
                      | ((uint32_t)data[pos+7] << 24);
    pos += 8;
    if (pos + sz > len) break;

    if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
      *channels    = (uint16_t)(data[pos + 2] | (data[pos + 3] << 8));
      *sample_rate = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8)
                   | ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
      *bits        = (uint16_t)(data[pos + 14] | (data[pos + 15] << 8));
      have_fmt = true;
    } else if (memcmp(id, "data", 4) == 0) {
      *pcm_offset = pos;
      *pcm_bytes  = sz;
      have_data = true;
    }
    pos += sz;
    if (sz & 1) pos++;
    if (have_fmt && have_data) return true;
  }
  return false;
}

// PSRAM source → internal-RAM scratch → resample to dst buffer.
// Reading PSRAM one int16 at a time inside the inner loop runs at ~10
// MB/s on this chip (per the project-x51 ESP32-S3 memcpy benchmarks);
// pulling 4 KB blocks into a stack-local scratch buffer first lets the
// inner loop run on cache-hot internal RAM at ~300+ MB/s. Net: ~2-3x
// faster resample for typical Piper TTS audio.
size_t resample_to_16k(const int16_t* src, size_t src_count, uint32_t src_rate,
                       int16_t* dst, size_t dst_capacity) {
  if (src_rate == 16000) {
    if (src_count > dst_capacity) return 0;
    memcpy(dst, src, src_count * sizeof(int16_t));
    return src_count;
  }
  const size_t dst_count = (size_t)((uint64_t)src_count * 16000ULL / src_rate);
  if (dst_count > dst_capacity) return 0;

  constexpr size_t SCRATCH_SAMPLES = 2048;     // 4 KB on the stack
  int16_t scratch[SCRATCH_SAMPLES];
  size_t scratch_base  = (size_t)-1;           // src index of scratch[0], or "empty"
  size_t scratch_count = 0;

  // Per-output-sample interpolation. Pulls into scratch on demand when
  // the source window slides past the loaded region.
  for (size_t i = 0; i < dst_count; ++i) {
    const uint64_t src_pos_q16 = (uint64_t)i * src_rate * 65536ULL / 16000ULL;
    const size_t   idx  = (size_t)(src_pos_q16 >> 16);
    const uint32_t frac = (uint32_t)(src_pos_q16 & 0xFFFF);

    // We need src[idx] and src[idx+1]. Both must be in scratch.
    if (scratch_base == (size_t)-1 ||
        idx     <  scratch_base ||
        idx + 1 >= scratch_base + scratch_count) {
      scratch_base = idx;
      const size_t remaining = src_count - idx;
      scratch_count = remaining < SCRATCH_SAMPLES ? remaining : SCRATCH_SAMPLES;
      memcpy(scratch, src + idx, scratch_count * sizeof(int16_t));
    }

    const size_t local = idx - scratch_base;
    if (local + 1 >= scratch_count) {
      // Tail edge of the source — clamp.
      dst[i] = scratch[scratch_count - 1];
    } else {
      const int32_t a = scratch[local];
      const int32_t b = scratch[local + 1];
      dst[i] = (int16_t)(a + ((int32_t)(b - a) * (int32_t)frac >> 16));
    }
  }
  return dst_count;
}

// ---------------------------------------------------------------------------
// /play — async upload via body callback. AsyncWebServer streams the body
// in chunks; we copy each chunk into the PSRAM buffer at the supplied
// index, then on the final chunk validate, acquire audio, stage the
// metadata, and signal the play worker. The response (202/4xx/5xx) is
// sent from the request callback after the body callback completes.
// ---------------------------------------------------------------------------

// Per-request state. Single static instance — only one /play can be in
// flight at a time (gated by s_buf_avail), so allocating per request was
// pure heap-fragmentation tax. The body handler initializes this on the
// first chunk; the request handler consumes it.
struct ReqState {
  bool    accepted    = false;
  bool    error       = false;
  uint8_t error_code  = 400;       // 400 / 503 / 409
  const char* error_msg = "";      // points at static literal
  size_t  total_bytes = 0;
  char    session[40] = {0};
  uint8_t turn        = 0;
};
static ReqState s_req;

void on_play_body(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                  size_t index, size_t total) {
  if (index == 0) {
    // First chunk — reset the slot, pull headers, take the buffer.
    s_req = ReqState{};
    s_req.total_bytes = total;
    if (req->hasHeader("X-Session-Id")) {
      const String& s = req->header("X-Session-Id");
      strncpy(s_req.session, s.c_str(), sizeof(s_req.session) - 1);
    }
    if (req->hasHeader("X-Turn-Index")) {
      s_req.turn = (uint8_t)req->header("X-Turn-Index").toInt();
    }
    LOG_I(TAG, "/play hdr session='%s' turn=%u total=%u",
          s_req.session, (unsigned)s_req.turn, (unsigned)total);

    if (s_req.session[0] == '\0') {
      s_req.error = true; s_req.error_code = 400;
      s_req.error_msg = "missing X-Session-Id";
      return;
    }
    if (total == 0 || total > MAX_INBOUND_BYTES) {
      s_req.error = true; s_req.error_code = 400;
      s_req.error_msg = "bad content length";
      return;
    }
    if (xSemaphoreTake(s_buf_avail, 0) != pdTRUE) {
      s_req.error = true; s_req.error_code = 503;
      s_req.error_msg = "playback in progress";
      return;
    }
    s_req.accepted = true;
    s_in_size = 0;
  }

  if (s_req.error || !s_req.accepted) return;

  if (index + len > MAX_INBOUND_BYTES) {
    s_req.error = true; s_req.error_code = 400;
    s_req.error_msg = "body overflow";
    xSemaphoreGive(s_buf_avail);
    s_req.accepted = false;
    return;
  }
  memcpy(s_in_buf + index, data, len);
  s_in_size = index + len;
}

void on_play_request(AsyncWebServerRequest* req) {
  if (s_req.error) {
    req->send(s_req.error_code, "text/plain", s_req.error_msg);
    return;
  }
  if (!s_req.accepted) {
    req->send(400, "text/plain", "missing body");
    return;
  }
  if (s_in_size != s_req.total_bytes) {
    req->send(400, "text/plain", "incomplete body");
    xSemaphoreGive(s_buf_avail);
    s_req.accepted = false;
    return;
  }

  if (!conversation_session::note_play_received(s_req.session, s_req.turn)) {
    req->send(409, "text/plain", "session policy rejected");
    xSemaphoreGive(s_buf_avail);
    s_req.accepted = false;
    return;
  }
  if (!::robimon::hal::audio::try_acquire(::robimon::hal::audio::Owner::PROXIMITY)) {
    LOG_W(TAG, "audio owned by higher priority — dropping play");
    companion_client::send_playback_error(s_req.session, "audio_busy");
    req->send(503, "text/plain", "audio busy");
    xSemaphoreGive(s_buf_avail);
    s_req.accepted = false;
    return;
  }

  // Stage metadata for the worker, signal it.
  strncpy(s_pending_session, s_req.session, sizeof(s_pending_session) - 1);
  s_pending_session[sizeof(s_pending_session) - 1] = '\0';
  s_pending_turn = s_req.turn;
  s_playing      = true;

  xSemaphoreGive(s_play_sem);
  req->send(202, "text/plain", "accepted");
}

void on_stop_request(AsyncWebServerRequest* req) {
  conversation_session::end_session("/stop");
  s_playing = false;
  req->send(200, "text/plain", "stopped");
}

void on_status_request(AsyncWebServerRequest* req) {
  // Extended diagnostic dump — what the field would want when the
  // conversation didn't start. JSON kept compact; values are best-effort.
  using ::robimon::services::proximity::state_str;
  using ::robimon::services::proximity::state;
  using ::robimon::services::proximity::smoothed_rssi;
  using ::robimon::hal::audio::current_owner;
  using ::robimon::hal::audio::Owner;

  const Owner owner = current_owner();
  const char* owner_str =
      owner == Owner::ALARM     ? "alarm" :
      owner == Owner::VOICE     ? "voice" :
      owner == Owner::PROXIMITY ? "proximity" : "none";

  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"bot_id\":\"%s\","
           "\"playing\":%s,"
           "\"current_session\":\"%s\","
           "\"current_turn\":%u,"
           "\"prox_state\":\"%s\","
           "\"smoothed_rssi\":%d,"
           "\"audio_owner\":\"%s\","
           "\"free_heap\":%u,"
           "\"free_psram\":%u,"
           "\"uptime_s\":%lu}",
           companion_client::bot_id(),
           s_playing ? "true" : "false",
           conversation_session::current_session_id(),
           (unsigned)conversation_session::current_turn_index(),
           state_str(state()),
           (int)smoothed_rssi(),
           owner_str,
           (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getFreePsram(),
           (unsigned long)(millis() / 1000));
  req->send(200, "application/json", buf);
}

// ---------------------------------------------------------------------------
// Play worker — drains a binary semaphore, plays the staged audio,
// notifies the companion, releases the buffer.
// ---------------------------------------------------------------------------
void play_task(void*) {
  esp_task_wdt_add(NULL);
  char    cur_session[40] = {0};
  uint8_t cur_turn = 0;
  for (;;) {
    if (xSemaphoreTake(s_play_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
      esp_task_wdt_reset();
      continue;
    }
    strncpy(cur_session, s_pending_session, sizeof(cur_session) - 1);
    cur_session[sizeof(cur_session) - 1] = '\0';
    cur_turn = s_pending_turn;

    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    size_t   pcm_off = 0, pcm_len = 0;
    if (!parse_wav(s_in_buf, s_in_size, &rate, &channels, &bits,
                    &pcm_off, &pcm_len)) {
      LOG_W(TAG, "bad WAV (%u bytes)", (unsigned)s_in_size);
      goto fail;
    }
    LOG_I(TAG, "session %s turn %u: %u Hz, %u ch, %u-bit, %u PCM",
          cur_session, (unsigned)cur_turn,
          (unsigned)rate, (unsigned)channels, (unsigned)bits, (unsigned)pcm_len);

    if (bits != 16 || channels != 1) {
      LOG_W(TAG, "unsupported format (need mono 16-bit)");
      goto fail;
    }
    {
      const int16_t* src = (const int16_t*)(s_in_buf + pcm_off);
      const size_t src_count = pcm_len / sizeof(int16_t);
      const size_t out_count = resample_to_16k(
          src, src_count, rate, s_resamp_buf,
          MAX_RESAMPLED_BYTES / sizeof(int16_t));
      if (out_count == 0) {
        LOG_W(TAG, "resampler overflow");
        goto fail;
      }
      ::robimon::hal::audio::enable_amp(true);
      delay(10);
      ::robimon::hal::audio::play(s_resamp_buf, out_count);
      delay(40);
      ::robimon::hal::audio::enable_amp(false);
      esp_task_wdt_reset();
    }

    ::robimon::hal::audio::release(::robimon::hal::audio::Owner::PROXIMITY);
    s_playing = false;
    conversation_session::note_playback_complete();
    companion_client::send_playback_complete(cur_session, cur_turn);
    xSemaphoreGive(s_buf_avail);
    continue;

fail:
    ::robimon::hal::audio::release(::robimon::hal::audio::Owner::PROXIMITY);
    s_playing = false;
    companion_client::send_playback_error(cur_session, "decode_or_play_failed");
    xSemaphoreGive(s_buf_avail);
  }
}

}  // namespace

bool begin() {
  s_in_buf = (uint8_t*)heap_caps_malloc(MAX_INBOUND_BYTES, MALLOC_CAP_SPIRAM);
  s_resamp_buf = (int16_t*)heap_caps_malloc(MAX_RESAMPLED_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_in_buf || !s_resamp_buf) {
    LOG_E(TAG, "PSRAM alloc failed (in=%p resamp=%p)", s_in_buf, s_resamp_buf);
    return false;
  }
  LOG_I(TAG, "buffers ready (in=%u resamp=%u)",
        (unsigned)MAX_INBOUND_BYTES, (unsigned)MAX_RESAMPLED_BYTES);

  // Binary semaphores — safe to give from a different task than took.
  // s_buf_avail starts as available (buffer free, ready for first /play).
  s_buf_avail = xSemaphoreCreateBinary();
  s_play_sem  = xSemaphoreCreateBinary();
  if (!s_buf_avail || !s_play_sem) {
    LOG_E(TAG, "semaphore create failed");
    return false;
  }
  xSemaphoreGive(s_buf_avail);

  s_web = new AsyncWebServer(proximity_config::PLAYBACK_SERVER_PORT);
  s_web->on("/play", HTTP_POST, on_play_request,
            /*onUpload=*/nullptr,
            /*onBody  =*/on_play_body);
  s_web->on("/stop",   HTTP_POST, on_stop_request);
  s_web->on("/status", HTTP_GET,  on_status_request);
  s_web->begin();
  LOG_I(TAG, "listening on :%u", (unsigned)proximity_config::PLAYBACK_SERVER_PORT);

  // Play task on core 1 — alongside the main loop, OFF core 0 where
  // NimBLE host + AsyncTCP + the BT controller all already live. With
  // play_task on core 0, audio playback was starving the BLE scan
  // callback during turns; moving it lets BLE keep receiving samples
  // while audio is decoded/resampled/written.
  xTaskCreatePinnedToCore(play_task, "play_audio", 6144, nullptr,
                           /*priority=*/2, &s_play_task, /*core=*/1);
  return true;
}

bool is_playing() { return s_playing; }

}  // namespace robimon::services::playback_server
