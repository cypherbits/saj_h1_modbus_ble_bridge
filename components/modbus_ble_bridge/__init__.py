import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, STATE_CLASS_TOTAL_INCREASING
from esphome.components import sensor
from esphome.components.ble_client import CONF_BLE_CLIENT_ID, BLEClient
import esphome.components.ble_client as ble_client
from esphome.config_validation import positive_int
from esphome.core import CORE

DEPENDENCIES = ["ble_client"]

modbus_ble_bridge_ns = cg.esphome_ns.namespace("modbus_ble_bridge")
ModbusBleBridge = modbus_ble_bridge_ns.class_(
    "ModbusBleBridge", cg.Component, ble_client.BLEClientNode
)

CONF_MODBUS_PORT = "modbus_port"
CONF_TOTAL_REQUESTS = "total_requests"
CONF_TIMEOUTS = "timeouts"


def AUTO_LOAD():
    # ESPHome's "sensor" component is only needed when at least one metric
    # sensor is configured. Without metrics the bridge never instantiates a
    # sensor::Sensor, so skipping the auto-load means its sources are neither
    # copied nor compiled (and USE_SENSOR stays undefined). ESPHome resolves
    # AUTO_LOAD after substitutions, so the raw config is available here.
    # Only skip "sensor" when we positively know no metric is configured;
    # otherwise fall back to loading it, so metrics can never break.
    conf = (CORE.raw_config or {}).get("modbus_ble_bridge", {})
    if isinstance(conf, dict) and CONF_TOTAL_REQUESTS not in conf and CONF_TIMEOUTS not in conf:
        return ["network"]
    return ["network", "sensor"]

# When at least one metric is configured we define this flag, which compiles in
# the metric sensors and their publish calls. Devices that configure none pay no
# RAM or runtime cost, since the whole code path is excluded at compile time.
METRICS_DEFINE = "USE_MODBUS_BLE_BRIDGE_METRICS"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ModbusBleBridge),
        cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(BLEClient),
        cv.Optional(CONF_MODBUS_PORT, default=502): positive_int,
        cv.Optional(CONF_TOTAL_REQUESTS): sensor.sensor_schema(
            icon="mdi:counter",
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        cv.Optional(CONF_TIMEOUTS): sensor.sensor_schema(
            icon="mdi:alert-circle-outline",
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield ble_client.register_ble_node(var, config)
    cg.add(var.set_modbus_port(config[CONF_MODBUS_PORT]))

    metrics = False
    if CONF_TOTAL_REQUESTS in config:
        sens = yield sensor.new_sensor(config[CONF_TOTAL_REQUESTS])
        cg.add(var.set_total_requests_sensor(sens))
        metrics = True
    if CONF_TIMEOUTS in config:
        sens = yield sensor.new_sensor(config[CONF_TIMEOUTS])
        cg.add(var.set_timeouts_sensor(sens))
        metrics = True

    if metrics:
        cg.add_define(METRICS_DEFINE)
