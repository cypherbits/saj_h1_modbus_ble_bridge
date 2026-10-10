# SAJ H1-*-S2 Modbus BLE Bridge

ESPHome external component that exposes the SAJ H1-*-S2 inverter Modbus registers over TCP by tunnelling requests through the official Bluetooth dongle. Acts as a Modbus/TCP server backed by BLE (Modbus/RTU frames encapsulated over a proprietary BLE service). Based on: https://github.com/sgsancho/saj_h1_s2_modbus_esp32

Designed for inverters that only provide local Bluetooth access (no LAN API) so you can query them from Home Assistant or any Modbus/TCP client (Node-RED, Python, etc.).

## Status / Compatibility
Compatible with ESPHome 2025.9.x (CI builds against 2025.9.3).
Requires the ESP32 **ESP-IDF** framework (Arduino is not supported). The network server is implemented directly on lwIP sockets.  
Not validated together with other active BLE clients; feedback welcome.

## Repository Structure
```
components/
  modbus_ble_bridge/       Component folder (required by ESPHome external_components)
    __init__.py            Python registration & schema
    modbus_ble_bridge.h/.cpp C++ implementation
tests/
  test.yaml                Device config used by CI to validate & compile the component
.github/workflows/ci.yml   CI pipeline
README.md
```

ESPHome expects each external component to live in a folder named after the component (`modbus_ble_bridge`) inside a `components` source directory. The previous flat layout (files in repo root) would not be auto-detected — this repo is already reorganized accordingly.

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
  # Optional metric sensors (omit all of them for zero overhead):
  total_requests:
    name: "Bridge Requests"
  timeouts:
    name: "Bridge Timeouts"
```

## 4. Get the data on NodeRed

Import the `flows_h1s2_full_v10_mqtt.json` file on your node-red.

## 5. Querying
Once flashed and connected, the device opens a Modbus/TCP server on the configured port (default 502). Point any Modbus tool at the ESP32 IP. Example (Python `pymodbus` or Node-RED Modbus node).

## Metrics (optional)
The component can expose a few diagnostic sensors. They are completely optional:
if none is configured, the metric code is excluded at compile time, so the bridge
keeps no metric state and performs no extra work.

| Key              | Description                                  |
| ---------------- | -------------------------------------------- |
| `total_requests` | Modbus/TCP requests forwarded to the dongle. |
| `timeouts`       | BLE response timeouts encountered.           |

Each key accepts a standard ESPHome [sensor](https://esphome.io/components/sensor/)
config (at least `name`). Example:

```yaml
modbus_ble_bridge:
  ble_client_id: saj_ble
  timeouts:
    name: "Bridge Timeouts"
```

Notes:
* The counters (`total_requests`, `timeouts`) are published only when they
  change, so they add no periodic work.
* `timeouts` counts BLE response timeouts (the bridge waiting for a notify that
  never arrived). Frame length errors are reset on success and are not counted.
* Metrics use ESPHome's standard `sensor` component, which is auto-loaded as a
dependency of this component.

## Continuous Integration
Every pull request runs the [CI workflow](.github/workflows/ci.yml), which builds the component against each supported ESPHome version (currently `2025.9.3`) using the device config in `tests/test.yaml`:

1. `esphome config tests/test.yaml` — validates the YAML schema and component registration.
2. `esphome compile tests/test.yaml` — compiles the firmware for ESP32 (ESP-IDF).

The workflow posts a build report comment on the pull request. To make the pipeline mandatory for merging, enable branch protection on `main` and mark the **Build** and **Report** checks as *required* (Settings → Branches → Branch protection rules).

To reproduce locally:

```bash
pip install "esphome==2025.9.3"
esphome config tests/test.yaml
esphome compile tests/test.yaml
```

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
