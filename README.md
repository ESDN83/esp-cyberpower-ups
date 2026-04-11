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
- 
<img width="1510" height="1525" alt="grafik" src="https://github.com/user-attachments/assets/cacf1d3a-af55-4aa8-9f09-44d434832d96" />

<img width="1549" height="1474" alt="grafik" src="https://github.com/user-attachments/assets/9595da39-6d2c-44f6-a424-a16bfc9305e2" />


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

### HA Automation Examples

See `esphome/ha-automations-example.yaml` for a complete set of automations including:

- **Power Failure Alert** — persistent notification when AC is lost
- **Battery Low Alert** — notification when battery thresholds are breached
- **AC Restored** — clears alerts when power returns
- **Proxmox Shutdown Sequence** — graceful shutdown: VMs → Containers → Host → HA (last!)
- **Shutdown Imminent** — emergency HA shutdown on UPS hardware signal

Quick example:

```yaml
automation:
  - alias: "UPS: Power Failure Alert"
    trigger:
      - platform: state
        entity_id: text_sensor.cyberpower_ups_monitor_ups_status
        to: "Power Failure"
    action:
      - service: persistent_notification.create
        data:
          title: "UPS Power Failure"
          message: "Power failure! Battery: {{ states('sensor.cyberpower_ups_monitor_battery_capacity') }}%"
          notification_id: ups_power_failure
```

### Migrating from pwrstat / MQTT

If you previously used the `pwrstat` daemon with MQTT (e.g. `sensor.usv_proxmox*` entities):

1. Set up this ESPHome device and verify all sensors appear in HA
2. Update your automations to use the new entity IDs (`text_sensor.cyberpower_ups_monitor_*`, `sensor.cyberpower_ups_monitor_*`)
3. Remove the old MQTT entities: **Settings → Devices → search "usv_proxmox" → Delete**
4. Remove the pwrstat cron job / systemd service on the old server
5. Disconnect the USB cable from the server and connect it to the ESP32-S3

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
