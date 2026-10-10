#include "modbus_ble_bridge.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>

namespace esphome {
namespace modbus_ble_bridge {

static const char *const TAG = "modbus_ble_bridge";
static constexpr uint16_t BLE_SERVICE = 0xFFFF;
static constexpr uint16_t BLE_CHAR_READ = 0xFF02;
static constexpr uint16_t BLE_CHAR_WRITE = 0xFF01;

void ModbusBleBridge::setup() {
  ESP_LOGI(TAG, "Setting up Modbus BLE Bridge");
  this->modbus_request_.resize(260);
  this->modbus_frame_response_.resize(8);
  this->c_ = 0;
  this->total_calls_ = 0;
  this->total_errors_ = 0;
}

void ModbusBleBridge::close_client_() {
  if (this->client_fd_ >= 0) {
    ::close(this->client_fd_);
    this->client_fd_ = -1;
  }
}

void ModbusBleBridge::loop() {
  unsigned long now = millis();
  if (!esphome::network::is_connected()) {
    if (now - this->last_wifi_check_ >= 5000) {
      ESP_LOGW(TAG, "WiFi not connected");
      this->last_wifi_check_ = now;
    }
    return;
  }
  if (this->server_fd_ < 0) {
    this->server_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (this->server_fd_ < 0) {
      ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
      return;
    }
    int opt = 1;
    setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(this->modbus_port_);
    if (bind(this->server_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
      ::close(this->server_fd_);
      this->server_fd_ = -1;
      return;
    }
    if (listen(this->server_fd_, 1) != 0) {
      ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
      ::close(this->server_fd_);
      this->server_fd_ = -1;
      return;
    }
    int flags = fcntl(this->server_fd_, F_GETFL, 0);
    fcntl(this->server_fd_, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "Modbus TCP server started on port %d", this->modbus_port_);
  }

  if (this->waiting_message_ && this->waiting_since_ != 0 && (now - this->waiting_since_) > this->ble_response_timeout_ms_) {
    this->total_errors_++;
    ESP_LOGW(TAG, "BLE response timeout after %u ms (loop). Resetting state.", this->ble_response_timeout_ms_);
    this->waiting_message_ = false;
    this->waiting_since_ = 0;
    ESP_LOGI(TAG, "Closing TCP client due to BLE timeout (loop)");
    this->close_client_();
  }
  if (this->client_fd_ < 0) {
    sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
    int fd = ::accept(this->server_fd_, (struct sockaddr*)&caddr, &clen);
    if (fd < 0) return;
    int cflags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, cflags | O_NONBLOCK);
    this->client_fd_ = fd;
    ESP_LOGI(TAG, "Modbus TCP client connected");
  }
  this->handle_modbus_tcp();
}

void ModbusBleBridge::handle_modbus_tcp() {
  int avail = 0;
  if (this->client_fd_ >= 0) {
    uint8_t tmp[1];
    int r = ::recv(this->client_fd_, (char*)tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r > 0) {
      int bytes = 0;
      if (ioctl(this->client_fd_, FIONREAD, &bytes) == 0) avail = bytes; else avail = r;
    } else if (r == 0) {
      ESP_LOGI(TAG, "TCP client closed by peer");
      this->close_client_();
      return;
    } else {
      avail = 0;
    }
  }
  if (!avail) return;

  if (this->waiting_message_) {
    unsigned long now = millis();
    if (now - this->last_wait_log_ >= 1000) {
      ESP_LOGD(TAG, "Still waiting for BLE response. Ignoring new Modbus request.");
      this->last_wait_log_ = now;
    }
    if (this->waiting_since_ != 0 && (now - this->waiting_since_) > this->ble_response_timeout_ms_) {
      this->total_errors_++;
      ESP_LOGW(TAG, "BLE response timeout after %u ms. Resetting state.", this->ble_response_timeout_ms_);
      this->waiting_message_ = false;
      this->waiting_since_ = 0;
      ESP_LOGI(TAG, "Closing TCP client due to BLE timeout while waiting");
      this->close_client_();
    }
    return;
  }

  ESP_LOGD(TAG, "handle_modbus_tcp: available bytes=%d", avail);
  int i = 0;
  if (this->client_fd_ >= 0) {
    int r = ::recv(this->client_fd_, (char*)this->modbus_request_.data(), 260, MSG_DONTWAIT);
    if (r <= 0) return; else i = r;
  }
  ESP_LOGD(TAG, "Read %d bytes from Modbus TCP client", i);
  if (i < 12) {
    ESP_LOGW(TAG, "Incomplete Modbus TCP frame: %d bytes, aborting", i);
    return;
  }
  uint8_t fn_code = this->modbus_request_[7];
  uint8_t unit_id = this->modbus_request_[6];
  this->ble_response_buffer_.clear();
  this->expected_frame_len_ = 0;
  uint8_t addr_hi = this->modbus_request_[8];
  uint8_t addr_lo = this->modbus_request_[9];
  uint8_t reg_hi  = this->modbus_request_[10];
  uint8_t reg_lo  = this->modbus_request_[11];
  this->requested_regs_ = (uint16_t(reg_hi) << 8) | reg_lo;
  int num_regs = ((reg_hi << 8) | reg_lo);
  this->total_registers_ = num_regs * 2 + 3;
  std::vector<uint8_t> ble_req = {77, 0, static_cast<uint8_t>(c_), 9, 50, unit_id, fn_code, addr_hi, addr_lo, reg_hi, reg_lo};
  uint16_t crc = this->modrtu_crc(ble_req.data() + 5, 6);
  ble_req.push_back(crc & 0xFF);
  ble_req.push_back((crc >> 8) & 0xFF);
  int total_regs = this->total_registers_;
  this->modbus_frame_response_[0] = this->modbus_request_[0];
  this->modbus_frame_response_[1] = this->modbus_request_[1];
  this->modbus_frame_response_[2] = this->modbus_request_[2];
  this->modbus_frame_response_[3] = this->modbus_request_[3];
  this->modbus_frame_response_[4] = (total_regs & 0xFF00) >> 8;
  this->modbus_frame_response_[5] = total_regs & 0xFF;
  this->modbus_frame_response_[6] = unit_id;
  this->modbus_frame_response_[7] = fn_code;
  ESP_LOGD(TAG, "Sending BLE request of %d bytes (seq=%d)", ble_req.size(), this->c_);
  this->send_ble_request(ble_req);
  this->c_++;
  if (this->c_ > 255) this->c_ = 0;
  this->total_calls_++;
  ESP_LOGD(TAG, "BLE request sent, next seq=%d, total_calls=%d", this->c_, this->total_calls_);
  this->waiting_message_ = true;
  this->waiting_since_ = millis();
  this->last_wait_log_ = 0;
}

void ModbusBleBridge::send_ble_request(const std::vector<uint8_t> &request) {
  ESP_LOGD(TAG, "send_ble_request invoked, size=%d", request.size());
  if (!this->parent_ || !this->parent_->connected()) {
    ESP_LOGW(TAG, "BLE not connected when attempting to send request");
    return;
  }
  if (!this->char_write_) {
    this->char_write_ = this->parent_->get_characteristic(BLE_SERVICE, BLE_CHAR_WRITE);
    if (!this->char_write_) {
      ESP_LOGE(TAG, "BLE write characteristic not found");
      return;
    }
    ESP_LOGD(TAG, "BLE write characteristic acquired");
  }
  this->char_write_->write_value((uint8_t*)request.data(), request.size(), ESP_GATT_WRITE_TYPE_NO_RSP);
  ESP_LOGD(TAG, "BLE request sent successfully");
}

void ModbusBleBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "Modbus BLE Bridge:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->modbus_port_);
  ESP_LOGCONFIG(TAG, "  BLE response timeout: %u ms", this->ble_response_timeout_ms_);
}

void ModbusBleBridge::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      ESP_LOGI(TAG, "[gattc_event_handler] ESP_GATTC_OPEN_EVT");
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected successfully!");
        esp_err_t mtu_err = esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        if (mtu_err) ESP_LOGW(TAG, "MTU request failed, status=%d", mtu_err);
      } else {
        ESP_LOGW(TAG, "Connection failed, status=%d", param->open.status);
      }
      break;
    }
    case ESP_GATTC_CFG_MTU_EVT: {
      ESP_LOGI(TAG, "[gattc_event_handler] ESP_GATTC_CFG_MTU_EVT: MTU updated to %d", param->cfg_mtu.mtu);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "[gattc_event_handler] ESP_GATTC_DISCONNECT_EVT");
      this->char_read_ = nullptr;
      this->char_write_ = nullptr;
      this->waiting_message_ = false;
      this->waiting_since_ = 0;
      this->close_client_();
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGI(TAG, "[gattc_event_handler] ESP_GATTC_SEARCH_CMPL_EVT");
      this->char_read_ = this->parent_->get_characteristic(BLE_SERVICE, BLE_CHAR_READ);
      if (this->char_read_ == nullptr) {
        ESP_LOGE(TAG, "Read characteristic not found");
        break;
      }
      ESP_LOGD(TAG, "Found read characteristic");
      auto status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(),
                                                      this->char_read_->handle);
      if (status) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status);
      }
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      ESP_LOGI(TAG, "[gattc_event_handler] ESP_GATTC_REG_FOR_NOTIFY_EVT");
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->char_read_->handle || this->client_fd_ < 0) {
        break;
      }
      this->waiting_message_ = false;
      this->waiting_since_ = 0;
      const uint8_t *pData = param->notify.value;
      size_t length = param->notify.value_len;
      ESP_LOGD(TAG, "BLE notify: length=%d, expected=%d, registers=%d", length, this->total_registers_ + 7, this->total_registers_);
      if (this->modbus_frame_response_.size() >= 8 && this->modbus_frame_response_[7] == 6) {
        ESP_LOGI(TAG, "Write command response - flushing client");
        ESP_LOGI(TAG, "Closing TCP client after write single register response");
        this->close_client_();
        break;
      }
      if (((int)length - 7) != this->total_registers_) {
        this->errlen_++;
        ESP_LOGW(TAG, "Wrong response length (error %d): expected %d registers, got %d bytes", this->errlen_, this->total_registers_, (int)length - 7);
        if (this->errlen_ > 5) {
          ESP_LOGE(TAG, "Too many length errors (5) - resetting connection");
          ESP_LOGI(TAG, "Closing TCP client due to repeated length errors");
          this->close_client_();
        } else {
          this->last_ble_msg_ = millis();
        }
        break;
      }
      ESP_LOGI(TAG, "Received correct BLE response length");
      this->errlen_ = 0;
      this->last_ble_msg_ = millis();
      int longitud = abs(pData[7]);
      ESP_LOGD(TAG, "Response byte count: %d", longitud);
      std::vector<uint8_t> combined;
      combined.reserve(8 + 1 + longitud);
      combined.insert(combined.end(), this->modbus_frame_response_.begin(), this->modbus_frame_response_.end());
      combined.insert(combined.end(), pData + 7, pData + 7 + longitud + 1);
      ESP_LOGI(TAG, "Sending Modbus/TCP response of %d bytes", combined.size());
      if (this->client_fd_ >= 0) {
        ::send(this->client_fd_, (const char*)combined.data(), combined.size(), 0);
        this->close_client_();
        ESP_LOGI(TAG, "Closed TCP client after sending Modbus/TCP response");
      }
      break;
    }
    default:
      break;
  }
}

uint16_t ModbusBleBridge::modrtu_crc(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else
        crc >>= 1;
    }
  }
  return crc;
}

}  // namespace modbus_ble_bridge
}  // namespace esphome
