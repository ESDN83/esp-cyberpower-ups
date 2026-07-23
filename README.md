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
| Battery Voltage | V | Battery pack voltage (diagnostic) |
| Battery Capacity | % | Current battery capacity |
| Remaining Runtime | min | Estimated remaining runtime on battery |
| Load | W | Current load in watts |
| Load Percent | % | Load percentage |
| Rating Voltage | V | Nominal **mains** voltage, e.g. 230 (diagnostic) |
| Rating Power | VA | Nameplate apparent power (diagnostic) |
| Rating Power Active | W | Nameplate active power, if reported (diagnostic) |

> **Sanity check after first flash.** Utility Voltage should sit near your
> mains (~230 V in Europe) and drift by a volt or two over the day.
> Battery Voltage should be a low double-digit number (~13 V, ~27 V, ~41 V
> depending on pack size). If Utility Voltage is pinned to an unchanging
> value and Rating Voltage reads something like 24 or 48, the component
> picked the battery fields for mains — see
> [Reporting a problem](#reporting-a-problem).

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

## Reporting a problem

Every UPS model ships its own HID report descriptor, and this component
has only been verified against the models listed under [Hardware](#hardware).
If a reading looks wrong on your unit, the report descriptor is almost
always where the answer is — and you can capture it in under a minute.

### 1. Grab the report map

Open the device's web UI log page:

```
http://<device-ip>/log
```

On every USB connect the component dumps the parsed HID report
descriptor there. Copy the whole thing — it looks like this:

```
===== usage resolution (45 fields parsed) =====
  UtilityVoltage   0084/0030 coll=001A -> rpt=35/FEATURE off=0 len=16 exp=7 unit=00F0D121 eff=0  [3 on this usage]
      coll=0024 rpt= 32/FEATURE off=  8 len=16 log=0..65535 exp=6 unit=00F0D121
      coll=001A rpt= 35/FEATURE off=  0 len=16 log=0..350   exp=7 unit=00F0D121  <-- used
      coll=001C rpt= 35/FEATURE off= 16 len=16 log=0..300   exp=7 unit=00F0D121
  ...
===== full field table follows on the web UI /log page =====
  [17] 0084/0030 coll=0024 rpt=32/FEATURE off=  8 len=16 log=0..65535 exp=6 unit=00F0D121
  ...
```

> Use the web UI page, not the ESPHome log. The full table is 40+ lines
> and a burst that size overruns the ESPHome API log buffer — you will
> silently lose most of it. The web UI reads from an 8 KB ring buffer on
> the device and keeps everything.

### 2. Open an issue with

- **UPS model** exactly as printed on the label, plus the `UPS Model` sensor value
- **The report map** from step 1, complete
- **What is wrong**: the sensor, the value it shows, and the value you expect
- **A reference measurement** if you have one — a smart meter, a plug-in
  power meter, anything independent. "The UPS says 252 V, my energy meter
  on the same circuit says 231 V" is worth more than any amount of prose.
- Whether the wrong value **changes over time or is frozen**. A reading
  that never moves is the strongest hint that the wrong field is being
  read; check the history in Home Assistant, not just the current value.
- **The parsed field count, from two or three separate boots.** The line
  reads `usage resolution (N fields parsed)`. It must be the same number
  every time. If it varies, the descriptor is arriving incomplete —
  look for a `HID desc short read` warning in the same log. A truncated
  descriptor loses whole fields off the end, and the command reports
  (Test, DelayBeforeShutdown) sit right there, so `Cmd support:` will
  flip to `no` while the sensors still look fine.

### Reading the dump yourself

Most wrong readings come down to one thing: **a usage that occurs in
more than one collection**. `Voltage` (usage `0030`) is declared
identically under Input, Output and PowerSummary — the number alone does
not say whether it is mains, output or battery voltage. Only the
enclosing collection does.

| `coll=` | Collection | Meaning of a Voltage in it |
|---------|------------|----------------------------|
| `001A` | Input | Mains voltage |
| `001C` | Output | UPS output voltage |
| `0024` | PowerSummary | **Battery** voltage |
| `0010` | BatterySystem | Battery subsystem |

The `[N on this usage]` counter tells you how many copies exist, and the
lines below it list every one with `<-- used` marking the chosen field.
If the marker sits on the wrong collection, that is your bug.

`eff=` is the exponent actually applied after resolving the unit's
built-in scale — `eff=0` means the raw value is used as-is, `eff=-1`
means it is divided by ten. A wrong `eff` shows up as a reading off by a
clean factor of ten. If `unit=00000000` on a value that should have a
unit, the descriptor did not declare one and no scaling is applied at
all; include that in the report.

<details>
<summary>Worked example: BR1200ELCD reporting 252 V mains</summary>

Utility Voltage sat at exactly 252 V for 23 hours while an energy meter
on the same house read 228–233 V and varied normally. The dump showed
three `0084/0030` fields, and the one being used was `coll=0024` —
PowerSummary, i.e. the battery. With `exp=6` against the HID voltage
unit's built-in exponent of 7, the effective exponent is −1, so the raw
252 meant **25.2 V**: a float-charged 24 V pack, which is exactly why it
never moved. The fix was to look the usage up within `coll=001A` (Input)
instead of taking whichever copy the descriptor declared first.

</details>

## License

MIT License

---

## 🔗 More Projects by ESDN83

| Project | Description |
|---------|-------------|
| [HA_enoceanmqtt-addon-ui](https://github.com/ESDN83/HA_enoceanmqtt-addon-ui) | EnOcean MQTT Home Assistant add-on with Web UI — visual device wizard, 96+ EEP profiles, MQTT auto-discovery, Eltako actuator control |
| [esp-ha-usb-gateway](https://github.com/ESDN83/esp-ha-usb-gateway) | ESP32-S3 USB-to-TCP bridge for Home Assistant — use Zigbee/EnOcean USB sticks over the network (ESPHome) |
| [heizung-vitoconnect](https://github.com/ESDN83/heizung-vitoconnect) | ESPHome Vitoconnect replacement for Viessmann heating (WT32-ETH01 + Optolink) with Home Assistant integration |
| [Home-Solar-Portable-emergency-charger](https://github.com/ESDN83/Home-Solar-Portable-emergency-charger) | DIY portable emergency charger for PV home battery systems (E3DC / Victron compatible) |
| [HA-Blueprints](https://github.com/ESDN83/HA-Blueprints) | Home Assistant automation blueprints — EnOcean PTM 215Z dimming & color scenes via Zigbee2MQTT |
