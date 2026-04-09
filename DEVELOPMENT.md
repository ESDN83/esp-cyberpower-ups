# ESP CyberPower UPS Monitor — Projektplanung

## Ziel

ESP32-S3 liest eine CyberPower BR1200ELCD USV direkt über USB HID aus und stellt
alle Daten als native ESPHome-Sensoren in Home Assistant bereit — ohne NUT, ohne
pwrstat, ohne SSH-Ketten. Die USV hängt per USB am ESP32 statt an einem Server.

**Vorher:**
```
USV → USB → Server (pwrstat) → SSH → Proxmox → mosquitto_pub → MQTT → HA
```

**Nachher:**
```
USV → USB → ESP32-S3 → ESPHome API → Home Assistant
```

## Hardware

| Komponente | Details |
|-----------|---------|
| USV | CyberPower BR1200ELCD, USB VID `0x0764`, PID `0x0501` |
| MCU | ESP32-S3-DevKitC-1 (USB OTG auf GPIO19/20) |
| Verbindung | USB-A Kabel von USV direkt an ESP32-S3 OTG Port |
| Stromversorgung | ESP32 über separates USB-Netzteil (NICHT über die USV-USB!) |

**Wichtig:** Der ESP32-S3 OTG Port liefert KEIN 5V VBUS. Die CyberPower USV
versorgt ihren USB-Port selbst mit Strom, daher braucht man keinen powered Hub.

## USB HID Power Device Protocol

CyberPower USVen verwenden den USB HID Power Device Class Standard:

- **Usage Page 0x84** (Power Device): Spannungen, Ströme, Last
- **Usage Page 0x85** (Battery System): Kapazität, Laufzeit, Lade-/Entladestatus

Die Daten werden über **HID Feature Reports** gelesen (GET_REPORT Control Transfer).
Einige Status-Flags kommen als **Input Reports** (Interrupt IN).

### Relevante HID Usages

| Usage Page | Usage ID | Bedeutung | Einheit |
|-----------|----------|-----------|---------|
| 0x84 | 0x0030 | Input Voltage (Netzspannung) | V |
| 0x84 | 0x0030 | Output Voltage (Ausgangsspannung) | V |
| 0x84 | 0x0035 | Percent Load (Auslastung) | % |
| 0x84 | 0x0040 | Config Voltage (Nennspannung) | V |
| 0x84 | 0x0043 | Config Apparent Power (Nennleistung) | VA |
| 0x85 | 0x0066 | Remaining Capacity (Batterieladung) | % |
| 0x85 | 0x0068 | Runtime To Empty (Restlaufzeit) | s |
| 0x85 | 0x0044 | Charging (lädt) | bool |
| 0x85 | 0x0045 | Discharging (entlädt = Netzbetrieb aus) | bool |
| 0x85 | 0x00D0 | AC Present (Netz vorhanden) | bool |
| 0x85 | 0x0042 | Below Remaining Capacity Limit | bool |
| 0x85 | 0x00D3 | Shutdown Imminent | bool |
| 0x85 | 0x0065 | Overload | bool |
| 0x85 | 0x006B | Need Replacement | bool |

### Kommunikationsablauf

1. ESP32 enumeriert USB-Gerät, liest Device Descriptor (VID/PID prüfen)
2. HID Report Descriptor lesen und parsen → Feld-Map aufbauen
3. Zyklisch (alle 5s) Feature Reports lesen via `GET_REPORT` Control Transfer
4. Werte extrahieren, in ESPHome-Sensoren publizieren
5. Status-Änderungen (Stromausfall etc.) sofort erkennen und Events feuern

## ESPHome Entities

### Sensoren (sensor)

| Entity ID | Name | Einheit | device_class | Quelle |
|-----------|------|---------|-------------|--------|
| `utility_voltage` | Netzspannung | V | voltage | HID Input/Voltage |
| `output_voltage` | Ausgangsspannung | V | voltage | HID Output/Voltage |
| `battery_capacity` | Batterieladung | % | battery | HID RemainingCapacity |
| `remaining_runtime` | Restlaufzeit | min | duration | HID RuntimeToEmpty |
| `load_watt` | Last | W | power | Berechnet aus % × Nennleistung |
| `load_percent` | Auslastung | % | power_factor | HID PercentLoad |
| `rating_voltage` | Nennspannung | V | voltage | HID ConfigVoltage |
| `rating_power` | Nennleistung | VA | apparent_power | HID ConfigApparentPower |

### Binary Sensoren (binary_sensor)

| Entity ID | Name | device_class | Quelle |
|-----------|------|-------------|--------|
| `ac_present` | Netz vorhanden | power | HID ACPresent |
| `on_battery` | Batteriebetrieb | — | HID Discharging |
| `charging` | Lädt | battery_charging | HID Charging |
| `overload` | Überlast | problem | HID Overload |
| `battery_low` | Batterie niedrig | battery | HID BelowRemainingCap |
| `replace_battery` | Batterie tauschen | problem | HID NeedReplacement |
| `ups_connected` | USV verbunden | connectivity | USB Device Present |

