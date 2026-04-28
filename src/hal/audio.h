// Audio HAL — ES8311 codec over I2C, I2S audio transport, NS4150B amp gate.
// Mono speaker out + mono mic in (the on-board ES7210 can route the dual
// mic array, but Stage B uses just the ES8311's own mic preamp; ES7210
// integration comes later when we add full AEC).
//
// Hardware quirks worth knowing:
//   - The codec defaults to muted; es8311_init unmutes and sets resolution.
//   - PA_CTRL (GPIO 46) gates the NS4150B amp. Don't enable it before the
//     I2S clocks are running or you'll get a startup pop.
//   - Stage B uses 16 kHz mono — speech-friendly, half the bandwidth of
//     phone-quality. We can bump to 16/24-bit stereo if music playback
//     becomes a feature.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace robimon::hal::audio {

constexpr uint32_t DEFAULT_SAMPLE_RATE = 16000;
constexpr uint8_t  MAX_VOLUME_PERCENT  = 70;   // spec cap; never go above this

bool begin(uint32_t sample_rate = DEFAULT_SAMPLE_RATE);

// 0..100. Caller-supplied value is clamped to MAX_VOLUME_PERCENT.
void set_volume_percent(uint8_t percent);

// Toggle the speaker amp. Disable before yanking I2S clocks to avoid pops.
void enable_amp(bool on);

// Synchronous PCM write/read. Mono int16 samples; the I2S peripheral runs
// stereo internally and we duplicate / pick channel 0 inside.
// Returns the number of mono samples actually written / read.
size_t play(const int16_t* samples, size_t count);
size_t capture(int16_t* samples, size_t max_count);

// ---- Audio ownership arbitration -----------------------------------------
// One thing plays at a time. Callers acquire the audio path at a priority
// before driving play()/capture(); the call fails if a higher-priority
// owner holds it. This is cooperative — there's no preemption. An ALARM
// that fires while voice is mid-playback waits for voice's chunk to
// complete; voice that starts while proximity is mid-playback similarly.
// Accept that limitation and the audio path stays simple. Document the
// trade-off in PROXIMITY_FEATURE.md.
enum class Owner : uint8_t {
  NONE      = 0,
  PROXIMITY = 1,
  VOICE     = 2,
  ALARM     = 3,    // highest — kid-safety wake-up
};

// Atomic test-and-set. Returns true and takes ownership if either:
//   - the path is currently NONE, or
//   - the requested owner is strictly higher priority than the current.
// Returns false if a same-or-higher priority owner already holds it.
bool  try_acquire(Owner owner);

// Releases ownership iff the caller currently holds it. No-op otherwise.
void  release(Owner owner);

// Current owner — callers can poll this to detect they were preempted by
// a higher priority owner taking over.
Owner current_owner();

// Plays a short sine-ish chirp on the speaker. Useful as a one-shot bring-up
// confirmation. Blocks for ~120 ms.
void play_test_tone();

// Brief "ready to listen" cue — a single ~80 ms blip at 1 kHz. Played
// before voice recording starts so the user knows when to start talking.
// Blocks for ~120 ms total (cue + amp settling).
void play_listen_cue();

// Short rising "yay" chirp (600→1000 Hz). Used as a success accent on
// completed voice rounds. Blocks for ~250 ms.
void play_success();

// Short descending "uh-oh" chirp (700→500 Hz). Used as a gentle failure
// cue on voice flow errors. Friendly, not punishing. Blocks for ~250 ms.
void play_oops();

// Alarm sound — repeating two-tone chime that loops on a background task
// pinned to core 0 so the UI on core 1 stays responsive. start_alarm() is
// idempotent (no-op if the alarm is already playing); stop_alarm() returns
// once the playback task has cleaned up.
void start_alarm();
void stop_alarm();
bool alarm_is_playing();

bool ok();

}  // namespace robimon::hal::audio
