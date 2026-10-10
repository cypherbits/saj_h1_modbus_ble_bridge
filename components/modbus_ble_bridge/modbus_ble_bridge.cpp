#include "modbus_ble_bridge.h"
#include "esphome/core/log.h"
#include <array>
#include <unistd.h>

namespace esphome {
namespace modbus_ble_bridge {

static const char *const TAG = "modbus_ble_bridge";
static constexpr uint16_t BLE_SERVICE = 0xFFFF;
static constexpr uint16_t BLE_CHAR_READ = 0xFF02;
static constexpr uint16_t BLE_CHAR_WRITE = 0xFF01;

// MBAP (6) + unit id + function code + address (2) + count/value (2).
static constexpr size_t MIN_TCP_FRAME = 12;
static constexpr size_t MAX_TCP_FRAME = 260;
static constexpr size_t MAX_TCP_RESPONSE = 264;
static constexpr int MAX_READ_REGISTERS = 125;  // Modbus limit for FC3
static constexpr uint8_t FC_READ_HOLDING_REGISTERS = 3;
static constexpr uint8_t FC_WRITE_SINGLE_REGISTER = 6;

void ModbusBleBridge::setup() {
  ESP_LOGI(TAG, "Setting up Modbus BLE Bridge");
  this->modbus_request_.resize(MAX_TCP_FRAME);
  this->modbus_frame_response_.resize(8);
  this->tcp_response_.reserve(MAX_TCP_RESPONSE);
  this->c_ = 0;
  this->total_calls_ = 0;
  this->total_timeouts_ = 0;
}

void ModbusBleBridge::note_request_() {
  this->total_calls_++;
#ifdef USE_MODBUS_BLE_BRIDGE_METRICS
  if (this->total_requests_sensor_ != nullptr)
    this->total_requests_sensor_->publish_state(this->total_calls_);
#endif
}

void ModbusBleBridge::note_timeout_() {
  this->total_timeouts_++;
#ifdef USE_MODBUS_BLE_BRIDGE_METRICS
  if (this->timeouts_sensor_ != nullptr)
    this->timeouts_sensor_->publish_state(this->total_timeouts_);
#endif
}

void ModbusBleBridge::close_client_() {
  if (this->client_fd_ >= 0) {
    ::close(this->client_fd_);
    this->client_fd_ = -1;
  }
}

// Send the whole buffer, retrying on short writes and EAGAIN (non-blocking
// socket). Gives up after 1s so the main loop can never block indefinitely.
bool ModbusBleBridge::send_all_(const uint8_t *data, size_t len) {
  if (this->client_fd_ < 0)
    return false;
  size_t sent = 0;
  uint32_t start = millis();
  while (sent < len) {
    ssize_t n = ::send(this->client_fd_, (const char *) (data + sent), len - sent, 0);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (millis() - start > 1000) {
        ESP_LOGW(TAG, "TCP send timed out");
        return false;
      }
      delay(1);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
    return false;
  }
  return true;
}

// Reply with a Modbus/TCP exception PDU (unit + fn|0x80 + exception code).
void ModbusBleBridge::send_exception_(uint8_t fn_code, uint8_t exception_code) {
  if (this->client_fd_ < 0)
    return;
  uint8_t exception[9] = {
      this->modbus_request_[0], this->modbus_request_[1], 0, 0, 0, 3,
      this->modbus_request_[6], static_cast<uint8_t>(fn_code | 0x80), exception_code};
  ESP_LOGD(TAG, "Sending Modbus exception 0x%02X for function %u", exception_code, fn_code);
  this->send_all_(exception, sizeof(exception));
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
    this->note_timeout_();
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
    // Peek (without consuming) up to a full frame to learn how many bytes are
    // buffered. Using the peeked length instead of ioctl(FIONREAD) avoids
    // depending on FIONREAD's byte count and never consumes a partial frame.
    uint8_t peek_buf[MIN_TCP_FRAME];
    int r = ::recv(this->client_fd_, (char*)peek_buf, sizeof(peek_buf), MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) {
      ESP_LOGI(TAG, "TCP client closed by peer");
      this->close_client_();
      return;
    }
    avail = (r > 0) ? r : 0;
  }
  if (!avail) return;

