#pragma once

// ═══════════════════════════════════════════════════════════════
// CyberPower UPS Monitor — ESPHome Component
//
// Reads CyberPower USV data via USB HID Power Device Class and
// exposes values as native ESPHome sensors. Implements power
// failure detection with configurable thresholds.
//
// Hardware: ESP32-S3 (USB OTG on GPIO19/20)
// Target:  CyberPower BR1200ELCD (VID 0x0764)
// ═══════════════════════════════════════════════════════════════

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"

#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <atomic>
#include <cstring>
#include <cmath>

#include "hid_ups_protocol.h"

namespace esphome {
namespace cyberpower_ups {

static const char *const TAG = "cyberpower_ups";
static const char *const FW_BUILD_ID = "cyberpower-ups build 2026-04-10-a";

// CyberPower USB identifiers
static constexpr uint16_t CYBERPOWER_VID = 0x0764;
static constexpr uint16_t CYBERPOWER_PID = 0x0501;  // Common PID; verify on real hardware

// HID class requests
static constexpr uint8_t HID_REQ_GET_REPORT    = 0x01;
static constexpr uint8_t HID_REQ_SET_REPORT    = 0x09;
static constexpr uint8_t HID_REPORT_TYPE_INPUT  = 0x01;
static constexpr uint8_t HID_REPORT_TYPE_FEATURE = 0x03;

// HID descriptor type
static constexpr uint8_t USB_DT_HID        = 0x21;
static constexpr uint8_t USB_DT_HID_REPORT = 0x22;

// Polling interval
static constexpr uint32_t POLL_INTERVAL_MS = 5000;

// ── Power Event States ──────────────────────────────────────
enum class PowerState : uint8_t {
  NORMAL = 0,
  POWER_FAIL_GRACE,   // AC lost, waiting grace period
  BATTERY_LOW,        // Below runtime/capacity threshold
  SHUTDOWN_IMMINENT,  // UPS reports shutdown imminent
};

static const char *power_state_str(PowerState s) {
  switch (s) {
    case PowerState::NORMAL:            return "Normal";
    case PowerState::POWER_FAIL_GRACE:  return "Power Failure";
    case PowerState::BATTERY_LOW:       return "Battery Low";
    case PowerState::SHUTDOWN_IMMINENT: return "Shutdown Imminent";
    default: return "Unknown";
  }
}

// ── UPS Data (shared between tasks, protected by mutex) ─────
struct UpsData {
  // Sensor values
  float utility_voltage = 0;
  float output_voltage = 0;
  float battery_capacity = 0;
  float remaining_runtime_sec = 0;
  float load_percent = 0;
  float rating_voltage = 0;
  float rating_power_va = 0;

  // Binary status
  bool ac_present = true;
  bool on_battery = false;
  bool charging = false;
  bool overload = false;
  bool battery_low_flag = false;
  bool replace_battery = false;
  bool shutdown_imminent = false;

  // Device info
  char model[64] = {};
  char serial[64] = {};
  bool connected = false;

  // State machine
  PowerState power_state = PowerState::NORMAL;
  uint32_t power_fail_start_ms = 0;   // millis() when AC was lost
  char last_event[64] = "None";
  uint32_t last_event_time = 0;
};

// ── Ring buffer debug log ──────────────────────────────────
static constexpr size_t LOG_RING_SIZE = 8192;
static char log_ring_[LOG_RING_SIZE];
static size_t log_ring_head_ = 0;
static SemaphoreHandle_t log_ring_mutex_ = nullptr;

static void log_ring_init_() {
  if (!log_ring_mutex_) log_ring_mutex_ = xSemaphoreCreateMutex();
}

static void log_ring_append_(const char *msg) {
  if (!log_ring_mutex_) return;
  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);
  size_t len = strlen(msg);
  for (size_t i = 0; i < len; i++) {
    log_ring_[log_ring_head_] = msg[i];
    log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  }
  // Newline
  log_ring_[log_ring_head_] = '\n';
  log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  xSemaphoreGive(log_ring_mutex_);
}

// ═════════════════════════════════════════════════════════════
// Main Component
// ═════════════════════════════════════════════════════════════
class CyberpowerUpsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void setup() override;
  void loop() override;

