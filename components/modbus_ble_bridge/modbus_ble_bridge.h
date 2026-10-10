#pragma once
#include "esphome.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/network/util.h"
#ifdef USE_MODBUS_BLE_BRIDGE_METRICS
#include "esphome/components/sensor/sensor.h"
#endif
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>

namespace esphome {
namespace modbus_ble_bridge {

class ModbusBleBridge : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_modbus_port(uint16_t port) { modbus_port_ = port; }
  void set_ble_response_timeout(uint32_t ms) { ble_response_timeout_ms_ = ms; }

#ifdef USE_MODBUS_BLE_BRIDGE_METRICS
  void set_total_requests_sensor(sensor::Sensor *sensor) { total_requests_sensor_ = sensor; }
  void set_timeouts_sensor(sensor::Sensor *sensor) { timeouts_sensor_ = sensor; }
#endif

 protected:
  uint16_t modbus_port_ = 502;
  uint32_t ble_response_timeout_ms_ = 2000;

  int server_fd_ = -1;
  int client_fd_ = -1;
  std::vector<uint8_t> modbus_request_;
  std::vector<uint8_t> modbus_frame_response_;
  std::vector<uint8_t> tcp_response_;
  int total_registers_ = 0;
  int errlen_ = 0;
  int c_ = 0;
  int total_calls_ = 0;
  int total_timeouts_ = 0;
#ifdef USE_MODBUS_BLE_BRIDGE_METRICS
  sensor::Sensor *total_requests_sensor_{nullptr};
  sensor::Sensor *timeouts_sensor_{nullptr};
#endif
  unsigned long last_wifi_check_ = 0;
  bool waiting_message_ = false;
  unsigned long waiting_since_ = 0;
  unsigned long last_wait_log_ = 0;

  esp32_ble_client::BLECharacteristic *char_read_ = nullptr;
  esp32_ble_client::BLECharacteristic *char_write_ = nullptr;

  void handle_modbus_tcp();
  bool send_ble_request(uint8_t *data, size_t len);
  bool send_all_(const uint8_t *data, size_t len);
  void send_exception_(uint8_t fn_code, uint8_t exception_code);
  void close_client_();
  void note_request_();
  void note_timeout_();
  uint16_t modrtu_crc(const uint8_t *buf, int len);
};

}  // namespace modbus_ble_bridge
}  // namespace esphome
