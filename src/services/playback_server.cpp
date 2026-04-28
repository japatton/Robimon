#include "playback_server.h"
#include "proximity_config.h"
#include "conversation_session.h"
#include "companion_client.h"
#include "../hal/audio.h"
#include "../app/log.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
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

WebServer*       s_web        = nullptr;
TaskHandle_t     s_http_task  = nullptr;
TaskHandle_t     s_play_task  = nullptr;

// Inbound + resample buffers (PSRAM).
uint8_t*         s_in_buf       = nullptr;
size_t           s_in_size      = 0;
int16_t*         s_resamp_buf   = nullptr;

// Per-play metadata stashed by the http handler for the play worker.
char             s_pending_session[40] = {0};
uint8_t          s_pending_turn        = 0;

// Synchronization:
//   s_buf_mux — guards s_in_buf so only one /play uses it at a time
//   s_play_sem — http handler signals play worker; worker takes it on wake
SemaphoreHandle_t s_buf_mux  = nullptr;
SemaphoreHandle_t s_play_sem = nullptr;

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

// Linear resampler to 16 kHz mono int16. Same algorithm as voice_client.
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

// ---------------------------------------------------------------------------
// Body streaming
// ---------------------------------------------------------------------------
size_t read_body(WiFiClient& client, size_t expected) {
  if (expected > MAX_INBOUND_BYTES) return 0;
  size_t got = 0;
  const uint32_t deadline = millis() + 8000;
  while (got < expected) {
    if (millis() > deadline) {
      LOG_W(TAG, "body read timeout @ %u/%u", (unsigned)got, (unsigned)expected);
      return 0;
    }
    if (!client.connected() && client.available() == 0) break;
    const int avail = client.available();
    if (avail <= 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const size_t want = (size_t)avail < (expected - got) ? (size_t)avail
                                                          : (expected - got);
    got += client.readBytes(s_in_buf + got, want);
    esp_task_wdt_reset();
  }
  return got;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
void handle_play() {
  // Validate caller-supplied metadata up front.
  String session = s_web->header("X-Session-Id");
  String turn_s  = s_web->header("X-Turn-Index");
  if (session.length() == 0) {
    s_web->send(400, "text/plain", "missing X-Session-Id");
    return;
  }
  uint8_t turn = (uint8_t)turn_s.toInt();

  // Take the buffer mutex — non-blocking. If a previous /play is still
  // playing, signal the companion app to back off.
  if (xSemaphoreTake(s_buf_mux, 0) != pdTRUE) {
    s_web->send(503, "text/plain", "playback in progress");
    return;
  }

  const int content_len = s_web->header("Content-Length").toInt();
  if (content_len <= 0 || (size_t)content_len > MAX_INBOUND_BYTES) {
    s_web->send(400, "text/plain", "bad content length");
    xSemaphoreGive(s_buf_mux);
    return;
  }

  WiFiClient& client = s_web->client();
  s_in_size = read_body(client, (size_t)content_len);
  if (s_in_size == 0) {
    s_web->send(400, "text/plain", "body read failed");
    xSemaphoreGive(s_buf_mux);
    return;
  }

  // Session policy (idle timeout, max-turns) gates whether we accept.
  if (!conversation_session::note_play_received(session.c_str(), turn)) {
    s_web->send(409, "text/plain", "session policy rejected");
    xSemaphoreGive(s_buf_mux);
    return;
  }

  // Audio path arbitration. Alarm + voice both out-rank proximity; if
  // either is active we tell the companion app and bail.
  if (!::robimon::hal::audio::try_acquire(::robimon::hal::audio::Owner::PROXIMITY)) {
    LOG_W(TAG, "audio owned by higher priority — dropping play");
    companion_client::send_playback_error(session.c_str(), "audio_busy");
    s_web->send(503, "text/plain", "audio busy");
    xSemaphoreGive(s_buf_mux);
    return;
  }

  // Stash session metadata for the worker, signal it.
  strncpy(s_pending_session, session.c_str(), sizeof(s_pending_session) - 1);
  s_pending_session[sizeof(s_pending_session) - 1] = '\0';
  s_pending_turn = turn;
  s_playing      = true;

  xSemaphoreGive(s_play_sem);
  s_web->send(202, "text/plain", "accepted");
}

void handle_stop() {
  conversation_session::end_session("/stop");
  // Signal "session ended" — the play worker checks s_playing periodically.
  // A more aggressive preempt would acquire ALARM-priority then drop it,
  // but for now we let the current chunk finish.
  s_playing = false;
  s_web->send(200, "text/plain", "stopped");
}

void handle_status() {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"bot_id\":\"%s\",\"playing\":%s,\"current_session\":\"%s\"}",
           companion_client::bot_id(),
           s_playing ? "true" : "false",
           conversation_session::current_session_id());
  s_web->send(200, "application/json", buf);
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
void http_task(void*) {
  esp_task_wdt_add(NULL);
  for (;;) {
    s_web->handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_task_wdt_reset();
  }
}

void play_task(void*) {
  esp_task_wdt_add(NULL);
  char    cur_session[40] = {0};
  uint8_t cur_turn = 0;
  for (;;) {
    if (xSemaphoreTake(s_play_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
      esp_task_wdt_reset();
      continue;
    }

    // Snapshot the metadata under the buffer mutex (which the http handler
    // currently holds — we'll release it after we're done).
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

    // Success — release audio + buffer, ack the companion.
    ::robimon::hal::audio::release(::robimon::hal::audio::Owner::PROXIMITY);
    s_playing = false;
    conversation_session::note_playback_complete();
    companion_client::send_playback_complete(cur_session, cur_turn);
    xSemaphoreGive(s_buf_mux);
    continue;

fail:
    ::robimon::hal::audio::release(::robimon::hal::audio::Owner::PROXIMITY);
    s_playing = false;
    companion_client::send_playback_error(cur_session, "decode_or_play_failed");
    xSemaphoreGive(s_buf_mux);
  }
}

}  // namespace

bool begin() {
  // PSRAM buffers — sized once, lived forever. Same approach as voice_client
  // (avoids per-/play malloc churn that would fragment PSRAM).
  s_in_buf = (uint8_t*)heap_caps_malloc(MAX_INBOUND_BYTES, MALLOC_CAP_SPIRAM);
  s_resamp_buf = (int16_t*)heap_caps_malloc(MAX_RESAMPLED_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_in_buf || !s_resamp_buf) {
    LOG_E(TAG, "PSRAM alloc failed (in=%p resamp=%p)", s_in_buf, s_resamp_buf);
    return false;
  }
  LOG_I(TAG, "buffers ready (in=%u resamp=%u)",
        (unsigned)MAX_INBOUND_BYTES, (unsigned)MAX_RESAMPLED_BYTES);

  s_buf_mux  = xSemaphoreCreateMutex();
  s_play_sem = xSemaphoreCreateBinary();
  if (!s_buf_mux || !s_play_sem) {
    LOG_E(TAG, "semaphore create failed");
    return false;
  }

  s_web = new WebServer(proximity_config::PLAYBACK_SERVER_PORT);
  s_web->collectHeaders((const char*[]){ "X-Session-Id", "X-Turn-Index", "Content-Length" }, 3);
  s_web->on("/play",   HTTP_POST, handle_play);
  s_web->on("/stop",   HTTP_POST, handle_stop);
  s_web->on("/status", HTTP_GET,  handle_status);
  s_web->begin();
  LOG_I(TAG, "listening on :%u", (unsigned)proximity_config::PLAYBACK_SERVER_PORT);

  xTaskCreatePinnedToCore(http_task, "play_http", 6144, nullptr, /*priority=*/1,
                           &s_http_task, /*core=*/0);
  xTaskCreatePinnedToCore(play_task, "play_audio", 6144, nullptr, /*priority=*/1,
                           &s_play_task, /*core=*/0);
  return true;
}

bool is_playing() { return s_playing; }

}  // namespace robimon::services::playback_server