  // ── Public accessors for Web UI ───────────────────────────
  UpsData get_data() {
    UpsData snapshot;
    if (!data_mutex_) return snapshot;  // Not yet initialized
    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    snapshot = data_;
    xSemaphoreGive(data_mutex_);
    return snapshot;
  }

  // Config accessors
  uint32_t get_power_fail_delay() const { return power_fail_delay_s_; }
  uint32_t get_battery_low_runtime() const { return battery_low_runtime_s_; }
  uint32_t get_battery_low_capacity() const { return battery_low_capacity_pct_; }

  void set_power_fail_delay(uint32_t s) { power_fail_delay_s_ = s; save_config_(); }
  void set_battery_low_runtime(uint32_t s) { battery_low_runtime_s_ = s; save_config_(); }
  void set_battery_low_capacity(uint32_t pct) { battery_low_capacity_pct_ = pct; save_config_(); }

 private:
  SemaphoreHandle_t data_mutex_ = nullptr;
  SemaphoreHandle_t ctrl_sem_ = nullptr;
  UpsData data_;
  std::atomic<bool> publish_pending_{false};

  // USB host state
  usb_host_client_handle_t client_hdl_ = nullptr;
  usb_device_handle_t dev_hdl_ = nullptr;
  uint8_t dev_addr_ = 0;
  uint8_t hid_iface_num_ = 0;
  uint16_t hid_report_desc_len_ = 0;
  HidReportMap report_map_;
  bool device_open_ = false;

  // Control transfer buffer
  static constexpr size_t CTRL_BUF_SIZE = 1024;
  usb_transfer_t *ctrl_xfer_ = nullptr;

  // Configurable thresholds (stored in NVS)
  uint32_t power_fail_delay_s_ = 60;
  uint32_t battery_low_runtime_s_ = 300;
  uint32_t battery_low_capacity_pct_ = 35;

  // ── NVS Config ────────────────────────────────────────────
  void load_config_() {
    nvs_handle_t nvs;
    if (nvs_open("ups_config", NVS_READONLY, &nvs) == ESP_OK) {
      nvs_get_u32(nvs, "pf_delay", &power_fail_delay_s_);
      nvs_get_u32(nvs, "bl_runtime", &battery_low_runtime_s_);
      nvs_get_u32(nvs, "bl_capacity", &battery_low_capacity_pct_);
      nvs_close(nvs);
      ESP_LOGI(TAG, "Config loaded: pf_delay=%lus, bl_runtime=%lus, bl_cap=%lu%%",
               power_fail_delay_s_, battery_low_runtime_s_, battery_low_capacity_pct_);
    } else {
      ESP_LOGI(TAG, "No saved config, using defaults");
    }
  }

