#pragma once

// ═══════════════════════════════════════════════════════════════
// CyberPower UPS Monitor — Web UI
//
// Simple status page and configuration for thresholds.
// Runs on port 80 via ESP-IDF HTTP server.
//
// NOTE: This file is included from cyberpower_ups.h AFTER the
// full class definition, so CyberpowerUpsComponent is complete.
// ═══════════════════════════════════════════════════════════════

#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <cstring>
#include <cstdio>

// No namespace open here — we are already inside esphome::cyberpower_ups
// because this file is #included from within that namespace in cyberpower_ups.h

static httpd_handle_t httpd_ = nullptr;
static CyberpowerUpsComponent *web_component_ = nullptr;

// ── Base64 decode (for Basic Auth) ──────────────────────────
static int base64_decode_char_(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t base64_decode_(const char *in, char *out, size_t out_size) {
  size_t out_pos = 0;
  int buf = 0;
  int bits = 0;
  while (*in && *in != '=' && out_pos < out_size - 1) {
    int v = base64_decode_char_(*in++);
    if (v < 0) continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[out_pos++] = (char)((buf >> bits) & 0xFF);
    }
  }
  out[out_pos] = '\0';
  return out_pos;
}

// ── Auth check — returns true if authorized or no auth required ─
// On failure, sends 401 with WWW-Authenticate and returns false.
static bool check_auth_(httpd_req_t *req) {
  if (!web_component_ || !web_component_->has_password()) return true;

  char hdr[128];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"UPS Monitor\"");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return false;
  }

  // Expect "Basic <base64(user:password)>"
  const char *b64 = strstr(hdr, "Basic ");
  if (!b64) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"UPS Monitor\"");
    httpd_resp_send(req, "Bad auth header", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  b64 += 6;

  char decoded[128];
  base64_decode_(b64, decoded, sizeof(decoded));

  // decoded = "user:password" — we accept any username; only password must match
  const char *colon = strchr(decoded, ':');
  const char *supplied_pw = colon ? colon + 1 : decoded;
  const char *expected = web_component_->get_password();

  if (strcmp(supplied_pw, expected) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"UPS Monitor\"");
    httpd_resp_send(req, "Invalid credentials", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  return true;
}

// ── HTML Page Generator ─────────────────────────────────────
static esp_err_t root_handler_(httpd_req_t *req) {
  if (!web_component_) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Component not ready");
    return ESP_OK;
  }
  if (!check_auth_(req)) return ESP_OK;

  auto data = web_component_->get_data();

  char buf[4096];
  int len = snprintf(buf, sizeof(buf),
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>CyberPower UPS Monitor</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:16px;background:#1a1a2e;color:#e0e0e0}"
    "h1{color:#64ffda;font-size:1.4em;margin:0 0 16px}"
    ".card{background:#16213e;border-radius:12px;padding:16px;margin:0 0 12px;box-shadow:0 2px 8px rgba(0,0,0,.3)}"
    ".card h2{margin:0 0 12px;font-size:1.1em;color:#82b1ff}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
    ".item{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #1a1a2e}"
    ".label{color:#888}.value{font-weight:600;color:#fff}"
    ".status{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.85em;font-weight:600}"
    ".ok{background:#1b5e20;color:#69f0ae}.warn{background:#e65100;color:#ffab40}.err{background:#b71c1c;color:#ef9a9a}"
    ".off{background:#333;color:#888}"
    "input[type=number]{width:80px;padding:4px 8px;border:1px solid #444;border-radius:6px;background:#0f3460;color:#fff}"
    "button{padding:8px 20px;border:none;border-radius:6px;background:#64ffda;color:#000;font-weight:600;cursor:pointer}"
    "button:hover{background:#00e5ff}"
    ".bool-on{color:#69f0ae}.bool-off{color:#666}"
    "</style></head><body>"
    "<h1>&#9889; CyberPower UPS Monitor</h1>"

    // Connection status
    "<div class='card'><h2>Connection</h2>"
    "<div class='item'><span class='label'>UPS Connected</span>"
    "<span class='status %s'>%s</span></div>"
    "<div class='item'><span class='label'>Model</span>"
    "<span class='value'>%s</span></div>"
    "<div class='item'><span class='label'>Serial</span>"
    "<span class='value'>%s</span></div>"
    "</div>",
    data.connected ? "ok" : "err",
    data.connected ? "Connected" : "Disconnected",
    data.model[0] ? data.model : "-",
    data.serial[0] ? data.serial : "-");

  httpd_resp_send_chunk(req, buf, len);

  // Sensor values
  float load_watt = (data.rating_power_va > 0 && data.load_percent > 0)
    ? (data.load_percent / 100.0f * data.rating_power_va * 0.6f)  // PF ~0.6 for typical UPS
    : 0;

  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Readings</h2><div class='grid'>"
    "<div class='item'><span class='label'>Utility Voltage</span>"
    "<span class='value'>%.0f V</span></div>"
    "<div class='item'><span class='label'>Output Voltage</span>"
    "<span class='value'>%.0f V</span></div>"
    "<div class='item'><span class='label'>Battery Capacity</span>"
    "<span class='value'>%.0f %%</span></div>"
    "<div class='item'><span class='label'>Remaining Runtime</span>"
    "<span class='value'>%.0f min</span></div>"
    "<div class='item'><span class='label'>Load</span>"
    "<span class='value'>%.0f W (%.0f %%)</span></div>"
    "<div class='item'><span class='label'>Rating Power</span>"
    "<span class='value'>%.0f VA</span></div>"
    "</div></div>",
    data.utility_voltage, data.output_voltage,
    data.battery_capacity, data.remaining_runtime_sec / 60.0f,
    load_watt, data.load_percent,
    data.rating_power_va);

  httpd_resp_send_chunk(req, buf, len);

  // Status flags
  const char *state_css = "ok";
  if (data.power_state == PowerState::POWER_FAIL_GRACE) state_css = "warn";
  if (data.power_state == PowerState::BATTERY_LOW) state_css = "err";
  if (data.power_state == PowerState::SHUTDOWN_IMMINENT) state_css = "err";

  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Status</h2>"
    "<div class='item'><span class='label'>State</span>"
    "<span class='status %s'>%s</span></div>"
    "<div class='item'><span class='label'>AC Present</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>On Battery</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Charging</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Overload</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Battery Low</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Replace Battery</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Last Event</span>"
    "<span class='value'>%s</span></div>"
    "</div>",
    state_css, power_state_str(data.power_state),
    data.ac_present ? "bool-on" : "bool-off", data.ac_present ? "Yes" : "No",
    data.on_battery ? "bool-on" : "bool-off", data.on_battery ? "Yes" : "No",
    data.charging ? "bool-on" : "bool-off", data.charging ? "Yes" : "No",
    data.overload ? "bool-on" : "bool-off", data.overload ? "Yes" : "No",
    data.battery_low_flag ? "bool-on" : "bool-off", data.battery_low_flag ? "Yes" : "No",
    data.replace_battery ? "bool-on" : "bool-off", data.replace_battery ? "Yes" : "No",
    data.last_event);

  httpd_resp_send_chunk(req, buf, len);

  // Config form
  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Thresholds</h2>"
    "<form action='/config' method='POST'>"
    "<div class='item'><span class='label'>Power Failure Delay (s)</span>"
    "<input type='number' name='pf_delay' value='%lu' min='0' max='600'></div>"
    "<div class='item'><span class='label'>Battery Low Runtime (s)</span>"
    "<input type='number' name='bl_runtime' value='%lu' min='0' max='3600'></div>"
    "<div class='item'><span class='label'>Battery Low Capacity (%%)</span>"
    "<input type='number' name='bl_capacity' value='%lu' min='0' max='100'></div>"
    "<div style='text-align:right;margin-top:12px'><button type='submit'>Save</button></div>"
    "</form></div>",
    web_component_->get_power_fail_delay(),
    web_component_->get_battery_low_runtime(),
    web_component_->get_battery_low_capacity());

  httpd_resp_send_chunk(req, buf, len);

  // Password form — blank "new" field means no change; blank password disables auth.
  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Access Control</h2>"
    "<div class='item'><span class='label'>Auth Status</span>"
    "<span class='status %s'>%s</span></div>"
    "<form action='/password' method='POST'>"
    "<div class='item'><span class='label'>New Password</span>"
    "<input type='password' name='new' placeholder='leave blank to disable'></div>"
    "<div style='text-align:right;margin-top:12px'>"
    "<button type='submit'>Update Password</button></div>"
    "</form>"
    "<div style='color:#888;font-size:.85em;margin-top:8px'>"
    "User: admin &middot; leave password empty to disable auth</div>"
    "</div>",
    web_component_->has_password() ? "ok" : "warn",
    web_component_->has_password() ? "Enabled" : "Disabled (open)");

  httpd_resp_send_chunk(req, buf, len);

  // Auto-refresh — but skip reload while the user is editing a threshold input,
  // otherwise their typed value is wiped every 5 seconds.
  len = snprintf(buf, sizeof(buf),
    "<script>"
    "setInterval(function(){"
      "var a=document.activeElement;"
      "if(a && (a.tagName==='INPUT' || a.tagName==='TEXTAREA' || a.tagName==='SELECT'))return;"
      "location.reload();"
    "},5000);"
    "</script>"
    "</body></html>");
  httpd_resp_send_chunk(req, buf, len);

  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// ── Config POST handler ─────────────────────────────────────