  if (this->waiting_message_) {
    unsigned long now = millis();
    if (now - this->last_wait_log_ >= 1000) {
      ESP_LOGD(TAG, "Still waiting for BLE response. Ignoring new Modbus request.");
      this->last_wait_log_ = now;
    }
    if (this->waiting_since_ != 0 && (now - this->waiting_since_) > this->ble_response_timeout_ms_) {
      this->note_timeout_();
      ESP_LOGW(TAG, "BLE response timeout after %u ms. Resetting state.", this->ble_response_timeout_ms_);
      this->waiting_message_ = false;
      this->waiting_since_ = 0;
      ESP_LOGI(TAG, "Closing TCP client due to BLE timeout while waiting");
      this->close_client_();
    }
    return;
  }

  // Wait until a complete Modbus/TCP frame is buffered. TCP may split the
  // request across segments; consuming a partial frame would lose the request.
  if (avail < static_cast<int>(MIN_TCP_FRAME)) return;

  ESP_LOGD(TAG, "handle_modbus_tcp: available bytes=%d", avail);
  int i = 0;
  if (this->client_fd_ >= 0) {
    int r = ::recv(this->client_fd_, (char*)this->modbus_request_.data(), MAX_TCP_FRAME, MSG_DONTWAIT);
    if (r <= 0) return; else i = r;
  }
  ESP_LOGD(TAG, "Read %d bytes from Modbus TCP client", i);
  if (i < static_cast<int>(MIN_TCP_FRAME)) {
    ESP_LOGW(TAG, "Incomplete Modbus TCP frame: %d bytes, aborting", i);
    return;
  }

  uint8_t unit_id = this->modbus_request_[6];
  uint8_t fn_code = this->modbus_request_[7];
  uint16_t mbap_len = (uint16_t(this->modbus_request_[4]) << 8) | this->modbus_request_[5];

  // Only FC3 (read holding registers) and FC6 (write single register) are
  // tunnelled. Anything else is rejected without touching the inverter.
  if (fn_code != FC_READ_HOLDING_REGISTERS && fn_code != FC_WRITE_SINGLE_REGISTER) {
    ESP_LOGW(TAG, "Unsupported function code %u, sending exception", fn_code);
    this->send_exception_(fn_code, 0x01);  // illegal function
    return;
  }
  // Reject malformed MBAP headers (protocol id must be 0, length must match).
  if (this->modbus_request_[2] != 0 || this->modbus_request_[3] != 0 || mbap_len != 6) {
    ESP_LOGW(TAG, "Invalid MBAP header (protocol id / length), sending exception");
    this->send_exception_(fn_code, 0x03);  // illegal data value
    return;
  }

  uint8_t addr_hi = this->modbus_request_[8];
  uint8_t addr_lo = this->modbus_request_[9];
  uint8_t reg_hi  = this->modbus_request_[10];
  uint8_t reg_lo  = this->modbus_request_[11];

  if (fn_code == FC_READ_HOLDING_REGISTERS) {
    int num_regs = (int(reg_hi) << 8) | reg_lo;
    if (num_regs < 1 || num_regs > MAX_READ_REGISTERS) {
      ESP_LOGW(TAG, "Invalid register count %d, sending exception", num_regs);
      this->send_exception_(fn_code, 0x03);  // illegal data value
      return;
    }
    this->total_registers_ = num_regs * 2 + 3;
  }

  // Always record the pending function code so the notify handler can tell a
  // read response from a write acknowledgement.
  this->modbus_frame_response_[7] = fn_code;
  if (fn_code == FC_READ_HOLDING_REGISTERS) {
    int total_regs = this->total_registers_;
    this->modbus_frame_response_[0] = this->modbus_request_[0];
    this->modbus_frame_response_[1] = this->modbus_request_[1];
    this->modbus_frame_response_[2] = this->modbus_request_[2];
    this->modbus_frame_response_[3] = this->modbus_request_[3];
    this->modbus_frame_response_[4] = static_cast<uint8_t>((total_regs >> 8) & 0xFF);
    this->modbus_frame_response_[5] = static_cast<uint8_t>(total_regs & 0xFF);
    this->modbus_frame_response_[6] = unit_id;
  }

  uint8_t seq = static_cast<uint8_t>(this->c_);
  std::array<uint8_t, 13> ble_req = {77, 0, seq, 9, 50, unit_id, fn_code, addr_hi, addr_lo, reg_hi, reg_lo};
  uint16_t crc = this->modrtu_crc(ble_req.data() + 5, 6);
  ble_req[11] = static_cast<uint8_t>(crc & 0xFF);
  ble_req[12] = static_cast<uint8_t>((crc >> 8) & 0xFF);