  void save_config_() {
    nvs_handle_t nvs;
    if (nvs_open("ups_config", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_set_u32(nvs, "pf_delay", power_fail_delay_s_);
      nvs_set_u32(nvs, "bl_runtime", battery_low_runtime_s_);
      nvs_set_u32(nvs, "bl_capacity", battery_low_capacity_pct_);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }

  // ── USB Enum Filter (MUST return true or devices are silently skipped!) ──
  static bool enum_filter_allow_all_(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
    ESP_LOGI(TAG, "Enum filter: ALLOW VID=%04X PID=%04X", dev_desc->idVendor, dev_desc->idProduct);
    return true;
  }

  // ── USB Host Library Task ─────────────────────────────────
  static void usb_lib_task_entry_(void *arg) {
    auto *self = static_cast<CyberpowerUpsComponent *>(arg);
    self->usb_lib_task_();
  }

  void usb_lib_task_() {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    #ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
    host_config.enum_filter_cb = enum_filter_allow_all_;
    #endif

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: USB host install failed");
      vTaskDelete(nullptr);
      return;
    }
    ESP_LOGI(TAG, "USB host library installed");
    log_ring_append_("USB host library installed");

    while (true) {
      usb_host_lib_handle_events(portMAX_DELAY, nullptr);
    }
  }

  // ── USB Monitor Task ──────────────────────────────────────
  static void usb_mon_task_entry_(void *arg) {
    auto *self = static_cast<CyberpowerUpsComponent *>(arg);
    self->usb_mon_task_();
  }

  // Client event callback
  static void client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg) {
    auto *self = static_cast<CyberpowerUpsComponent *>(arg);
    switch (msg->event) {
      case USB_HOST_CLIENT_EVENT_NEW_DEV:
        self->dev_addr_ = msg->new_dev.address;
        ESP_LOGI(TAG, "New USB device at address %d", self->dev_addr_);
        break;
      case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected");
        log_ring_append_("USB device disconnected");
        self->handle_disconnect_();
        break;
    }
  }

  void usb_mon_task_() {
    // Small delay to let USB host library fully initialize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Register client
    usb_host_client_config_t client_config = {};
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = client_event_cb_;
    client_config.async.callback_arg = this;

    esp_err_t err = usb_host_client_register(&client_config, &client_hdl_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Client register failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: Client register failed");
      vTaskDelete(nullptr);
      return;
    }
    ESP_LOGI(TAG, "USB client registered successfully");
    log_ring_append_("USB client registered");

    // Allocate control transfer
    err = usb_host_transfer_alloc(CTRL_BUF_SIZE, 0, &ctrl_xfer_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Transfer alloc failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: Transfer alloc failed");
    }
    ctrl_xfer_->callback = ctrl_xfer_cb_;
    ctrl_xfer_->context = this;

    ESP_LOGI(TAG, "Waiting for USB devices... (hub support: %s)",
    #ifdef CONFIG_USB_HOST_HUBS_SUPPORTED
      "YES"
    #else
      "NO"
    #endif
    );
    log_ring_append_("Waiting for USB devices...");

    uint32_t heartbeat = 0;
    while (true) {
      // Pump client events
      usb_host_client_handle_events(client_hdl_, 200);

      // New device detected?
      if (dev_addr_ != 0 && !device_open_) {
        handle_new_device_();
      }

      // If connected, poll UPS data
      if (device_open_) {
        poll_ups_data_();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        heartbeat = 0;
      } else {
        // Heartbeat + active device scan every 10s when no device connected
        heartbeat++;
        if (heartbeat >= 50) {  // 50 * 200ms = 10s
          // Try to actively list devices known to the USB host library
          int num_devs = 0;
          uint8_t dev_addrs[8] = {};
          usb_host_device_addr_list_fill(sizeof(dev_addrs), dev_addrs, &num_devs);
          ESP_LOGW(TAG, "USB scan: %d device(s) known to host lib, cb_addr=%d, open=%d",
                   num_devs, dev_addr_, device_open_);
          char msg[80];
          snprintf(msg, sizeof(msg), "USB scan: %d devices, cb_addr=%d", num_devs, dev_addr_);
          log_ring_append_(msg);

          if (num_devs > 0) {
            for (int i = 0; i < num_devs; i++) {
              ESP_LOGW(TAG, "  Device at address %d (trying to open...)", dev_addrs[i]);
              // If callback missed it, try opening directly
              if (dev_addr_ == 0 && !device_open_) {
                dev_addr_ = dev_addrs[i];
              }
            }
          }
          heartbeat = 0;
        }
      }
    }
  }

