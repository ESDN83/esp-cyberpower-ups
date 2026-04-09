# ESP CyberPower UPS Monitor

ESP32-S3 liest eine CyberPower USV direkt über USB HID aus und stellt alle Daten als native ESPHome-Sensoren in Home Assistant bereit.

**Kein NUT, kein pwrstat, kein SSH — direkt USB → ESP32 → Home Assistant.**

## Features

- Direkte USB-HID-Kommunikation mit CyberPower USV (BR1200ELCD und kompatible)
- Native ESPHome-Sensoren (Spannung, Batterie, Last, Laufzeit, Status)
- Stromausfall-Erkennung mit konfigurierbaren Schwellwerten
- HA Events für Automationen (Power Failure, Battery Low, Shutdown Imminent)
- Web UI für Live-Status und Konfiguration
- Kein separater Server oder Daemon nötig

## Hardware

| Komponente | Details |
|-----------|---------|
| **USV** | CyberPower BR1200ELCD (oder kompatibel) |
| **MCU** | ESP32-S3-DevKitC-1 |
| **Verbindung** | USB-A Kabel von USV direkt an ESP32-S3 OTG Port |

> **Wichtig:** Logger auf `UART0` setzen! Der Default `USB_SERIAL_JTAG` blockiert die GPIO19/20 Pins die für USB Host gebraucht werden.

## Installation

1. **ESPHome YAML** erstellen (siehe `esphome/cyberpower-ups.yaml`)
2. **Flashen** über ESPHome Dashboard oder CLI
3. **USB-Kabel** von USV an ESP32-S3 OTG Port anschließen
4. **Home Assistant** erkennt das Gerät automatisch über ESPHome API

### Minimale YAML Config

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

Siehe `esphome/cyberpower-ups.yaml` für die vollständige Konfiguration mit allen Sensoren.

## Sensoren in Home Assistant

### Messwerte
| Sensor | Einheit | Beschreibung |
|--------|---------|-------------|
| Netzspannung | V | Eingangsspannung vom Netz |
| Ausgangsspannung | V | Ausgangsspannung der USV |
| Batterieladung | % | Aktuelle Batteriekapazität |
| Restlaufzeit | min | Geschätzte Restlaufzeit im Batteriebetrieb |
| Last | W | Aktuelle Last in Watt |
| Auslastung | % | Auslastung in Prozent |

### Status
| Sensor | Typ | Beschreibung |
|--------|-----|-------------|
| USV verbunden | Binary | USB-Verbindung zur USV |
| Netz vorhanden | Binary | Netzspannung vorhanden |
| Batteriebetrieb | Binary | USV läuft auf Batterie |
| Lädt | Binary | Batterie wird geladen |
| Überlast | Binary | USV ist überlastet |
| USV Status | Text | Normal / Stromausfall / Batterie niedrig / Shutdown |

## Stromausfall-Logik

Die Komponente implementiert eine State Machine die die pwrstat-Daemon-Logik repliziert:

```
NORMAL → POWER_FAIL (nach AC-Verlust, 60s Verzögerung) → BATTERY_LOW → SHUTDOWN_IMMINENT
```

### Schwellwerte (konfigurierbar über Web UI)

| Parameter | Default | Beschreibung |
|-----------|---------|-------------|
| Power-Failure Verzögerung | 60 s | Zeit bevor Power-Failure-Event gefeuert wird |
| Battery-Low Laufzeit | 300 s | Runtime-Schwelle |
| Battery-Low Kapazität | 35 % | Kapazitäts-Schwelle |

### HA Automation Beispiel

```yaml
# Benachrichtigung bei Stromausfall
automation:
  - alias: "USV Stromausfall"
    trigger:
      - platform: state
        entity_id: text_sensor.cyberpower_ups_monitor_usv_status
        to: "Stromausfall"
    action:
      - service: notify.mobile_app
        data:
          message: "Stromausfall! USV läuft auf Batterie."

  - alias: "USV Batterie niedrig - Shutdown"
    trigger:
      - platform: state
        entity_id: text_sensor.cyberpower_ups_monitor_usv_status
        to: "Batterie niedrig"
    action:
      - service: shell_command.shutdown_proxmox
```

## Web UI

Erreichbar unter `http://<device-ip>/` — zeigt Live-Status aller USV-Werte und ermöglicht Konfiguration der Schwellwerte.

## Architektur

```
CyberPower USV
    │ USB HID (Power Device Class)
    ▼
ESP32-S3 (USB Host)
    │ GET_REPORT Control Transfers (alle 5s)
    ▼
HID Report Parser → State Machine
    │
    ├──→ ESPHome API → Home Assistant (Sensoren + Events)
    └──→ Web UI (Port 80, Live-Status)
```

## Lizenz

MIT License
