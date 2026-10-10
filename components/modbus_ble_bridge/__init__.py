import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.ble_client import CONF_BLE_CLIENT_ID, BLEClient
import esphome.components.ble_client as ble_client
from esphome.config_validation import positive_int

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["network"]

modbus_ble_bridge_ns = cg.esphome_ns.namespace("modbus_ble_bridge")
ModbusBleBridge = modbus_ble_bridge_ns.class_(
    "ModbusBleBridge", cg.Component, ble_client.BLEClientNode
)

CONF_MODBUS_PORT = "modbus_port"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ModbusBleBridge),
        cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(BLEClient),
        cv.Optional(CONF_MODBUS_PORT, default=502): positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield ble_client.register_ble_node(var, config)
    cg.add(var.set_modbus_port(config[CONF_MODBUS_PORT]))