  // ── Device Enumeration ────────────────────────────────────
  void handle_new_device_() {
    esp_err_t err = usb_host_device_open(client_hdl_, dev_addr_, &dev_hdl_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
      dev_addr_ = 0;
      return;
    }

    // Get device descriptor
    const usb_device_desc_t *desc;
    usb_host_get_device_descriptor(dev_hdl_, &desc);

    ESP_LOGI(TAG, "Device: VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    log_ring_append_("USB device opened");

    char msg[80];
    snprintf(msg, sizeof(msg), "VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    log_ring_append_(msg);

    // Check if it's a CyberPower UPS (or accept any HID Power Device)
    if (desc->idVendor != CYBERPOWER_VID) {
      ESP_LOGW(TAG, "Not a CyberPower device (VID 0x%04X), will try anyway", desc->idVendor);
      log_ring_append_("Warning: non-CyberPower VID, attempting HID Power Device anyway");
    }

    // Get string descriptors
    get_string_descriptor_(desc->iProduct, data_.model, sizeof(data_.model));
    get_string_descriptor_(desc->iSerialNumber, data_.serial, sizeof(data_.serial));
    ESP_LOGI(TAG, "Model: %s, Serial: %s", data_.model, data_.serial);

    // Find HID interface
    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(dev_hdl_, &config_desc);

    if (!find_hid_interface_(config_desc)) {
      ESP_LOGE(TAG, "No HID interface found");
      log_ring_append_("ERROR: No HID interface found");
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    // Claim the HID interface
    err = usb_host_interface_claim(client_hdl_, dev_hdl_, hid_iface_num_, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to claim interface %d: %s", hid_iface_num_, esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    ESP_LOGI(TAG, "HID interface %d claimed, report desc len=%d", hid_iface_num_, hid_report_desc_len_);

    // Read and parse HID report descriptor
    if (!read_hid_report_descriptor_()) {
      ESP_LOGE(TAG, "Failed to read/parse HID report descriptor");
      log_ring_append_("ERROR: HID report descriptor parse failed");
      usb_host_interface_release(client_hdl_, dev_hdl_, hid_iface_num_);
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    snprintf(msg, sizeof(msg), "HID parsed: %d fields found", (int)report_map_.fields.size());
    log_ring_append_(msg);
    ESP_LOGI(TAG, "%s", msg);

    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    data_.connected = true;

    // Extract VA rating from model name (e.g. "BR1200ELCD" → 1200)
    // CyberPower naming: BR/CP/PR + digits (VA rating) + suffix
    if (data_.rating_power_va <= 0) {
      const char *p = data_.model;
      while (*p && !(*p >= '0' && *p <= '9')) p++;  // skip letters
      if (*p) {
        data_.rating_power_va = (float)atoi(p);
        ESP_LOGI(TAG, "Rating VA from model name: %.0f", data_.rating_power_va);
      }
    }
    xSemaphoreGive(data_mutex_);

    device_open_ = true;
    log_ring_append_("UPS connected and ready");
  }

  void handle_disconnect_() {
    if (device_open_) {
      usb_host_interface_release(client_hdl_, dev_hdl_, hid_iface_num_);
      usb_host_device_close(client_hdl_, dev_hdl_);
    }
    device_open_ = false;
    dev_addr_ = 0;
    dev_hdl_ = nullptr;
    report_map_.fields.clear();

    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    data_.connected = false;
    data_.power_state = PowerState::NORMAL;
    xSemaphoreGive(data_mutex_);

    publish_pending_ = true;
  }

  // ── Find HID interface in config descriptor ───────────────
  bool find_hid_interface_(const usb_config_desc_t *config_desc) {
    const uint8_t *p = (const uint8_t *)config_desc;
    size_t total_len = config_desc->wTotalLength;
    size_t offset = 0;

    while (offset < total_len) {
      uint8_t desc_len = p[offset];
      uint8_t desc_type = p[offset + 1];
      if (desc_len == 0) break;

      if (desc_type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
        const usb_intf_desc_t *iface = (const usb_intf_desc_t *)&p[offset];
        // HID class = 0x03
        if (iface->bInterfaceClass == 0x03) {
          hid_iface_num_ = iface->bInterfaceNumber;

          // Look for HID descriptor right after interface descriptor
          size_t next = offset + desc_len;
          while (next < total_len) {
            uint8_t nd_len = p[next];
            uint8_t nd_type = p[next + 1];
            if (nd_len == 0) break;

            if (nd_type == USB_DT_HID && nd_len >= 9) {
              // HID descriptor: byte 7-8 = wDescriptorLength (report desc length)
              hid_report_desc_len_ = p[next + 7] | (p[next + 8] << 8);
              return true;
            }

            // Stop if we hit another interface descriptor
            if (nd_type == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;
            next += nd_len;
          }

          // Fallback: assume 256 bytes if HID descriptor not found
          ESP_LOGW(TAG, "HID descriptor not found, assuming report desc len=256");
          hid_report_desc_len_ = 256;
          return true;
        }
      }
      offset += desc_len;
    }
    return false;
  }

  // ── Control Transfer Helpers ──────────────────────────────
  static void ctrl_xfer_cb_(usb_transfer_t *transfer) {
    auto *self = static_cast<CyberpowerUpsComponent *>(transfer->context);
    xSemaphoreGive(self->ctrl_sem_);
  }

  // Synchronous control transfer — blocks until complete
  esp_err_t ctrl_transfer_sync_(uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, uint8_t *data_out = nullptr) {
    ctrl_xfer_->device_handle = dev_hdl_;
    ctrl_xfer_->bEndpointAddress = 0;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer_->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    // If OUT transfer, copy data after setup packet
    if (!(bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      memcpy(ctrl_xfer_->data_buffer + sizeof(usb_setup_packet_t), data_out, wLength);
    }

    ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + wLength;

    // Submit and pump client events while waiting for completion.
    // The callback is delivered via usb_host_client_handle_events(),
    // so we must keep calling it or the callback never fires (deadlock).
    xSemaphoreTake(ctrl_sem_, 0);  // Clear semaphore
    esp_err_t err = usb_host_transfer_submit_control(client_hdl_, ctrl_xfer_);
    if (err != ESP_OK) return err;

    bool got_it = false;
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
      if (xSemaphoreTake(ctrl_sem_, 0) == pdTRUE) {
        got_it = true;
        break;
      }
      usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(50));
    }

    if (!got_it) {
      ESP_LOGE(TAG, "Control transfer timeout");
      return ESP_ERR_TIMEOUT;
    }

    if (ctrl_xfer_->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGE(TAG, "Control transfer failed, status=%d", ctrl_xfer_->status);
      return ESP_FAIL;
    }

    // Copy response data back
    if ((bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      size_t actual = ctrl_xfer_->actual_num_bytes - sizeof(usb_setup_packet_t);
      if (actual > wLength) actual = wLength;
      memcpy(data_out, ctrl_xfer_->data_buffer + sizeof(usb_setup_packet_t), actual);
    }

    return ESP_OK;
  }

  // ── Get String Descriptor ─────────────────────────────────
  void get_string_descriptor_(uint8_t index, char *buf, size_t buf_size) {
    buf[0] = '\0';
    if (index == 0) return;

    uint8_t tmp[128];
    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
      USB_B_REQUEST_GET_DESCRIPTOR,
      (USB_B_DESCRIPTOR_TYPE_STRING << 8) | index,
      0x0409,  // English
      sizeof(tmp), tmp);

    if (err != ESP_OK || tmp[0] < 4) return;

    // Convert UTF-16LE to ASCII
    size_t str_len = (tmp[0] - 2) / 2;
    if (str_len >= buf_size) str_len = buf_size - 1;
    for (size_t i = 0; i < str_len; i++) {
      uint16_t ch = tmp[2 + i * 2] | (tmp[3 + i * 2] << 8);
      buf[i] = (ch < 128) ? (char)ch : '?';
    }
    buf[str_len] = '\0';
  }

  // ── Read & Parse HID Report Descriptor ────────────────────
  bool read_hid_report_descriptor_() {
    size_t desc_len = hid_report_desc_len_;
    if (desc_len > CTRL_BUF_SIZE - sizeof(usb_setup_packet_t))
      desc_len = CTRL_BUF_SIZE - sizeof(usb_setup_packet_t);

    uint8_t *desc_buf = (uint8_t *)malloc(desc_len);
    if (!desc_buf) return false;

    // GET_DESCRIPTOR (HID Report Descriptor) — Standard request to interface
    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
      USB_B_REQUEST_GET_DESCRIPTOR,
      (USB_DT_HID_REPORT << 8),
      hid_iface_num_,
      desc_len, desc_buf);

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to get HID report descriptor: %s", esp_err_to_name(err));
      free(desc_buf);
      return false;
    }

    // Log full descriptor in chunks for offline analysis
    ESP_LOGI(TAG, "HID Report Desc [%d bytes]:", (int)desc_len);
    char hex[128];
    for (size_t off = 0; off < desc_len; off += 32) {
      size_t chunk = (desc_len - off > 32) ? 32 : desc_len - off;
      for (size_t i = 0; i < chunk; i++) {
        snprintf(hex + i * 3, 4, "%02X ", desc_buf[off + i]);
      }
      hex[chunk * 3] = '\0';
      ESP_LOGI(TAG, "  @%03d: %s", (int)off, hex);
    }

    // Parse
    report_map_ = {};
    bool ok = parse_report_descriptor(desc_buf, desc_len, report_map_);
    free(desc_buf);

    if (ok) {
      // Log ALL found fields (at INFO level so they're always visible)
      for (size_t i = 0; i < report_map_.fields.size(); i++) {
        auto &f = report_map_.fields[i];
        ESP_LOGI(TAG, "  [%02d] page=0x%04X usage=0x%04X rid=%d type=%d exp=%d bits=%d@%d",
                 (int)i, f.usage_page, f.usage, f.report_id, (int)f.report_type,
                 f.unit_exponent, f.bit_size, f.bit_offset);
      }
    }

    return ok;
  }

  // ── Read a single HID Feature/Input Report ────────────────
  bool read_hid_report_(uint8_t report_id, ReportType type, uint8_t *buf, size_t buf_size) {
    uint8_t hid_type = (type == ReportType::INPUT) ? HID_REPORT_TYPE_INPUT : HID_REPORT_TYPE_FEATURE;
    uint16_t wValue = (hid_type << 8) | report_id;

    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
      HID_REQ_GET_REPORT,
      wValue,
      hid_iface_num_,
      buf_size, buf);

    return (err == ESP_OK);
  }

  // ── Read a single field value from UPS ────────────────────
  bool read_field_value_(const HidField *field, int32_t &value) {
    if (!field) return false;

    // Calculate report data size (max bit offset + size in this report)
    size_t report_bytes = 0;
    for (auto &f : report_map_.fields) {
      if (f.report_id == field->report_id && f.report_type == field->report_type) {
        size_t end = (f.bit_offset + f.bit_size + 7) / 8;
        if (end > report_bytes) report_bytes = end;
      }
    }
    if (report_bytes == 0) report_bytes = 8;

    // GET_REPORT response includes report ID as first byte, so request +1
    size_t xfer_bytes = report_bytes + 1;
    if (xfer_bytes > 63) xfer_bytes = 63;

    uint8_t report_buf[64] = {};
    if (!read_hid_report_(field->report_id, field->report_type, report_buf, xfer_bytes))
      return false;

    // GET_REPORT always prepends the report ID byte — skip it
    uint8_t *data = report_buf + 1;

    value = extract_field_value(data, *field);
    return true;
  }

  // ── Poll all UPS data ─────────────────────────────────────
  void poll_ups_data_() {
    int32_t val;

    xSemaphoreTake(data_mutex_, portMAX_DELAY);

    // ── Sensor values ──

    // Input (utility) voltage — Page 0x84, within Input collection
    auto *f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_VOLTAGE);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "Voltage raw=%d exp=%d", val, f->unit_exponent);
      data_.utility_voltage = (float)val;  // CyberPower reports whole volts
    }