### Text Sensoren (text_sensor)

| Entity ID | Name | Quelle |
|-----------|------|--------|
| `ups_status` | USV Status | Zusammenfassung: "Normal", "On Battery", "Battery Low", etc. |
| `ups_model` | Modell | USB iProduct String |
| `last_power_event` | Letztes Ereignis | Intern getrackt |

## Stromausfall-Logik (State Machine)

Repliziert die pwrstat-Daemon Konfiguration als ESPHome-Events, die in HA
Automationen genutzt werden können.

### States

```
┌─────────────┐
│   NORMAL     │  AC Present, nicht entladen
└──────┬──────┘
       │ AC Lost (Discharging = true)
       ▼
┌─────────────┐
│ POWER_FAIL   │  Timer: 60s Verzögerung
│ (grace)      │  → nach 60s: Event "power_failure" feuern
└──────┬──────┘
       │ Runtime < 300s ODER Capacity < 35%
       ▼
┌─────────────┐
│ BATTERY_LOW  │  Event "battery_low" feuern
│              │  → HA Automation: Shutdown VMs, dann Host
└──────┬──────┘
       │ Shutdown Imminent (USV meldet)
       ▼
┌─────────────┐
│ SHUTDOWN     │  Event "shutdown_imminent" feuern
│ IMMINENT     │  Letzte Warnung
└─────────────┘

Jeder State → NORMAL wenn AC zurückkehrt
```

### Konfigurierbare Parameter (über Web UI)

| Parameter | Default | Beschreibung |
|-----------|---------|-------------|
| `power_fail_delay` | 60 s | Verzögerung bevor Power-Failure-Event gefeuert wird |
| `battery_low_runtime` | 300 s | Runtime-Schwelle für Battery-Low |
| `battery_low_capacity` | 35 % | Kapazitäts-Schwelle für Battery-Low |

### ESPHome Events für HA Automationen

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
      message: "Stromausfall! USV läuft auf Batterie seit 60s"
      
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

## Architektur / Code-Struktur

```
esp-cyberpower-mqtt/
├── components/
│   └── cyberpower_ups/
│       ├── __init__.py              ESPHome Component-Definition
│       ├── cyberpower_ups.h         Hauptkomponente (USB Host, Sensoren, Events)
│       ├── hid_ups_protocol.h       HID Report Descriptor Parser
│       └── web_ui.h                 Status-Webseite & Konfiguration
├── esphome/
│   └── cyberpower-ups.yaml          Beispiel ESPHome Config
├── DEVELOPMENT.md                   Diese Datei
├── README.md                        Benutzer-Doku
├── LICENSE
└── .gitignore
```

### FreeRTOS Tasks

| Task | Core | Prio | Aufgabe |
|------|------|------|---------|
| `usb_lib` | 0 | 10 | USB Host Library Event Loop |
| `usb_mon` | 1 | 5 | Device Connect/Disconnect, Enumeration |
| `ups_poll` | 1 | 6 | Zyklisches Lesen der HID Reports (alle 5s) |

### Datenfluss

```
USB HID Reports
    ↓ GET_REPORT (Control Transfer, alle 5s)
HID Report Parser (hid_ups_protocol.h)
    ↓ Extrahierte Werte
UPS State Machine (cyberpower_ups.h)
    ↓ Sensor Updates + Events
ESPHome API → Home Assistant
    ↓
HA Automationen (Benachrichtigungen, Shutdown-Skripte)
```

## Offene Fragen / Risiken

1. **VID/PID Verifizierung**: Der PID `0x0501` muss am realen Gerät bestätigt werden.
   → Kann mit `lsusb` auf einem Linux-Rechner geprüft werden, wenn die USV dort angeschlossen ist.

2. **VBUS**: Die CyberPower USV sollte ihren USB-Port selbst mit Strom versorgen.
   Falls nicht, braucht man einen powered Hub (wie beim USB-Gateway Projekt).

3. **HID Report Format**: CyberPower folgt dem Standard weitgehend, aber manche
   Hersteller haben Quirks. Erster Test wird zeigen ob der Parser alle Werte findet.

4. **ESP32-S3 OTG + HID**: ESP-IDF hat keinen fertigen USB HID Host Treiber als
   High-Level-Komponente. Wir nutzen die Low-Level USB Host Library direkt und
   implementieren HID GET_REPORT über Control Transfers. Das ist robuster als
   der experimentelle `usb_host_hid` Treiber.

## Nächste Schritte

1. [x] Projektstruktur anlegen
2. [x] HID Report Descriptor Parser implementieren
3. [ ] Hauptkomponente: USB Host + HID Communication
4. [ ] ESPHome Sensor-Entities definieren und publishen
5. [ ] State Machine für Stromausfall-Logik
6. [ ] Web UI für Live-Status
7. [ ] ESPHome YAML Beispiel-Config
8. [ ] README schreiben
9. [ ] Git Repo erstellen und pushen
10. [ ] Realer Hardware-Test mit BR1200ELCD
