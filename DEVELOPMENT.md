# ESP CyberPower UPS Monitor — Project Plan

## Goal

ESP32-S3 reads a CyberPower BR1200ELCD UPS directly via USB HID and exposes
all data as native ESPHome sensors in Home Assistant — no NUT, no pwrstat,
no SSH chains. The UPS connects via USB to the ESP32 instead of a server.

**Before:**
```
UPS → USB → Server (pwrstat) → SSH → Proxmox → mosquitto_pub → MQTT → HA
```

**After:**
```
UPS → USB → ESP32-S3 → ESPHome API → Home Assistant
```

## Hardware

| Component | Details |
|-----------|---------|
| UPS | CyberPower BR1200ELCD, USB VID `0x0764`, PID `0x0501` |
| MCU | ESP32-S3-DevKitC-1 (USB OTG on GPIO19/20) |
| Connection | USB-A cable from UPS directly to ESP32-S3 OTG Port |
| Power Supply | ESP32 via separate USB power supply (NOT via the UPS USB!) |

**Important:** The ESP32-S3 OTG port does NOT provide 5V VBUS. The CyberPower UPS
powers its USB port itself, so no powered hub is needed.

## USB HID Power Device Protocol

CyberPower UPS devices use the USB HID Power Device Class standard:

- **Usage Page 0x84** (Power Device): Voltages, currents, load
- **Usage Page 0x85** (Battery System): Capacity, runtime, charge/discharge status

Data is read via **HID Feature Reports** (GET_REPORT Control Transfer).
Some status flags come as **Input Reports** (Interrupt IN).

### Relevant HID Usages

| Usage Page | Usage ID | Meaning | Unit |
|-----------|----------|---------|------|
| 0x84 | 0x0030 | Input Voltage (Utility Voltage) | V |
| 0x84 | 0x0030 | Output Voltage | V |
| 0x84 | 0x0035 | Percent Load | % |
| 0x84 | 0x0040 | Config Voltage (Rating Voltage) | V |
| 0x84 | 0x0043 | Config Apparent Power (Rating Power) | VA |
| 0x85 | 0x0066 | Remaining Capacity (Battery Capacity) | % |
| 0x85 | 0x0068 | Runtime To Empty (Remaining Runtime) | s |
| 0x85 | 0x0044 | Charging | bool |
| 0x85 | 0x0045 | Discharging (on battery) | bool |
| 0x85 | 0x00D0 | AC Present | bool |
| 0x85 | 0x0042 | Below Remaining Capacity Limit | bool |
| 0x85 | 0x00D3 | Shutdown Imminent | bool |
| 0x85 | 0x0065 | Overload | bool |
| 0x85 | 0x006B | Need Replacement | bool |

### Communication Flow

1. ESP32 enumerates USB device, reads Device Descriptor (verify VID/PID)
2. Read and parse HID Report Descriptor → build field map
3. Cyclically (every 5s) read Feature Reports via `GET_REPORT` Control Transfer
4. Extract values, publish to ESPHome sensors
5. Detect status changes (power failure etc.) immediately and fire events

## ESPHome Entities

### Sensors (sensor)

| Entity ID | Name | Unit | device_class | Source |
|-----------|------|------|-------------|--------|
| `utility_voltage` | Utility Voltage | V | voltage | HID Input/Voltage |
| `output_voltage` | Output Voltage | V | voltage | HID Output/Voltage |
| `battery_capacity` | Battery Capacity | % | battery | HID RemainingCapacity |
| `remaining_runtime` | Remaining Runtime | min | duration | HID RuntimeToEmpty |
| `load_watt` | Load | W | power | Calculated from % x Rating Power |
| `load_percent` | Load Percent | % | power_factor | HID PercentLoad |
| `rating_voltage` | Rating Voltage | V | voltage | HID ConfigVoltage |
| `rating_power` | Rating Power | VA | apparent_power | HID ConfigApparentPower |

### Binary Sensors (binary_sensor)

| Entity ID | Name | device_class | Source |
|-----------|------|-------------|--------|
| `ac_present` | AC Present | power | HID ACPresent |
| `on_battery` | On Battery | — | HID Discharging |
| `charging` | Charging | battery_charging | HID Charging |
| `overload` | Overload | problem | HID Overload |
| `battery_low` | Battery Low | battery | HID BelowRemainingCap |
| `replace_battery` | Replace Battery | problem | HID NeedReplacement |
| `ups_connected` | UPS Connected | connectivity | USB Device Present |

### Text Sensors (text_sensor)

