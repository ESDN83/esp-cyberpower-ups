# ESP CyberPower UPS Monitor

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESPHome](https://img.shields.io/badge/ESPHome-Component-blue.svg)](https://esphome.io/)
[![Home Assistant](https://img.shields.io/badge/Home%20Assistant-Integration-41BDF5.svg)](https://www.home-assistant.io/)
[![ESP32-S3](https://img.shields.io/badge/ESP32--S3-USB%20HID-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![GitHub Release](https://img.shields.io/github/v/release/ESDN83/esp-cyberpower-ups)](https://github.com/ESDN83/esp-cyberpower-ups/releases)

ESP32-S3 reads a CyberPower UPS directly via USB HID and exposes all data as native ESPHome sensors in Home Assistant.

**No NUT, no pwrstat, no SSH — direct USB → ESP32 → Home Assistant.**

## Features

- Direct USB HID communication with CyberPower UPS (BR1200ELCD and compatible)
- Native ESPHome sensors (voltage, battery, load, runtime, status)
- Power failure detection with configurable thresholds
- HA events for automations (Power Failure, Battery Low, Shutdown Imminent)
- NUT-style command buttons (beeper, battery test, cancel shutdown) — see [Commands / Buttons](#commands--buttons)
- Web UI for live status and configuration
- No separate server or daemon required
  
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

## Commands / Buttons

Besides reading data, the component can send **NUT-style instant commands** back to
the UPS over USB HID (`SET_REPORT` on the matching FEATURE report). These appear as
**button entities** in Home Assistant.

Support depends on the UPS model. After boot the log prints a line such as
`Cmd support: beeper=YES test=YES load=no` (visible in the ESPHome log and on the
`http://<device-ip>/log` page) so you can see what **your** model accepts. A button
for an unsupported command is simply a no-op (logged as "NOT supported").

### Safe commands (enabled by default)

| Button | NUT command | Effect |
|--------|-------------|--------|
| Beeper Mute | `beeper.mute` | Silence the currently sounding alarm |
| Beeper Enable | `beeper.enable` | Re-enable the audible alarm |
| Beeper Disable | `beeper.disable` | Disable the audible alarm |
| Battery Test Start | `test.battery.start.quick` | Run a quick battery self-test |
| Battery Test Stop | `test.battery.stop` | Abort a running battery test |
| Cancel Shutdown | `shutdown.stop` | Cancel a pending shutdown/reboot |

These are reversible and do **not** affect the devices plugged into the UPS.

> **Battery test & shutdown automations:** a self-test runs the UPS on battery on
> purpose and may briefly report a low capacity. The firmware suppresses the
> power-failure / battery-low state machine for ~45 s after a test is started, so
> `Battery Test Start` will **not** trigger your shutdown automations.

### Dangerous commands (disabled by default) ⚠️

The following commands switch the **UPS output** — pressing them cuts power to
everything plugged into the UPS (NAS, PC, network gear …), after a short delay,
**even while running on mains**:

| Button | NUT command | Effect |
|--------|-------------|--------|
| UPS Reboot Load | `shutdown.reboot` | Turn the load off, then back on |
| UPS Load Off | `load.off.delay` | Turn the load off and keep it off |

They are shipped **commented out** in `esphome/cyberpower-ups.yaml`. Enable them only
if you understand the consequences, by removing the leading `# ` from the block marked
`DANGEROUS COMMANDS`.

> **No confirmation on the entity.** An ESPHome/HA button fires the instant it is
> pressed or called from an automation — there is no built-in "are you sure?" prompt.
> For the dangerous buttons, add a confirmation dialog on the **Home Assistant
> dashboard card** instead:
>
> ```yaml
> type: button
> entity: button.cyberpower_ups_monitor_ups_load_off
> tap_action:
>   action: toggle
>   confirmation:
>     text: "Really cut power to all devices on the UPS?"
> ```

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
    │ GET_REPORT (read, every 5s)  ▲ SET_REPORT (commands)
    ▼                              │
HID Report Parser → State Machine ─┴─ Command Queue
    │
    ├──→ ESPHome API → Home Assistant (Sensors + Events + Buttons)
    └──→ Web UI (Port 80, Live Status)
```

## License

MIT License
