#include "setup_portal.h"
#include "config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_system.h>

namespace robimon::services::setup_portal {

namespace {
constexpr const char* TAG = "setup";

WebServer  s_web(80);
DNSServer  s_dns;
bool       s_running    = false;
bool       s_scan_running = false;
uint32_t   s_scan_started_ms = 0;

// ---- Setup page HTML ------------------------------------------------------
// Single self-contained page: HTML + CSS + tiny JS. Dark theme to match the
// device's vibe. Auto-scans WiFi on load via fetch('/scan').
const char* const SETUP_PAGE = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Robimon Setup</title>
<style>
body { font-family: system-ui, -apple-system, sans-serif; max-width: 480px;
       margin: 1em auto; padding: 0 1em; background: #0a0a0e; color: #eee; }
h1 { color: #38c8ff; text-align: center; margin: 0.5em 0 1em; }
label { display: block; margin: 1em 0 0.3em; font-weight: bold; color: #ccc; }
input, select, textarea { width: 100%; padding: 0.6em; margin: 0.2em 0;
       box-sizing: border-box; background: #1a1a2e; color: #fff;
       border: 1px solid #2a4a6a; border-radius: 4px; font-size: 1em; }
textarea { font-family: ui-monospace, monospace; height: 6em; resize: vertical; }
button { width: 100%; padding: 0.8em; margin: 0.5em 0; font-size: 1em;
         cursor: pointer; border: none; border-radius: 4px; }
.btn-primary { background: #38c8ff; color: #0a0a0e; font-weight: bold; }
.btn-secondary { background: #2a4a6a; color: #eee; }
.section { margin: 1.5em 0; padding-bottom: 1em; border-bottom: 1px solid #2a4a6a; }
.hint { font-size: 0.85em; color: #888; margin-top: 0.3em; }
</style>
</head>
<body>
<h1>Robimon Setup</h1>
<form id="f" method="POST" action="/save">
  <div class="section">
    <label>WiFi Network</label>
    <select name="wifi_ssid" id="ssid"><option value="">scanning...</option></select>
    <button type="button" class="btn-secondary" onclick="scan()">rescan</button>
    <label>WiFi Password</label>
    <input name="wifi_pass" type="password" autocomplete="new-password">
  </div>
  <div class="section">
    <label>Timezone</label>
    <select name="tz">
      <option value="EST5EDT,M3.2.0,M11.1.0">Eastern</option>
      <option value="CST6CDT,M3.2.0,M11.1.0" selected>Central</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">Mountain</option>
      <option value="MST7">Arizona (no DST)</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">Pacific</option>
      <option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska</option>
      <option value="HST10">Hawaii</option>
      <option value="UTC0">UTC</option>
    </select>
  </div>
  <div class="section">
    <label>Home Assistant URL</label>
    <input name="ha_url" placeholder="ws://homeassistant.local:8123" autocomplete="off">
    <p class="hint">Leave blank to skip HA integration.</p>
    <label>HA Access Token</label>
    <textarea name="ha_token" placeholder="paste long-lived access token" autocomplete="off"></textarea>
    <p class="hint">HA &rarr; Profile &rarr; Long-Lived Access Tokens &rarr; Create Token</p>
    <label>HA Default Entity</label>
    <input name="ha_entity" placeholder="sun.sun" autocomplete="off">
  </div>
  <button type="submit" class="btn-primary">Save &amp; Reboot</button>
</form>
<script>
async function scan() {
  const sel = document.getElementById('ssid');
  sel.innerHTML = '<option value="">scanning...</option>';
  // Poll up to ~6 s; the device runs the scan asynchronously and reports
  // results when ready.
  for (let i = 0; i < 12; i++) {
    try {
      const r = await fetch('/scan');
      const data = await r.json();
      if (!data.scanning) {
        sel.innerHTML = '';
        if (!data.networks.length) {
          sel.innerHTML = '<option value="">no networks found</option>';
          return;
        }
        data.networks.sort((a,b) => b.rssi - a.rssi);
        data.networks.forEach(n => {
          const opt = document.createElement('option');
          opt.value = n.ssid;
          opt.textContent = (n.secure ? '\u{1F512} ' : '') + n.ssid + ' (' + n.rssi + ' dBm)';
          sel.appendChild(opt);
        });
        return;
      }
    } catch (e) {}
    await new Promise(r => setTimeout(r, 500));
  }
  sel.innerHTML = '<option value="">scan timed out</option>';
}
scan();
</script>
</body>
</html>)HTML";

const char* const SAVED_PAGE = R"HTML(<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><title>Saved</title><style>body{background:#0a0a0e;color:#eee;font-family:system-ui,sans-serif;text-align:center;padding:3em 1em}h1{color:#38c8ff}</style></head><body><h1>Saved!</h1><p>Robimon will reboot now.</p><p>Re-connect to your WiFi and the device will come back online.</p></body></html>)HTML";

// ---- HTTP handlers --------------------------------------------------------
void handle_root() {
  s_web.send(200, "text/html", SETUP_PAGE);
}

void handle_scan() {
  // Async scan; first call kicks it off and returns scanning:true.
  // Subsequent calls return results when scanComplete reports them.
  if (!s_scan_running) {
    WiFi.scanNetworks(/*async=*/true);
    s_scan_running = true;
    s_scan_started_ms = millis();
    s_web.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) {
    s_web.send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
    return;
  }
  // Done — build JSON and clear scan state.
  String json = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
         + ",\"secure\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]}";
  s_web.send(200, "application/json", json);
  WiFi.scanDelete();
  s_scan_running = false;
}

void handle_save() {
  using namespace ::robimon::services;
  // All fields are optional except wifi_ssid; we still accept and persist
  // whatever's there so a re-run can incrementally set just one section.
  if (s_web.hasArg("wifi_ssid")) config::set_string("wifi_ssid", s_web.arg("wifi_ssid").c_str());
  if (s_web.hasArg("wifi_pass")) config::set_string("wifi_pass", s_web.arg("wifi_pass").c_str());
  if (s_web.hasArg("tz"))        config::set_string("tz_posix",  s_web.arg("tz").c_str());
  if (s_web.hasArg("ha_url"))    config::set_string("ha_url",    s_web.arg("ha_url").c_str());
  if (s_web.hasArg("ha_token"))  config::set_string("ha_token",  s_web.arg("ha_token").c_str());
  if (s_web.hasArg("ha_entity")) config::set_string("ha_entity", s_web.arg("ha_entity").c_str());
  config::set_uint("configured", 1);

  s_web.send(200, "text/html", SAVED_PAGE);
  LOG_I(TAG, "config saved -> rebooting");
  // Give the response a moment to flush, then restart.
  delay(1500);
  ESP.restart();
}

// Captive-portal helper: any URL we don't recognize redirects to "/".
// iOS / Android probe well-known URLs to detect the captive portal; sending
// a 302 to the setup page makes them auto-open the page on connect.
void handle_captive() {
  s_web.sendHeader("Location", String("http://") + AP_IP + "/", true);
  s_web.send(302, "text/plain", "");
}

}  // namespace

void begin() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);              // AP only — STA gets enabled if scan needs it
  WiFi.softAP(AP_SSID);            // open network for easy phone connect
  // Default AP IP is 192.168.4.1; that's what we hand out via DNS too.

  // Captive-portal DNS: every query → AP IP. iOS/Android then trigger their
  // captive-portal flow and pop our setup page.
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", IPAddress(192, 168, 4, 1));

  s_web.on("/",       HTTP_GET,  handle_root);
  s_web.on("/scan",   HTTP_GET,  handle_scan);
  s_web.on("/save",   HTTP_POST, handle_save);
  s_web.onNotFound(handle_captive);
  s_web.begin();

  s_running = true;
  LOG_I(TAG, "setup portal up — SSID '%s' @ %s", AP_SSID, AP_IP);
}

void update(uint32_t /*now_ms*/) {
  if (!s_running) return;
  s_dns.processNextRequest();
  s_web.handleClient();
}

bool is_running() { return s_running; }

}  // namespace robimon::services::setup_portal