| Entity ID | Name | Source |
|-----------|------|--------|
| `ups_status` | UPS Status | Summary: "Normal", "On Battery", "Battery Low", etc. |
| `ups_model` | Model | USB iProduct String |
| `last_power_event` | Last Power Event | Tracked internally |

## Power Failure Logic (State Machine)

Replicates the pwrstat daemon configuration as ESPHome events that can be
used in HA automations.

### States

```
┌─────────────┐
│   NORMAL     │  AC Present, not discharging
└──────┬──────┘
       │ AC Lost (Discharging = true)
       ▼
┌─────────────┐
│ POWER_FAIL   │  Timer: 60s delay
│ (grace)      │  → after 60s: fire "power_failure" event
└──────┬──────┘
       │ Runtime < 300s OR Capacity < 35%
       ▼
┌─────────────┐
│ BATTERY_LOW  │  Fire "battery_low" event
│              │  → HA Automation: Shutdown VMs, then host
└──────┬──────┘
       │ Shutdown Imminent (UPS reports)
       ▼
┌─────────────┐
│ SHUTDOWN     │  Fire "shutdown_imminent" event
│ IMMINENT     │  Final warning
└─────────────┘

Each state → NORMAL when AC returns
```

### Configurable Parameters (via Web UI)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `power_fail_delay` | 60 s | Delay before power failure event is fired |
| `battery_low_runtime` | 300 s | Runtime threshold for battery low |
| `battery_low_capacity` | 35 % | Capacity threshold for battery low |

### ESPHome Events for HA Automations

```yaml
# In HA Automation:
trigger:
  - platform: event
    event_type: esphome.cyberpower_ups
    event_data:
      type: power_failure
      
action:
  - service: notify.mobile_app
    data:
      message: "Power failure! UPS running on battery for 60s"
      
---
trigger:
  - platform: event
    event_type: esphome.cyberpower_ups
    event_data:
      type: battery_low
      
action:
  - service: shell_command.shutdown_proxmox
    data: {}
```

## Architecture / Code Structure

```
esp-cyberpower-ups/
├── components/
│   └── cyberpower_ups/
│       ├── __init__.py              ESPHome component definition
│       ├── cyberpower_ups.h         Main component (USB host, sensors, events)
│       ├── hid_ups_protocol.h       HID Report Descriptor parser
│       └── web_ui.h                 Status web page & configuration
├── esphome/
│   └── cyberpower-ups.yaml          Example ESPHome config
├── DEVELOPMENT.md                   This file
├── README.md                        User documentation
├── LICENSE
└── .gitignore
```

### FreeRTOS Tasks

| Task | Core | Prio | Purpose |
|------|------|------|---------|
| `usb_lib` | 0 | 10 | USB Host Library Event Loop |
| `usb_mon` | 1 | 5 | Device Connect/Disconnect, Enumeration |
| `ups_poll` | 1 | 6 | Cyclic reading of HID Reports (every 5s) |

### Data Flow

```
USB HID Reports
    ↓ GET_REPORT (Control Transfer, every 5s)
HID Report Parser (hid_ups_protocol.h)
    ↓ Extracted values
UPS State Machine (cyberpower_ups.h)
    ↓ Sensor Updates + Events
ESPHome API → Home Assistant
    ↓
HA Automations (Notifications, Shutdown scripts)
```

## Open Questions / Risks

1. **VID/PID Verification**: The PID `0x0501` must be confirmed on real hardware.
   → Can be checked with `lsusb` on a Linux machine when the UPS is connected there.

2. **VBUS**: The CyberPower UPS should power its USB port itself.
   If not, a powered hub is needed (as in the USB gateway project).

3. **HID Report Format**: CyberPower largely follows the standard, but some
   manufacturers have quirks. First test will show whether the parser finds all values.

4. **ESP32-S3 OTG + HID**: ESP-IDF has no ready-made USB HID Host driver as a
   high-level component. We use the low-level USB Host Library directly and
   implement HID GET_REPORT via Control Transfers. This is more robust than
   the experimental `usb_host_hid` driver.

## Next Steps

1. [x] Set up project structure
2. [x] Implement HID Report Descriptor parser
3. [ ] Main component: USB Host + HID Communication
4. [ ] Define and publish ESPHome sensor entities
5. [ ] State machine for power failure logic
6. [ ] Web UI for live status
7. [ ] Example ESPHome YAML config
8. [ ] Write README
9. [ ] Create and push Git repo
10. [ ] Real hardware test with BR1200ELCD
