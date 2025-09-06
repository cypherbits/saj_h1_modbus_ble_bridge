# SAJ H1-*-S2 Modbus BLE Bridge

ESPHome external component that exposes the SAJ H1-*-S2 inverter Modbus registers over TCP by tunnelling requests through the official Bluetooth dongle. Acts as a Modbus/TCP server backed by BLE (Modbus/RTU frames encapsulated over a proprietary BLE service). Based on: https://github.com/sgsancho/saj_h1_s2_modbus_esp32

Designed for inverters that only provide local Bluetooth access (no LAN API) so you can query them from Home Assistant or any Modbus/TCP client (Node-RED, Python, etc.).

## Status / Compatibility
Tested with ESPHome 2025.6.3, 2025.7.5, 2025.8.3
Tested on both Arduino and ESP-IDF variants (ESP32).  
Not validated together with other active BLE clients; feedback welcome.

## Repository Structure
```
modbus_ble_bridge/         Component folder (required by ESPHome external_components)
  __init__.py              Python registration & schema
  modbus_ble_bridge.h/.cpp C++ implementation
README.md
```

ESPHome expects each external component to live in its own top‑level folder named after the component (`modbus_ble_bridge`). The previous flat layout (files in repo root) would not be auto-detected — this repo is already reorganized accordingly.

## 1. Add the external component
```yaml
external_components:
  - source:
      type: git
      url: https://github.com/cypherbits/saj_h1_modbus_ble_bridge
      ref: main
    components: [modbus_ble_bridge]
    refresh: 1d   # optional
```

## 2. Configure BLE (scan + client)
```yaml
esp32_ble_tracker:
  scan_parameters:
    interval: 3500ms
    window: 1100ms
    active: true

ble_client:
  - mac_address: !secret saj_dongle_mac   # AA:BB:CC:DD:EE:FF
    id: saj_ble
```

## 3. Add the bridge component
```yaml
modbus_ble_bridge:
  ble_client_id: saj_ble
  modbus_port: 502   # Optional (default 502)
```

## 4. Get the data on NodeRed

Import the `flows_h1s2_full_v10_mqtt.json` file on your node-red.

## 5. Querying
Once flashed and connected, the device opens a Modbus/TCP server on the configured port (default 502). Point any Modbus tool at the ESP32 IP. Example (Python `pymodbus` or Node-RED Modbus node).

## Thanks

Big thanks to https://t.me/saj_nooficialoriginal Telegram group.

## Notes / Limitations
* Designed for simple request/response usage; one TCP request at a time.
* The BLE response timeout is currently fixed in code (2s). If needed this can be exposed later via YAML.
* Writes (FC 6) close the TCP connection immediately after the response (current implementation detail).

## Troubleshooting
* If you see "BLE write characteristic not found": ensure you paired the correct dongle MAC.
* If Wi-Fi is not up, Modbus server waits (lazy init).
* Repeated length errors (>5) force the TCP client to close; retry from the client side.

## Roadmap / TODO
* Expose BLE response timeout as a config option.
* Provide Node-RED example flow.
* Optional metrics sensors (uptime, error counters) via ESPHome sensors.
