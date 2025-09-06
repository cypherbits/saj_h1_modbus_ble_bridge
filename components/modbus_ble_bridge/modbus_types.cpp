#include "modbus_types.h"

namespace modbus_saj {

// Constructor implementation
ModbusTCPRequest::ModbusTCPRequest(const std::vector<uint8_t>& request) {
  // Parse header fields
  transaction_identifier[0] = request[0];
  transaction_identifier[1] = request[1];
  protocol_identifier[0] = request[2];
  protocol_identifier[1] = request[3];
  length[0] = request[4];
  length[1] = request[5];
  unit_id = request[6];
  function_code = request[7];
  address[0] = request[8];
  address[1] = request[9];
  number_registers[0] = request[10];
  number_registers[1] = request[11];
}

// Getter method implementations
uint8_t ModbusTCPRequest::getFunctionCode() const {
  return function_code;
}

uint8_t ModbusTCPRequest::getUnitId() const {
  return unit_id;
}

uint16_t ModbusTCPRequest::getTransactionId() const {
  return (transaction_identifier[0] << 8) + transaction_identifier[1] - 1;
}

uint16_t ModbusTCPRequest::getAddress() const {
  return (address[0] << 8) | address[1];
}

uint16_t ModbusTCPRequest::getNumberOfRegisters() const {
  return (number_registers[0] << 8) + (2 * number_registers[1]) + 3;
}

const uint8_t* ModbusTCPRequest::getTransactionIdentifierBytes() const {
  return transaction_identifier;
}

const uint8_t* ModbusTCPRequest::getProtocolIdentifierBytes() const {
  return protocol_identifier;
}

const uint8_t* ModbusTCPRequest::getLengthBytes() const {
  return length;
}

const uint8_t* ModbusTCPRequest::getAddressBytes() const {
  return address;
}

const uint8_t* ModbusTCPRequest::getNumberRegistersBytes() const {
  return number_registers;
}

void ModbusTCPRequest::printDebugInfo() const {
  ESP_LOGD(kModbusTypesTag, "Modbus TCP Frame Debug Info:");
  ESP_LOGD(kModbusTypesTag, "  Transaction ID: 0x%02X%02X (%d)", 
           transaction_identifier[0], transaction_identifier[1], 
           getTransactionId());
  ESP_LOGD(kModbusTypesTag, "  Protocol ID: 0x%02X%02X", 
           protocol_identifier[0], protocol_identifier[1]);
  ESP_LOGD(kModbusTypesTag, "  Length: 0x%02X%02X (%d bytes)", 
           length[0], length[1], 
           (length[0] << 8) | length[1]);
  ESP_LOGD(kModbusTypesTag, "  Unit ID: 0x%02X (%d)", 
           unit_id, unit_id);
  ESP_LOGD(kModbusTypesTag, "  Function Code: 0x%02X (%d)", 
           function_code, function_code);
  ESP_LOGD(kModbusTypesTag, "  Address: 0x%02X%02X (%d)", 
           address[0], address[1], getAddress());
  ESP_LOGD(kModbusTypesTag, "  Number of Registers: 0x%02X%02X (%d)", 
           number_registers[0], number_registers[1], 
           getNumberOfRegisters());
}

}  // namespace modbus_saj