#ifndef MODBUS_TYPES_H_
#define MODBUS_TYPES_H_

#include <array>
#include <cstdint>
#include <vector>

#include "esphome/core/log.h"

namespace modbus_saj {

static const char* const kModbusTypesTag = "modbus_types";

class ModbusTCPRequest {
 protected:
  // Modbus TCP header fields
  uint8_t transaction_identifier[2];
  uint8_t protocol_identifier[2];
  uint8_t length[2];
  uint8_t unit_id;
  uint8_t function_code;
  uint8_t address[2];
  uint8_t number_registers[2];

 public:
  // Default constructor
  ModbusTCPRequest() = default;
  
  // Constructor that takes a byte array
  explicit ModbusTCPRequest(const std::vector<uint8_t>& request);

  // Getter methods for accessing parsed fields
  uint8_t getFunctionCode() const;
  uint8_t getUnitId() const;
  uint16_t getTransactionId() const;
  uint16_t getAddress() const;
  uint16_t getNumberOfRegisters() const;

  // Get raw byte arrays (by reference - no copying!)
  const uint8_t* getTransactionIdentifierBytes() const;
  const uint8_t* getProtocolIdentifierBytes() const;
  const uint8_t* getLengthBytes() const;
  const uint8_t* getAddressBytes() const;
  const uint8_t* getNumberRegistersBytes() const;

  // Debug method using ESPHome logging
  void printDebugInfo() const;
};
}  // namespace modbus_saj

#endif // MODBUS_TYPES_H_