  ESP_LOGD(TAG, "Sending BLE request of %d bytes (seq=%d)", (int) ble_req.size(), this->c_);
  if (!this->send_ble_request(ble_req.data(), ble_req.size())) {
    ESP_LOGW(TAG, "Could not forward request to dongle, sending exception");
    this->send_exception_(fn_code, 0x0B);  // gateway target device failed to respond
    return;
  }
  this->c_ = (this->c_ + 1) & 0xFF;
  this->note_request_();
  this->waiting_message_ = true;
  this->waiting_since_ = millis();
  this->last_wait_log_ = 0;
}

bool ModbusBleBridge::send_ble_request(uint8_t *data, size_t len) {
  ESP_LOGD(TAG, "send_ble_request invoked, size=%d", (int) len);
  if (this->parent_ == nullptr || !this->parent_->connected()) {
    ESP_LOGW(TAG, "BLE not connected when attempting to send request");
    return false;
  }
  if (this->char_write_ == nullptr) {
    this->char_write_ = this->parent_->get_characteristic(BLE_SERVICE, BLE_CHAR_WRITE);
    if (this->char_write_ == nullptr) {
      ESP_LOGE(TAG, "BLE write characteristic not found");
      return false;
    }
    ESP_LOGD(TAG, "BLE write characteristic acquired");
  }
  esp_err_t err = this->char_write_->write_value(data, static_cast<int16_t>(len), ESP_GATT_WRITE_TYPE_NO_RSP);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "BLE write_value failed, status=%d", err);
    return false;
  }
  return true;
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
      } else {
        ESP_LOGW(TAG, "Connection failed, status=%d", param->open.status);
      }
      break;
    }
    case ESP_GATTC_CFG_MTU_EVT: {
      ESP_LOGD(TAG, "[gattc_event_handler] ESP_GATTC_CFG_MTU_EVT: MTU updated to %d", param->cfg_mtu.mtu);
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
      if (this->char_read_ == nullptr || param->notify.handle != this->char_read_->handle || this->client_fd_ < 0) {
        break;
      }
      this->waiting_message_ = false;
      this->waiting_since_ = 0;
      const uint8_t *pData = param->notify.value;
      size_t length = param->notify.value_len;
      ESP_LOGD(TAG, "BLE notify: length=%d, expected=%d", (int) length, this->total_registers_ + 7);

      // FC6 (write) acknowledgement: echo the request per Modbus/TCP so the
      // client gets a valid response, then keep the connection open.
      if (this->modbus_frame_response_[7] == FC_WRITE_SINGLE_REGISTER) {
        ESP_LOGD(TAG, "Write acknowledged, sending Modbus echo response");
        this->send_all_(this->modbus_request_.data(), MIN_TCP_FRAME);
        break;
      }

      if (((int)length - 7) != this->total_registers_) {
        this->errlen_++;
        ESP_LOGW(TAG, "Wrong response length (error %d): expected %d registers, got %d bytes", this->errlen_, this->total_registers_, (int)length - 7);
        if (this->errlen_ > 5) {
          ESP_LOGE(TAG, "Too many length errors (5) - resetting connection");
          ESP_LOGI(TAG, "Closing TCP client due to repeated length errors");
          this->errlen_ = 0;
          this->close_client_();
        }
        break;
      }
      this->errlen_ = 0;

      // Validate the payload byte count before reading it, so a bogus value
      // from the dongle can never read past the notification buffer.
      size_t data_len = pData[7];
      if (7 + 1 + data_len > length) {
        ESP_LOGW(TAG, "Notify byte count %d exceeds payload (%d bytes), dropping", (int) data_len, (int) length);
        break;
      }

      this->tcp_response_.clear();
      this->tcp_response_.insert(this->tcp_response_.end(), this->modbus_frame_response_.begin(), this->modbus_frame_response_.end());
      this->tcp_response_.insert(this->tcp_response_.end(), pData + 7, pData + 7 + data_len + 1);
      ESP_LOGD(TAG, "Sending Modbus/TCP response of %d bytes", (int) this->tcp_response_.size());
      this->send_all_(this->tcp_response_.data(), this->tcp_response_.size());
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