    // Battery capacity
    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_REMAINING_CAPACITY);
    if (f && read_field_value_(f, val)) {
      data_.battery_capacity = (float)val;
    }

    // Runtime to empty (seconds)
    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_RUNTIME_TO_EMPTY);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "Runtime raw=%d exp=%d", val, f->unit_exponent);
      data_.remaining_runtime_sec = (float)val;  // CyberPower reports whole seconds
    }

    // Percent load
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_PERCENT_LOAD);
    if (f && read_field_value_(f, val)) {
      data_.load_percent = (float)val;
    }

    // Config voltage (rating)
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_CONFIG_VOLTAGE);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "ConfigVoltage raw=%d exp=%d", val, f->unit_exponent);
      data_.rating_voltage = (float)val;
    }

    // Config apparent power (rating VA)
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_CONFIG_APPARENT_POWER);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "ConfigPower raw=%d exp=%d", val, f->unit_exponent);
      data_.rating_power_va = (float)val;
    }
    // rating_power_va may also come from model name (set during connect)

    // Output voltage — try to find a second voltage field in output collection
    // Many CyberPower UPS report output voltage same as input when on AC
    // For now, use the same value (can be refined once we see real report descriptor)
    data_.output_voltage = data_.utility_voltage;

    // ── Binary status ──

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_AC_PRESENT);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "ACPresent rid=%d raw=%d bits=%d@%d", f->report_id, val, f->bit_size, f->bit_offset);
      data_.ac_present = (val != 0);
    } else {
      ESP_LOGW(TAG, "ACPresent field %s", f ? "read failed" : "NOT FOUND");
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_DISCHARGING);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "Discharging rid=%d raw=%d", f->report_id, val);
      data_.on_battery = (val != 0);
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_CHARGING);
    if (f && read_field_value_(f, val)) {
      ESP_LOGD(TAG, "Charging rid=%d raw=%d", f->report_id, val);
      data_.charging = (val != 0);
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_OVERLOAD);
    if (f && read_field_value_(f, val)) {
      data_.overload = (val != 0);
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_BELOW_REMAINING_CAP);
    if (f && read_field_value_(f, val)) {
      data_.battery_low_flag = (val != 0);
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_NEED_REPLACEMENT);
    if (f && read_field_value_(f, val)) {
      data_.replace_battery = (val != 0);
    }

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_SHUTDOWN_IMMINENT);
    if (f && read_field_value_(f, val)) {
      data_.shutdown_imminent = (val != 0);
    }

    // ── State Machine ──
    update_power_state_();

    xSemaphoreGive(data_mutex_);

    publish_pending_ = true;
  }

  // ── Power State Machine ───────────────────────────────────
  // Must be called with data_mutex_ held!
  void update_power_state_() {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);  // millis

    // ── Transitions ──

    // AC returned -> back to normal
    if (data_.ac_present && !data_.on_battery) {
      if (data_.power_state != PowerState::NORMAL) {
        data_.power_state = PowerState::NORMAL;
        set_event_("AC Restored");
        ESP_LOGI(TAG, "AC restored, state -> NORMAL");
        log_ring_append_("AC restored -> NORMAL");
      }
      return;
    }

    // UPS reports shutdown imminent
    if (data_.shutdown_imminent) {
      if (data_.power_state != PowerState::SHUTDOWN_IMMINENT) {
        data_.power_state = PowerState::SHUTDOWN_IMMINENT;
        set_event_("Shutdown Imminent");
        ESP_LOGW(TAG, "UPS: Shutdown imminent!");
        log_ring_append_("SHUTDOWN IMMINENT!");
      }
      return;
    }

    // Check battery low thresholds
    bool runtime_low = (data_.remaining_runtime_sec > 0 &&
                        data_.remaining_runtime_sec < (float)battery_low_runtime_s_);
    bool capacity_low = (data_.battery_capacity > 0 &&
                         data_.battery_capacity < (float)battery_low_capacity_pct_);

    if (data_.on_battery && (runtime_low || capacity_low || data_.battery_low_flag)) {
      if (data_.power_state != PowerState::BATTERY_LOW &&
          data_.power_state != PowerState::SHUTDOWN_IMMINENT) {
        data_.power_state = PowerState::BATTERY_LOW;
        set_event_("Battery Low");
        ESP_LOGW(TAG, "Battery low! runtime=%.0fs, capacity=%.0f%%",
                 data_.remaining_runtime_sec, data_.battery_capacity);
        log_ring_append_("BATTERY LOW!");
      }
      return;
    }

    // On battery but not yet low
    if (data_.on_battery) {
      if (data_.power_state == PowerState::NORMAL) {
        // Start grace period
        data_.power_state = PowerState::POWER_FAIL_GRACE;
        data_.power_fail_start_ms = now;
        ESP_LOGW(TAG, "Power failure detected, grace period %lus", power_fail_delay_s_);
        log_ring_append_("Power failure detected, starting grace period");
      }

      if (data_.power_state == PowerState::POWER_FAIL_GRACE) {
        uint32_t elapsed = now - data_.power_fail_start_ms;
        if (elapsed >= power_fail_delay_s_ * 1000) {
          // Grace period expired — fire power failure event
          set_event_("Power Failure");
          ESP_LOGW(TAG, "Power failure grace period expired!");
          log_ring_append_("Power failure event fired (grace expired)");
        }
      }
    }
  }

  void set_event_(const char *event) {
    strncpy(data_.last_event, event, sizeof(data_.last_event) - 1);
    data_.last_event[sizeof(data_.last_event) - 1] = '\0';
    data_.last_event_time = (uint32_t)(esp_timer_get_time() / 1000000);  // seconds since boot

    // Fire ESPHome custom event
    fire_homeassistant_event_(event);
  }

  void fire_homeassistant_event_(const char *event_type) {
    ESP_LOGI(TAG, "Firing HA event: cyberpower_ups / %s", event_type);
  }

  // ── Publish Sensor Values to ESPHome ──────────────────────
  void publish_sensors_(const UpsData &d) {
    // Values are stored in UpsData and read by template sensor lambdas in YAML.
    // No direct publishing needed — ESPHome template sensors poll get_data().
  }
};