static esp_err_t config_handler_(httpd_req_t *req) {
  if (!check_auth_(req)) return ESP_OK;
  char body[256] = {};
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
    return ESP_OK;
  }
  body[recv] = '\0';

  // Parse form data (URL-encoded)
  auto get_val = [&](const char *key) -> uint32_t {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    char *p = strstr(body, search);
    if (!p) return 0;
    return (uint32_t)atoi(p + strlen(search));
  };

  if (web_component_) {
    web_component_->set_power_fail_delay(get_val("pf_delay"));
    web_component_->set_battery_low_runtime(get_val("bl_runtime"));
    web_component_->set_battery_low_capacity(get_val("bl_capacity"));
  }

  // Redirect back
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// ── Password POST handler ───────────────────────────────────
// Requires current auth (if set) — prevents unauthenticated password change.
static esp_err_t password_handler_(httpd_req_t *req) {
  if (!check_auth_(req)) return ESP_OK;

  char body[256] = {};
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
    return ESP_OK;
  }
  body[recv] = '\0';

  char new_pw[64] = {};
  char *p = strstr(body, "new=");
  if (p) {
    p += 4;
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(new_pw) - 1) {
      // URL-decode: '+' → space, %XX → byte
      if (*p == '+') { new_pw[i++] = ' '; p++; }
      else if (*p == '%' && p[1] && p[2]) {
        char hex[3] = { p[1], p[2], '\0' };
        new_pw[i++] = (char)strtol(hex, nullptr, 16);
        p += 3;
      } else {
        new_pw[i++] = *p++;
      }
    }
    new_pw[i] = '\0';
  }

  if (web_component_) web_component_->set_password(new_pw);

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// ── Log endpoint ────────────────────────────────────────────
// Copies the ring buffer to a local snapshot under the mutex, then releases
// the mutex BEFORE sending. A slow HTTP client must not block log_ring_append_
// (called from the USB task on every event).
static esp_err_t log_handler_(httpd_req_t *req) {
  if (!check_auth_(req)) return ESP_OK;
  httpd_resp_set_type(req, "text/plain");

  if (!log_ring_mutex_) {
    httpd_resp_send(req, "Log not initialized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Snapshot under mutex (fast)
  static char snapshot[LOG_RING_SIZE];
  size_t head_snap;
  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);
  memcpy(snapshot, log_ring_, LOG_RING_SIZE);
  head_snap = log_ring_head_;
  xSemaphoreGive(log_ring_mutex_);

  // Send (no mutex held — slow clients can't starve the USB task)
  char line[256];
  size_t pos = head_snap;
  size_t line_pos = 0;

  for (size_t i = 0; i < LOG_RING_SIZE; i++) {
    char c = snapshot[pos];
    pos = (pos + 1) % LOG_RING_SIZE;

    if (c == '\0') continue;
    if (c == '\n' || line_pos >= sizeof(line) - 2) {
      line[line_pos] = '\n';
      line[line_pos + 1] = '\0';
      httpd_resp_send_chunk(req, line, line_pos + 1);
      line_pos = 0;
    } else {
      line[line_pos++] = c;
    }
  }

  if (line_pos > 0) {
    line[line_pos] = '\0';
    httpd_resp_send_chunk(req, line, line_pos);
  }

  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// ── Start Web Server ────────────────────────────────────────
static void start_web_ui_(CyberpowerUpsComponent *component) {
  web_component_ = component;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;

  if (httpd_start(&httpd_, &config) != ESP_OK) {
    ESP_LOGE("web_ui", "Failed to start HTTP server");
    return;
  }

  httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler_ };
  httpd_register_uri_handler(httpd_, &root_uri);

  httpd_uri_t config_uri = { .uri = "/config", .method = HTTP_POST, .handler = config_handler_ };
  httpd_register_uri_handler(httpd_, &config_uri);

  httpd_uri_t password_uri = { .uri = "/password", .method = HTTP_POST, .handler = password_handler_ };
  httpd_register_uri_handler(httpd_, &password_uri);

  httpd_uri_t log_uri = { .uri = "/log", .method = HTTP_GET, .handler = log_handler_ };
  httpd_register_uri_handler(httpd_, &log_uri);

  ESP_LOGI("web_ui", "Web UI started on port 80");
}
