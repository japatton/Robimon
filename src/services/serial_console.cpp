#include "serial_console.h"
#include "config_store.h"
#include "ha_client.h"
#include "wifi_mgr.h"
#include "proximity_monitor.h"
#include "../app/log.h"

#include <Arduino.h>
#include <string.h>

namespace robimon::services::serial_console {

namespace {
constexpr const char* TAG = "ser";

constexpr size_t BUF_MAX = 768;   // long enough for HA tokens (~180 chars) + slack
char     s_buf[BUF_MAX];
size_t   s_len = 0;
bool     s_prompt_pending = true;

constexpr const char* PROMPT = "robimon> ";

void print_prompt() { Serial.print(PROMPT); s_prompt_pending = false; }

bool starts_with(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Splits at first whitespace. Trailing arg includes embedded spaces.
void split_first(char* line, const char** out_first, const char** out_rest) {
  *out_first = line;
  char* p = line;
  while (*p && *p != ' ' && *p != '\t') p++;
  if (!*p) { *out_rest = ""; return; }
  *p++ = '\0';
  while (*p == ' ' || *p == '\t') p++;
  *out_rest = p;
}

void cmd_help() {
  Serial.println("commands:");
  Serial.println("  help                          show this");
  Serial.println("  set <key> <value>             save to NVS (e.g. set ha_token eyJ...)");
  Serial.println("  get <key>                     print NVS value (ha_token is masked)");
  Serial.println("  forget <key>                  clear NVS key");
  Serial.println("  list                          known keys");
  Serial.println("  ha-reconnect                  retry HA WS with current creds");
  Serial.println("  status                        wifi + ha status");
  Serial.println("  pin-reset                     reset PIN to default (0000) — lockout escape");
  Serial.println("  force-setup                   enter setup mode on next boot");
  Serial.println("  factory-reset                 wipe all saved config + reboot into setup");
  Serial.println("  bot-id                        print this bot's BLE address (for pairing)");
  Serial.println("  pair-peer <MAC>               save peer's BLE address for proximity (e.g. AA:BB:CC:DD:EE:FF)");
  Serial.println("  unpair-peer                   forget paired peer");
  Serial.println("  proximity                     show proximity state + smoothed RSSI");
  Serial.println("  task-stats                    list FreeRTOS tasks + stack high-water marks");
}

void cmd_list() {
  Serial.println("known keys:");
  Serial.println("  wifi_ssid wifi_pass");
  Serial.println("  ha_url ha_token ha_entity");
  Serial.println("  tz_posix");
  Serial.println("  disp_b disp_dim_s disp_slp_s");
  Serial.println("  pin_h pin_s alarms_blob alarms_n");
}

void cmd_set(char* args) {
  const char *key, *value;
  split_first(args, &key, &value);
  if (!*key) { Serial.println("usage: set <key> <value>"); return; }
  config::set_string(key, value);
  Serial.printf("ok: %s = (%u chars)\n", key, (unsigned)strlen(value));
  // If we just changed a credential, kick the relevant service.
  if (starts_with(key, "ha_")) {
    Serial.println("(reconnecting HA)");
    ha_client::auto_connect();
  }
  if (strcmp(key, "wifi_ssid") == 0 || strcmp(key, "wifi_pass") == 0) {
    Serial.println("(use 'wifi reconnect' is not yet implemented; reboot to apply)");
  }
}

void cmd_get(char* args) {
  if (!*args) { Serial.println("usage: get <key>"); return; }
  char buf[640] = {0};
  const size_t n = config::get_string(args, buf, sizeof(buf));
  if (n == 0) { Serial.printf("%s = (empty)\n", args); return; }
  if (strcmp(args, "ha_token") == 0 || strcmp(args, "wifi_pass") == 0) {
    // Mask all but last 4 chars so the user can verify they have the right
    // token without exposing the whole value.
    const size_t show = n > 4 ? 4 : n;
    Serial.printf("%s = ****%s (%u chars)\n",
                  args, buf + (n - show), (unsigned)n);
  } else {
    Serial.printf("%s = %s\n", args, buf);
  }
}

void cmd_forget(char* args) {
  if (!*args) { Serial.println("usage: forget <key>"); return; }
  config::set_string(args, "");
  Serial.printf("ok: %s cleared\n", args);
}

void cmd_status() {
  Serial.printf("wifi: %s ssid='%s' ip='%s' rssi=%d\n",
                wifi_mgr::state_str(),
                wifi_mgr::current_ssid(),
                wifi_mgr::current_ip(),
                (int)wifi_mgr::current_rssi());
  Serial.printf("ha:   %s url='%s'\n",
                ha_client::state_str(),
                ha_client::url());
}

void cmd_pin_reset() {
  // Wipes the stored PIN hash + salt. config_store::pin_check() falls back
  // to the literal default ("0000") when no salt is set, so this is the
  // documented escape hatch for a forgotten PIN.
  config::set_string("pin_h", "");
  config::set_string("pin_s", "");
  Serial.println("ok: PIN reset — default is 0000");
}

void cmd_force_setup() {
  // One-shot flag picked up by main.cpp on the next boot — enters the
  // captive-portal setup flow regardless of whether 'configured' is set.
  config::set_uint("force_setup", 1);
  Serial.println("ok: setup mode armed for next boot — type 'reboot' to apply");
}

void cmd_bot_id() {
  Serial.printf("ble address: %s\n", proximity::own_address());
  uint8_t peer[6];
  if (proximity::get_paired_peer(peer)) {
    Serial.printf("paired peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
  } else {
    Serial.println("paired peer: (none — run 'pair-peer <MAC>')");
  }
}

void cmd_pair_peer(char* args) {
  if (!*args) { Serial.println("usage: pair-peer <MAC>  (e.g. pair-peer AA:BB:CC:DD:EE:FF)"); return; }
  uint8_t mac[6];
  // Reuse proximity_monitor's parser by going through set_paired_peer —
  // but we need our own parser to validate first.
  uint8_t b = 0;
  uint8_t nibble = 0;
  for (const char* p = args; *p && b < 6; ++p) {
    if (*p == ':' || *p == '-' || *p == ' ') continue;
    int v;
    if      (*p >= '0' && *p <= '9') v = *p - '0';
    else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
    else { Serial.printf("bad hex char '%c'\n", *p); return; }
    if (nibble == 0) { mac[b] = (uint8_t)(v << 4); nibble = 1; }
    else             { mac[b] |= (uint8_t)v;       nibble = 0; b++; }
  }
  if (b != 6 || nibble != 0) {
    Serial.println("bad MAC — need 6 hex bytes (AA:BB:CC:DD:EE:FF or AABBCCDDEEFF)");
    return;
  }
  proximity::set_paired_peer(mac);
  Serial.printf("ok: paired with %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void cmd_unpair_peer() {
  proximity::forget_paired_peer();
  Serial.println("ok: peer forgotten");
}

void cmd_proximity() {
  Serial.printf("state: %s  smoothed_rssi: %d dBm\n",
                proximity::state_str(proximity::state()),
                (int)proximity::smoothed_rssi());
}

void cmd_task_stats() {
  // FreeRTOS gives us per-task stack high-water marks (smallest free
  // stack space the task has ever seen). Useful for sizing tasks
  // empirically rather than by guess. CONFIG_FREERTOS_USE_TRACE_FACILITY
  // must be enabled (it is by default on Arduino-ESP32 3.x).
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* arr = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
  if (!arr) { Serial.println("alloc failed"); return; }
  uint32_t total_runtime = 0;
  const UBaseType_t got = uxTaskGetSystemState(arr, n, &total_runtime);
  Serial.println("name              core  prio  stack-min(B)");
  for (UBaseType_t i = 0; i < got; ++i) {
    Serial.printf("  %-16s %4d   %3u   %u\n",
                  arr[i].pcTaskName,
                  (int)arr[i].xCoreID,
                  (unsigned)arr[i].uxCurrentPriority,
                  (unsigned)arr[i].usStackHighWaterMark * 4);   // words → bytes
  }
  Serial.printf("free heap: %u  free PSRAM: %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  free(arr);
}

void cmd_factory_reset() {
  // Clears every key the device cares about and arms setup mode. Doesn't
  // touch alarms_blob — those are unlikely to be the cause of a lockout,
  // and the alarm screen is read-only without PIN access anyway.
  const char* keys[] = {
    "wifi_ssid", "wifi_pass", "tz_posix",
    "ha_url", "ha_token", "ha_entity",
    "voice_url", "voice_model", "voice_prompt",
    "pin_h", "pin_s",
    "disp_b", "disp_dim_s", "disp_slp_s",
    nullptr,
  };
  for (int i = 0; keys[i]; ++i) config::set_string(keys[i], "");
  config::set_uint("configured", 0);
  config::set_uint("force_setup", 1);
  Serial.println("ok: factory reset — rebooting into setup");
  delay(500);
  ESP.restart();
}

void execute_line(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (!*line) return;

  const char *cmd, *rest;
  split_first(line, &cmd, &rest);

  if      (strcmp(cmd, "help") == 0)          cmd_help();
  else if (strcmp(cmd, "list") == 0)          cmd_list();
  else if (strcmp(cmd, "set") == 0)           cmd_set((char*)rest);
  else if (strcmp(cmd, "get") == 0)           cmd_get((char*)rest);
  else if (strcmp(cmd, "forget") == 0)        cmd_forget((char*)rest);
  else if (strcmp(cmd, "ha-reconnect") == 0)  { Serial.println("(reconnecting HA)"); ha_client::auto_connect(); }
  else if (strcmp(cmd, "status") == 0)        cmd_status();
  else if (strcmp(cmd, "pin-reset") == 0)     cmd_pin_reset();
  else if (strcmp(cmd, "force-setup") == 0)   cmd_force_setup();
  else if (strcmp(cmd, "factory-reset") == 0) cmd_factory_reset();
  else if (strcmp(cmd, "bot-id") == 0)        cmd_bot_id();
  else if (strcmp(cmd, "pair-peer") == 0)     cmd_pair_peer((char*)rest);
  else if (strcmp(cmd, "unpair-peer") == 0)   cmd_unpair_peer();
  else if (strcmp(cmd, "proximity") == 0)     cmd_proximity();
  else if (strcmp(cmd, "task-stats") == 0)    cmd_task_stats();
  else if (strcmp(cmd, "reboot") == 0)        { Serial.println("rebooting…"); delay(200); ESP.restart(); }
  else { Serial.printf("unknown: %s (try 'help')\n", cmd); }
}

}  // namespace

void begin() {
  s_len = 0;
  s_prompt_pending = true;
  Serial.println();
  Serial.println("robimon serial console — type 'help'");
}

void update(uint32_t /*now_ms*/) {
  if (s_prompt_pending) print_prompt();

  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      Serial.println();
      if (s_len > 0) {
        s_buf[s_len] = '\0';
        execute_line(s_buf);
      }
      s_len = 0;
      s_prompt_pending = true;
      return;
    }
    if (c == 8 || c == 127) {  // BS or DEL
      if (s_len > 0) {
        s_len--;
        Serial.print("\b \b");
      }
      continue;
    }
    if (c < 0x20) continue;
    if (s_len < BUF_MAX - 1) {
      s_buf[s_len++] = (char)c;
      Serial.write((char)c);
    }
  }
}

}  // namespace robimon::services::serial_console
