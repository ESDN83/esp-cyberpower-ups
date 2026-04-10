# ESP CyberPower UPS Monitor

ESP32-S3 reads a CyberPower UPS directly via USB HID and exposes all data as native ESPHome sensors in Home Assistant.

**No NUT, no pwrstat, no SSH — direct USB → ESP32 → Home Assistant.**

## Features

- Direct USB HID communication with CyberPower UPS (BR1200ELCD and compatible)
- Native ESPHome sensors (voltage, battery, load, runtime, status)
- Power failure detection with configurable thresholds
- HA events for automations (Power Failure, Battery Low, Shutdown Imminent)
- Web UI for live status and configuration
- No separate server or daemon required

## Hardware

| Component | Details |
|-----------|---------|
| **UPS** | CyberPower BR1200ELCD (or compatible) |
| **MCU** | ESP32-S3-DevKitC-1 |
| **Connection** | USB-A cable from UPS directly to ESP32-S3 OTG Port |

> **Important:** Set logger to `UART0`! The default `USB_SERIAL_JTAG` blocks the GPIO19/20 pins needed for USB Host.

## Installation

1. **Create ESPHome YAML** (see `esphome/cyberpower-ups.yaml`)
2. **Flash** via ESPHome Dashboard or CLI
3. **Connect USB cable** from UPS to ESP32-S3 OTG Port
4. **Home Assistant** discovers the device automatically via ESPHome API

### Minimal YAML Config

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/ESDN83/esp-cyberpower-ups
      ref: master
    components: [cyberpower_ups]

cyberpower_ups:
  id: ups
```

See `esphome/cyberpower-ups.yaml` for the full configuration with all sensors.

## Sensors in Home Assistant

### Readings
| Sensor | Unit | Description |
|--------|------|-------------|
| Utility Voltage | V | Input voltage from mains |
| Output Voltage | V | UPS output voltage |
| Battery Capacity | % | Current battery capacity |
| Remaining Runtime | min | Estimated remaining runtime on battery |
| Load | W | Current load in watts |
| Load Percent | % | Load percentage |

### Status
| Sensor | Type | Description |
|--------|------|-------------|
| UPS Connected | Binary | USB connection to UPS |
| AC Present | Binary | Mains voltage present |
| On Battery | Binary | UPS running on battery |
| Charging | Binary | Battery is charging |
| Overload | Binary | UPS is overloaded |
| UPS Status | Text | Normal / Power Failure / Battery Low / Shutdown |

## Power Failure Logic

The component implements a state machine that replicates the pwrstat daemon logic:

```
NORMAL → POWER_FAIL (after AC loss, 60s delay) → BATTERY_LOW → SHUTDOWN_IMMINENT
```

### Thresholds (configurable via Web UI)

| Parameter | Default | Description |
|-----------|---------|-------------|
| Power Failure Delay | 60 s | Time before power failure event is fired |
| Battery Low Runtime | 300 s | Runtime threshold |
| Battery Low Capacity | 35 % | Capacity threshold |

### HA Automation Example

```yaml
# Notification on power failure
automation:
  - alias: "UPS Power Failure"
    trigger:
      - platform: state
        entity_id: text_sensor.cyberpower_ups_monitor_ups_status
        to: "Power Failure"
    action:
      - service: notify.mobile_app
        data:
          message: "Power failure! UPS running on battery."

  - alias: "UPS Battery Low - Shutdown"
    trigger:
      - platform: state
        entity_id: text_sensor.cyberpower_ups_monitor_ups_status
        to: "Battery Low"
    action:
      - service: shell_command.shutdown_proxmox
```

## Web UI

Accessible at `http://<device-ip>/` — shows live status of all UPS values and allows configuration of thresholds.

## Architecture

```
CyberPower UPS
    │ USB HID (Power Device Class)
    ▼
ESP32-S3 (USB Host)
    │ GET_REPORT Control Transfers (every 5s)
    ▼
HID Report Parser → State Machine
    │
    ├──→ ESPHome API → Home Assistant (Sensors + Events)
    └──→ Web UI (Port 80, Live Status)
```

## License

MIT License
