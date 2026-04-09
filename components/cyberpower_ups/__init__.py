import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import add_idf_sdkconfig_option, include_builtin_idf_component

DEPENDENCIES = ["network"]
CODEOWNERS = []

cyberpower_ups_ns = cg.esphome_ns.namespace("cyberpower_ups")
CyberpowerUpsComponent = cyberpower_ups_ns.class_("CyberpowerUpsComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CyberpowerUpsComponent),
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # USB host on ESP32-S3
    add_idf_sdkconfig_option("CONFIG_USB_OTG_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)

    # HTTP server for status web UI
    add_idf_sdkconfig_option("CONFIG_HTTPD_MAX_REQ_HDR_LEN", 1024)
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 16)

    # USB HID host support
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", False)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK", True)

    # PSRAM can cause USB host interrupts to be missed (ESP-IDF #9519)
    add_idf_sdkconfig_option("CONFIG_SPIRAM", False)
    add_idf_sdkconfig_option("CONFIG_ESP32S3_SPIRAM_SUPPORT", False)

    # No MQTT needed — uses native ESPHome API for HA integration
