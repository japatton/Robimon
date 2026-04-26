// config_store — typed wrapper around NVS Preferences for our persistent
// settings. Single namespace ("robimon"); keys are prefixed by section so
// they sort sensibly in nvs-partition-tool dumps.
//
// What lives here (Stage F-1 + future):
//   pin_hash       (8B, PBKDF2-SHA256 of the PIN; stored as raw bytes)
//   pin_salt       (8B random per device, generated on first PIN change)
//   wifi_ssid      (str)
//   wifi_password  (str, plaintext — NVS encryption is not enabled yet)
//   ha_url         (str)
//   ha_token       (str)
//   mqtt_*         (str/int)
//   ollama_url     (str)
//   ollama_model   (str)
//   ollama_prompt  (str)
//   tz_posix       (str)
//   brightness     (uint8 0..255)
//   idle_dim_s     (uint16, seconds before idle dim)
//   sleep_s        (uint16, seconds before deep sleep)
//
// Stage F-1 only uses PIN-related storage. Other keys are listed here so the
// later stages know what to call them.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace robimon::services::config {

bool begin();

// PIN: stored as a salted hash; we never store the cleartext PIN. On a fresh
// device the stored hash is empty and pin_check() returns true for the
// default "0000" so the first-run flow can let the user in.
//
// pin_4_digits is exactly 4 chars (digits) + nul terminator (5 bytes total).
bool pin_check(const char* pin_4_digits);
bool pin_set  (const char* pin_4_digits);
bool pin_is_default();   // true on a fresh device (no PIN ever set)

// Generic typed accessors used by the various settings sections.
bool   set_string(const char* key, const char* value);
size_t get_string(const char* key, char* out, size_t out_max);  // returns bytes written (excl. NUL)
bool   set_uint  (const char* key, uint32_t value);
uint32_t get_uint(const char* key, uint32_t default_value);

// Wipe everything in the namespace. Used by Factory Reset.
bool factory_reset();

}  // namespace robimon::services::config
