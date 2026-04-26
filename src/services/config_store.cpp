#include "config_store.h"
#include "../app/log.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <string.h>

namespace robimon::services::config {

namespace {
constexpr const char* TAG = "cfg";
constexpr const char* NS  = "robimon";
constexpr const char* KEY_PIN_HASH = "pin_h";
constexpr const char* KEY_PIN_SALT = "pin_s";
constexpr const char* DEFAULT_PIN  = "0000";

constexpr size_t PIN_LEN     = 4;
constexpr size_t HASH_LEN    = 32;     // SHA-256
constexpr size_t SALT_LEN    = 16;

Preferences s_prefs;
bool        s_ok = false;

// HMAC-SHA256(key=salt, msg=pin) — keeps the implementation tiny vs PBKDF2
// while still preventing rainbow-table lookups of a 4-digit PIN. We accept
// brute-force resistance is limited (10⁴ keyspace) — the device is physically
// in the user's home and the threat model for a kid-companion isn't extreme.
void hash_pin(const uint8_t* salt, size_t salt_len, const char* pin, uint8_t* out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, /*hmac=*/1);
  mbedtls_md_hmac_starts(&ctx, salt, salt_len);
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)pin, strlen(pin));
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

}  // namespace

bool begin() {
  if (!s_prefs.begin(NS, /*readOnly=*/false)) {
    LOG_E(TAG, "Preferences::begin('%s') failed", NS);
    return false;
  }
  s_ok = true;
  LOG_I(TAG, "config store up — pin %s",
        pin_is_default() ? "is default (0000)" : "is set");
  return true;
}

bool pin_is_default() {
  if (!s_ok) return true;
  return !s_prefs.isKey(KEY_PIN_HASH);
}

bool pin_check(const char* pin_4_digits) {
  if (!s_ok || !pin_4_digits || strlen(pin_4_digits) != PIN_LEN) return false;

  if (pin_is_default()) {
    return strcmp(pin_4_digits, DEFAULT_PIN) == 0;
  }

  uint8_t stored_hash[HASH_LEN];
  uint8_t salt[SALT_LEN];
  if (s_prefs.getBytes(KEY_PIN_HASH, stored_hash, HASH_LEN) != HASH_LEN) return false;
  if (s_prefs.getBytes(KEY_PIN_SALT, salt,        SALT_LEN) != SALT_LEN) return false;

  uint8_t calc_hash[HASH_LEN];
  hash_pin(salt, SALT_LEN, pin_4_digits, calc_hash);

  // Constant-time compare so timing doesn't leak which digit was wrong.
  uint8_t diff = 0;
  for (size_t i = 0; i < HASH_LEN; ++i) diff |= stored_hash[i] ^ calc_hash[i];
  return diff == 0;
}

bool pin_set(const char* pin_4_digits) {
  if (!s_ok || !pin_4_digits || strlen(pin_4_digits) != PIN_LEN) return false;

  // New random salt each time the PIN is changed.
  uint8_t salt[SALT_LEN];
  esp_fill_random(salt, SALT_LEN);
  uint8_t hash[HASH_LEN];
  hash_pin(salt, SALT_LEN, pin_4_digits, hash);

  if (s_prefs.putBytes(KEY_PIN_SALT, salt, SALT_LEN) != SALT_LEN) return false;
  if (s_prefs.putBytes(KEY_PIN_HASH, hash, HASH_LEN) != HASH_LEN) return false;
  LOG_I(TAG, "PIN updated");
  return true;
}

bool set_string(const char* key, const char* value) {
  if (!s_ok || !key || !value) return false;
  return s_prefs.putString(key, value) > 0;
}

size_t get_string(const char* key, char* out, size_t out_max) {
  if (!s_ok || !key || !out || out_max == 0) return 0;
  return s_prefs.getString(key, out, out_max);
}

bool set_uint(const char* key, uint32_t value) {
  if (!s_ok || !key) return false;
  return s_prefs.putUInt(key, value) > 0;
}

uint32_t get_uint(const char* key, uint32_t default_value) {
  if (!s_ok || !key) return default_value;
  return s_prefs.getUInt(key, default_value);
}

bool factory_reset() {
  if (!s_ok) return false;
  const bool ok = s_prefs.clear();
  LOG_I(TAG, "factory reset (clear) -> %d", (int)ok);
  return ok;
}

}  // namespace robimon::services::config
