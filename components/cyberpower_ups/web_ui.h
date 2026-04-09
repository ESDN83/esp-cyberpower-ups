#pragma once

// ═══════════════════════════════════════════════════════════════
// CyberPower UPS Monitor — Web UI
//
// Simple status page and configuration for thresholds.
// Runs on port 80 via ESP-IDF HTTP server.
// ═══════════════════════════════════════════════════════════════

#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <cstring>
#include <cstdio>

namespace esphome {
namespace cyberpower_ups {

// Forward declare
class CyberpowerUpsComponent;

static httpd_handle_t httpd_ = nullptr;
static CyberpowerUpsComponent *web_component_ = nullptr;

// ── Log Ring accessors (defined in cyberpower_ups.h) ────────
extern char log_ring_[];
extern size_t log_ring_head_;
extern SemaphoreHandle_t log_ring_mutex_;
static constexpr size_t LOG_RING_SIZE_WEB = 8192;

// ── HTML Page Generator ─────────────────────────────────────
static esp_err_t root_handler_(httpd_req_t *req) {
  if (!web_component_) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Component not ready");
    return ESP_OK;
  }

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
    "<div class='card'><h2>Verbindung</h2>"
    "<div class='item'><span class='label'>USV verbunden</span>"
    "<span class='status %s'>%s</span></div>"
    "<div class='item'><span class='label'>Modell</span>"
    "<span class='value'>%s</span></div>"
    "<div class='item'><span class='label'>Serial</span>"
    "<span class='value'>%s</span></div>"
    "</div>",
    data.connected ? "ok" : "err",
    data.connected ? "Verbunden" : "Getrennt",
    data.model[0] ? data.model : "-",
    data.serial[0] ? data.serial : "-");

  httpd_resp_send_chunk(req, buf, len);

  // Sensor values
  float load_watt = (data.rating_power_va > 0 && data.load_percent > 0)
    ? (data.load_percent / 100.0f * data.rating_power_va * 0.6f)  // PF ~0.6 for typical UPS
    : 0;

  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Messwerte</h2><div class='grid'>"
    "<div class='item'><span class='label'>Netzspannung</span>"
    "<span class='value'>%.0f V</span></div>"
    "<div class='item'><span class='label'>Ausgangsspannung</span>"
    "<span class='value'>%.0f V</span></div>"
    "<div class='item'><span class='label'>Batterieladung</span>"
    "<span class='value'>%.0f %%</span></div>"
    "<div class='item'><span class='label'>Restlaufzeit</span>"
    "<span class='value'>%.0f min</span></div>"
    "<div class='item'><span class='label'>Last</span>"
    "<span class='value'>%.0f W (%.0f %%)</span></div>"
    "<div class='item'><span class='label'>Nennleistung</span>"
    "<span class='value'>%.0f VA</span></div>"
    "</div></div>",
    data.utility_voltage, data.output_voltage,
    data.battery_capacity, data.remaining_runtime_sec / 60.0f,
    load_watt, data.load_percent,
    data.rating_power_va);

  httpd_resp_send_chunk(req, buf, len);

  // Status flags
  const char *state_class = "ok";
  if (data.power_state == PowerState::POWER_FAIL_GRACE) state_class = "warn";
  if (data.power_state == PowerState::BATTERY_LOW) state_class = "err";
  if (data.power_state == PowerState::SHUTDOWN_IMMINENT) state_class = "err";

  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Status</h2>"
    "<div class='item'><span class='label'>Zustand</span>"
    "<span class='status %s'>%s</span></div>"
    "<div class='item'><span class='label'>Netz vorhanden</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Batteriebetrieb</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>L&auml;dt</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>&Uuml;berlast</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Batterie niedrig</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Batterie tauschen</span>"
    "<span class='%s'>%s</span></div>"
    "<div class='item'><span class='label'>Letztes Ereignis</span>"
    "<span class='value'>%s</span></div>"
    "</div>",
    state_class, power_state_str(data.power_state),
    data.ac_present ? "bool-on" : "bool-off", data.ac_present ? "Ja" : "Nein",
    data.on_battery ? "bool-on" : "bool-off", data.on_battery ? "Ja" : "Nein",
    data.charging ? "bool-on" : "bool-off", data.charging ? "Ja" : "Nein",
    data.overload ? "bool-on" : "bool-off", data.overload ? "Ja" : "Nein",
    data.battery_low_flag ? "bool-on" : "bool-off", data.battery_low_flag ? "Ja" : "Nein",
    data.replace_battery ? "bool-on" : "bool-off", data.replace_battery ? "Ja" : "Nein",
    data.last_event);

  httpd_resp_send_chunk(req, buf, len);

  // Config form
  len = snprintf(buf, sizeof(buf),
    "<div class='card'><h2>Schwellwerte</h2>"
    "<form action='/config' method='POST'>"
    "<div class='item'><span class='label'>Power-Failure Verz&ouml;gerung (s)</span>"
    "<input type='number' name='pf_delay' value='%lu' min='0' max='600'></div>"
    "<div class='item'><span class='label'>Battery-Low Laufzeit (s)</span>"
    "<input type='number' name='bl_runtime' value='%lu' min='0' max='3600'></div>"
    "<div class='item'><span class='label'>Battery-Low Kapazit&auml;t (%%)</span>"
    "<input type='number' name='bl_capacity' value='%lu' min='0' max='100'></div>"
    "<div style='text-align:right;margin-top:12px'><button type='submit'>Speichern</button></div>"
    "</form></div>",
    web_component_->get_power_fail_delay(),
    web_component_->get_battery_low_runtime(),
    web_component_->get_battery_low_capacity());

  httpd_resp_send_chunk(req, buf, len);

  // Auto-refresh
  len = snprintf(buf, sizeof(buf),
    "<script>setTimeout(()=>location.reload(), 5000);</script>"
    "</body></html>");
  httpd_resp_send_chunk(req, buf, len);

  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// ── Config POST handler ─────────────────────────────────────
static esp_err_t config_handler_(httpd_req_t *req) {
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

// ── Log endpoint ────────────────────────────────────────────
static esp_err_t log_handler_(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");

  if (!log_ring_mutex_) {
    httpd_resp_send(req, "Log not initialized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);

  // Simple: dump the entire ring buffer from head (oldest) to head-1 (newest)
  // Skip null bytes (unfilled portion)
  char line[256];
  size_t pos = log_ring_head_;
  size_t line_pos = 0;

  for (size_t i = 0; i < LOG_RING_SIZE_WEB; i++) {
    char c = log_ring_[pos];
    pos = (pos + 1) % LOG_RING_SIZE_WEB;

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

  xSemaphoreGive(log_ring_mutex_);

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

  httpd_uri_t log_uri = { .uri = "/log", .method = HTTP_GET, .handler = log_handler_ };
  httpd_register_uri_handler(httpd_, &log_uri);

  ESP_LOGI("web_ui", "Web UI started on port 80");
}

}  // namespace cyberpower_ups
}  // namespace esphome