// ── Include web UI (needs full class definition) ────────────
#include "web_ui.h"

// ── Deferred method implementations ─────────────────────────
inline void CyberpowerUpsComponent::setup() {
  ESP_LOGI(TAG, "CyberPower UPS Monitor starting... (%s)", FW_BUILD_ID);
  log_ring_init_();

  data_mutex_ = xSemaphoreCreateMutex();
  ctrl_sem_ = xSemaphoreCreateBinary();

  // Load config from NVS
  load_config_();

  // Start USB host library task
  xTaskCreatePinnedToCore(usb_lib_task_entry_, "usb_lib", 8192, this, 10, nullptr, 0);

  // Start USB monitor task
  xTaskCreatePinnedToCore(usb_mon_task_entry_, "usb_mon", 8192, this, 5, nullptr, 1);

  // Start web UI
  start_web_ui_(this);

  log_ring_append_("Component initialized");
}

inline void CyberpowerUpsComponent::loop() {
  // Publish sensor values to ESPHome from main loop (thread-safe)
  if (!publish_pending_) return;
  publish_pending_ = false;

  UpsData snapshot;
  xSemaphoreTake(data_mutex_, portMAX_DELAY);
  snapshot = data_;
  xSemaphoreGive(data_mutex_);

  publish_sensors_(snapshot);
}

}  // namespace cyberpower_ups
}  // namespace esphome
