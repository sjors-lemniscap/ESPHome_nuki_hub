#include "nuki_hub.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include "esphome/components/network/util.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::nuki_hub {

using namespace esphome::nuki_lock;

static const char *const TAG = "nuki_hub";

static const uint8_t RESTART_REASON_NONE = 0;
static const uint8_t RESTART_REASON_MQTT = 1;

// Payloads this component writes to its own command topics, never commands
static bool is_placeholder(const std::string &payload) {
  return payload.empty() || payload == "--" || payload == "ack" || payload == "unknown_action" ||
         payload == "denied" || payload == "error";
}

static const char *esp_reset_reason_text() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "ESP_RST_POWERON: Reset due to power-on event.";
    case ESP_RST_EXT: return "ESP_RST_EXT: Reset by external pin.";
    case ESP_RST_SW: return "ESP_RST_SW: Software reset via esp_restart.";
    case ESP_RST_PANIC: return "ESP_RST_PANIC: Software reset due to exception/panic.";
    case ESP_RST_INT_WDT: return "ESP_RST_INT_WDT: Reset (software or hardware) due to interrupt watchdog";
    case ESP_RST_TASK_WDT: return "ESP_RST_TASK_WDT: Reset due to task watchdog.";
    case ESP_RST_WDT: return "ESP_RST_WDT: Reset due to other watchdogs.";
    case ESP_RST_DEEPSLEEP: return "ESP_RST_DEEPSLEEP: Reset after exiting deep sleep mode.";
    case ESP_RST_BROWNOUT: return "ESP_RST_BROWNOUT: Brownout reset (software or hardware)";
    case ESP_RST_SDIO: return "ESP_RST_SDIO: Reset over SDIO.";
    case ESP_RST_USB: return "ESP_RST_USB: Reset by USB peripheral.";
    case ESP_RST_JTAG: return "ESP_RST_JTAG: Reset by JTAG.";
    default: return "ESP_RST_UNKNOWN: Reset reason can not be determined.";
  }
}

const char *NukiHubComponent::result_to_string(CmdResult result) {
  switch (result) {
    case CmdResult::Success: return "success";
    case CmdResult::Failed: return "failed";
    case CmdResult::TimeOut: return "timeOut";
    case CmdResult::Working: return "working";
    case CmdResult::NotPaired: return "notPaired";
    case CmdResult::Lock_Busy: return "busy";
    case CmdResult::Error: return "error";
    default: return "undefined";
  }
}

// --------------------------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------------------------

void NukiHubComponent::setup() {
  this->boot_millis_ = millis();

  this->restart_reason_pref_ =
      global_preferences->make_preference<uint8_t>(fnv1_hash(this->topic_prefix_ + "_nuki_hub_restart"));
  uint8_t reason = RESTART_REASON_NONE;
  if (this->restart_reason_pref_.load(&reason) && reason != RESTART_REASON_NONE) {
    this->restart_reason_ = reason;
    uint8_t none = RESTART_REASON_NONE;
    this->restart_reason_pref_.save(&none);
  }

#ifdef USE_LOGGER
  if (this->mqtt_log_level_ > 0 && logger::global_logger != nullptr) {
    this->log_topic_ = this->hub_topic_("/maintenance/log");
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
          static_cast<NukiHubComponent *>(self)->on_log_(level, message, message_len);
        });
  }
#endif

  NukiBle *ble = this->ble();
  ble->set_debug_connect(this->debug_connect_);
  ble->set_debug_communication(this->debug_communication_);
  ble->set_debug_readable_data(this->debug_readable_data_);
  ble->set_debug_hex_data(this->debug_hex_data_);
  ble->set_debug_command(this->debug_command_);

  // The Bluetooth client is not registered as a component of its own, see loop()
  ble->setup();
  ble->set_event_handler(this);
  ble->initialize();
  ble->set_disconnect_timeout(2000);
  ble->set_general_timeout(this->ble_general_timeout_);
  ble->set_command_timeout(this->ble_command_timeout_);

  if (!ble->is_paired_with_lock() && this->import_address_.size() == 6 && this->import_key_.size() == 32 &&
      this->import_auth_id_.size() == 4) {
    if (ble->import_credentials(this->import_address_.data(), this->import_key_.data(), this->import_auth_id_.data(),
                                this->security_pin_)) {
      ESP_LOGI(TAG, "Took over the pairing given in the configuration");
    } else {
      ESP_LOGE(TAG, "Could not store the pairing given in the configuration");
    }
  }

  if (this->security_pin_ != 0) {
    if (ble->is_lock_ultra()) {
      ble->save_ultra_pincode(this->security_pin_, false);
    } else if (ble->get_security_pincode() != (uint16_t) this->security_pin_) {
      ble->save_security_pincode((uint16_t) this->security_pin_);
    }
  }

  this->setup_device();

  if (ble->is_paired_with_lock()) {
    ESP_LOGI(TAG, "Paired with the Nuki %s", this->kind());
    this->want_state_ = true;
    this->want_config_ = true;
    this->want_battery_ = true;
    this->want_pin_check_ = this->security_pin_ != 0;
    this->want_keypad_ = true;
    this->want_auth_ = true;
    this->want_time_control_ = true;
    this->want_log_ = this->publish_auth_log_;
    this->want_time_ = this->update_time_;
  } else {
    ESP_LOGI(TAG, "Not paired with a Nuki %s yet, enable the pairing mode to pair", this->kind());
    ble->set_pairing_mode_active(this->pairing_mode_);
  }

  if (this->interval_state_ > 0) {
    this->set_interval("state", this->interval_state_ * 1000, [this] {
      this->want_state_ = true;
      this->want_log_ = this->publish_auth_log_;
    });
  }
  if (this->interval_config_ > 0) {
    this->set_interval("config", this->interval_config_ * 1000, [this] {
      this->want_config_ = true;
      this->want_time_ = this->update_time_;
    });
  }
  if (this->interval_battery_ > 0) {
    this->set_interval("battery", this->interval_battery_ * 1000, [this] { this->want_battery_ = true; });
  }
  if (this->interval_keypad_ > 0) {
    this->set_interval("keypad", this->interval_keypad_ * 1000, [this] { this->want_keypad_ = true; });
  }
  if (this->interval_auth_ > 0) {
    this->set_interval("auth", this->interval_auth_ * 1000, [this] { this->want_auth_ = true; });
  }
  if (this->interval_time_control_ > 0) {
    this->set_interval("time_control", this->interval_time_control_ * 1000,
                       [this] { this->want_time_control_ = true; });
  }
}

void NukiHubComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Nuki Hub:");
  ESP_LOGCONFIG(TAG, "  Device: %s", this->kind());
  ESP_LOGCONFIG(TAG, "  Topic prefix: %s", this->topic_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Paired: %s", YESNO(this->is_paired()));
  ESP_LOGCONFIG(TAG, "  Security PIN set: %s", YESNO(this->security_pin_ != 0));
  ESP_LOGCONFIG(TAG, "  Keypad control: %s", YESNO(this->keypad_control_));
  ESP_LOGCONFIG(TAG, "  Publish activity log: %s", YESNO(this->publish_auth_log_));
  ESP_LOGCONFIG(TAG, "  Command retries: %u, %" PRIu32 " ms apart", this->command_retries_, this->command_retry_delay_);
  ESP_LOGCONFIG(TAG, "  Query intervals: state %" PRIu32 " s, config %" PRIu32 " s, battery %" PRIu32 " s, keypad %" PRIu32
                     " s",
                this->interval_state_, this->interval_config_, this->interval_battery_, this->interval_keypad_);
}

// --------------------------------------------------------------------------------------------
// MQTT plumbing
// --------------------------------------------------------------------------------------------

std::string NukiHubComponent::topic_(const char *suffix) const {
  std::string topic = this->topic_prefix_;
  topic += '/';
  topic += this->kind();
  topic += suffix;
  return topic;
}

std::string NukiHubComponent::hub_topic_(const char *suffix) const { return this->topic_prefix_ + suffix; }

#ifdef USE_MQTT
static bool mqtt_publish(const std::string &topic, const char *payload, size_t length, bool retain, uint8_t qos = 1) {
  if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    return false;
  }
  return mqtt::global_mqtt_client->publish(topic, payload, length, qos, retain);
}
#else
static bool mqtt_publish(const std::string &topic, const char *payload, size_t length, bool retain, uint8_t qos = 1) {
  return false;
}
#endif

// Nuki Hub's "MQTT logging": every log line as one message, without the colour codes of the
// serial log
void NukiHubComponent::on_log_(uint8_t level, const char *message, size_t length) {
  if (level > this->mqtt_log_level_ || this->in_log_) {
    return;
  }
  this->in_log_ = true;
  char line[256];
  size_t n = 0;
  for (size_t i = 0; i < length && n < sizeof(line); i++) {
    if (message[i] == 0x1b) {
      while (i < length && message[i] != 'm') {
        i++;
      }
      continue;
    }
    if (message[i] == '\r' || message[i] == '\n') {
      continue;
    }
    line[n++] = message[i];
  }
  if (n > 0) {
    mqtt_publish(this->log_topic_, line, n, false, 0);
  }
  this->in_log_ = false;
}

bool NukiHubComponent::publish_(const char *suffix, const std::string &payload, bool retain) {
  return mqtt_publish(this->topic_(suffix), payload.c_str(), payload.size(), retain);
}

bool NukiHubComponent::publish_(const char *suffix, const char *payload, bool retain) {
  return mqtt_publish(this->topic_(suffix), payload, strlen(payload), retain);
}

bool NukiHubComponent::publish_int_(const char *suffix, int64_t value, bool retain) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%" PRId64, value);
  return this->publish_(suffix, buf, retain);
}

bool NukiHubComponent::publish_json_(const char *suffix, JsonDocument &doc, bool retain) {
  std::string out;
  serializeJson(doc, out);
  return this->publish_(suffix, out, retain);
}

bool NukiHubComponent::publish_hub_(const char *suffix, const std::string &payload, bool retain) {
  return mqtt_publish(this->hub_topic_(suffix), payload.c_str(), payload.size(), retain);
}

void NukiHubComponent::subscribe_(const char *suffix, std::function<void(const std::string &)> &&callback) {
#ifdef USE_MQTT
  mqtt::global_mqtt_client->subscribe(
      this->topic_(suffix),
      [callback = std::move(callback)](const std::string &topic, const std::string &payload) { callback(payload); },
      1);
#endif
}

void NukiHubComponent::subscribe_hub_(const char *suffix, std::function<void(const std::string &)> &&callback) {
#ifdef USE_MQTT
  mqtt::global_mqtt_client->subscribe(
      this->hub_topic_(suffix),
      [callback = std::move(callback)](const std::string &topic, const std::string &payload) { callback(payload); },
      1);
#endif
}

void NukiHubComponent::on_mqtt_connected_() {
#ifdef USE_MQTT
  if (!this->subscribed_) {
    this->subscribed_ = true;

    this->subscribe_("/action", [this](const std::string &payload) {
      if (is_placeholder(payload)) {
        return;
      }
      ESP_LOGI(TAG, "%s action received: %s", this->kind(), payload.c_str());
      // The plain action name of Nuki Hub, or {"action": "...", "requestId": "..."}
      std::string action = payload;
      std::string request_id;
      if (payload[0] == '{') {
        JsonDocument json;
        if (deserializeJson(json, payload) || !json["action"].is<const char *>()) {
          this->publish_("/action", "unknown_action");
          return;
        }
        action = json["action"].as<const char *>();
        request_id = util::as_text(json["requestId"]);
      }
      if (!this->is_paired()) {
        this->publish_("/action", "denied");
        this->publish_("/commandResult", "notPaired");
        this->publish_result_("/actionResult", request_id, action, "notPaired", false, millis());
        return;
      }
      if (this->queue_action(action, request_id)) {
        this->publish_("/action", "ack");
      } else {
        this->publish_("/action", "unknown_action");
        this->publish_result_("/actionResult", request_id, action, "unknown_action", false, millis());
      }
    });

    auto query = [this](const char *suffix, bool *flag) {
      this->subscribe_(suffix, [this, suffix, flag](const std::string &payload) {
        if (payload != "1") {
          return;
        }
        *flag = true;
        this->publish_(suffix, "0");
      });
    };
    query("/query/lockstate", &this->want_state_);
    query("/query/config", &this->want_config_);
    query("/query/battery", &this->want_battery_);
    query("/query/keypad", &this->want_keypad_);

    this->subscribe_("/keypad/actionJson", [this](const std::string &payload) {
      if (is_placeholder(payload)) {
        return;
      }
      this->handle_keypad_action_(payload);
    });
    this->subscribe_("/authorization/action", [this](const std::string &payload) {
      if (is_placeholder(payload)) {
        return;
      }
      this->handle_auth_action_(payload);
    });
    this->subscribe_("/timecontrol/action", [this](const std::string &payload) {
      if (is_placeholder(payload)) {
        return;
      }
      this->handle_time_control_action(payload);
    });
    this->subscribe_("/configuration/action", [this](const std::string &payload) {
      if (is_placeholder(payload)) {
        return;
      }
      this->handle_config_action(payload);
    });

    // The marker of the last published log entry survives a restart as a retained message
    this->subscribe_("/lastRollingLog", [this](const std::string &payload) {
      if (this->log_marker_known_ || !util::is_number(payload)) {
        return;
      }
      this->log_marker_ = strtoul(payload.c_str(), nullptr, 10);
      this->log_marker_known_ = true;
      ESP_LOGD(TAG, "Last published log entry: %" PRIu32, this->log_marker_);
    });

    this->subscribe_hub_("/maintenance/reset", [this](const std::string &payload) {
      if (payload != "1") {
        return;
      }
      ESP_LOGI(TAG, "Restart requested via MQTT");
      this->publish_hub_("/maintenance/reset", "0");
      // A clean disconnect sends no last will: say it here
      if (this->maintenance_enabled_) {
        this->publish_hub_("/maintenance/mqttConnectionState", "offline");
      }
      uint8_t reason = RESTART_REASON_MQTT;
      this->restart_reason_pref_.save(&reason);
      global_preferences->sync();
      this->set_timeout("restart", 500, [] { App.safe_reboot(); });
    });
  }

  this->log_marker_deadline_ = millis() + 3000;

  // What happened while the broker was away was not published: read everything again
  if (this->mqtt_connects_++ > 0 && this->is_paired()) {
    this->want_state_ = this->want_config_ = this->want_battery_ = true;
    this->want_keypad_ = this->want_auth_ = this->want_time_control_ = true;
    this->want_log_ = this->publish_auth_log_;
  }

  this->publish_("/action", "--");
  this->publish_("/keypad/actionJson", "--");
  this->publish_("/keypad/commandResultJson", "--");
  this->publish_("/timecontrol/action", "--");
  this->publish_("/authorization/action", "--");
  this->publish_("/configuration/action", "--");
  this->publish_("/query/lockstate", "0");
  this->publish_("/query/config", "0");
  this->publish_("/query/battery", "0");
  this->publish_("/query/keypad", "0");
  this->publish_hub_("/maintenance/reset", "0");
  this->availability_published_ = false;
  this->set_available_(this->available_);

  char address[18] = {0};
  if (this->is_paired()) {
    this->ble()->get_mac_address(address);
    this->publish_("/address", address);
  }
  this->publish_maintenance_(true);
#endif
}

void NukiHubComponent::publish_maintenance_(bool initial) {
  if (!this->maintenance_enabled_) {
    return;
  }
  char buf[32];
  if (initial) {
    this->publish_hub_("/maintenance/mqttConnectionState", "online");
    this->publish_hub_("/maintenance/restartReasonNukiHub",
                       this->restart_reason_ == RESTART_REASON_MQTT ? "RequestedViaMqtt" : "NotApplicable");
    this->publish_hub_("/maintenance/restartReasonNukiEsp", esp_reset_reason_text());
    this->publish_hub_("/info/nukiHubVersion", std::string("esphome-") + NUKI_HUB_VERSION);
    this->publish_hub_("/info/nukiHubBuild", ESPHOME_VERSION);
    for (const auto &ip : network::get_ip_addresses()) {
      if (ip.is_set()) {
        char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
        this->publish_hub_("/info/nukiHubIp", ip.str_to(ip_buf));
        break;
      }
    }
#ifdef USE_WIFI
    this->publish_hub_("/maintenance/networkDevice", "Built-in Wi-Fi");
#else
    this->publish_hub_("/maintenance/networkDevice", "Ethernet");
#endif
  }
  snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t) ((millis() - this->boot_millis_) / 60000));
  this->publish_hub_("/maintenance/uptime", buf);
  snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t) esp_get_free_heap_size());
  this->publish_hub_("/maintenance/freeHeap", buf);
  // freeHeap includes external RAM (PSRAM) if the board has it, these two don't
  snprintf(buf, sizeof(buf), "%u", (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  this->publish_hub_("/maintenance/freeHeapInternal", buf);
  snprintf(buf, sizeof(buf), "%u", (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  this->publish_hub_("/maintenance/minFreeHeapInternal", buf);
  this->publish_status_();
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    snprintf(buf, sizeof(buf), "%d", wifi::global_wifi_component->wifi_rssi());
    this->publish_hub_("/maintenance/wifiRssi", buf);
  }
#endif
  if (this->is_paired()) {
    this->publish_int_("/rssi", this->ble()->get_rssi());
  }
}

// The Nuki device advertises about once a second. Without advertisements for minutes it is
// out of reach or without power, whatever the last command said.
void NukiHubComponent::check_beacon_age_() {
  if (!this->is_paired()) {
    return;
  }
  const int64_t last = this->ble()->get_last_received_beacon_ts();
  const int64_t now = esp_timer_get_time() / 1000;
  const bool silent = now - last > BEACON_TIMEOUT_MS && now > BEACON_TIMEOUT_MS;
  if (silent && !this->beacon_lost_) {
    this->beacon_lost_ = true;
    ESP_LOGW(TAG, "No Bluetooth advertisement from the %s for %u seconds", this->kind(),
             (unsigned) (BEACON_TIMEOUT_MS / 1000));
    this->set_available_(false);
  } else if (!silent && this->beacon_lost_) {
    this->beacon_lost_ = false;
    ESP_LOGI(TAG, "The %s is advertising again", this->kind());
    this->want_state_ = true;
  }
}

void NukiHubComponent::set_available_(bool available) {
  if (this->available_ == available && this->availability_published_) {
    return;
  }
  bool changed = this->available_ != available;
  this->available_ = available;
  this->availability_published_ = this->publish_("/availability", available ? "online" : "offline");
  if (changed) {
    if (!available) {
      this->publish_unavailable();
    }
    this->publish_status_();
    this->state_callback_.call();
  }
}

// Everything about the link to the Nuki device in one message
void NukiHubComponent::publish_status_() {
  JsonDocument json;
  const bool paired = this->is_paired();
  json["paired"] = paired;
  json["available"] = this->available_;
  json["pinValid"] = this->pin_valid_;
  if (paired) {
    const int64_t last = this->ble()->get_last_received_beacon_ts();
    json["rssi"] = this->ble()->get_rssi();
    if (last > 0) {
      json["lastSeenSecondsAgo"] = (uint32_t) ((esp_timer_get_time() / 1000 - last) / 1000);
    }
  }
  json["queued"] = this->queue_.size() + (this->op_active_ ? 1 : 0);
  json["consecutiveFailures"] = this->consecutive_failures_;
  this->publish_json_("/status", json);
}

const char *NukiHubComponent::error_name(uint8_t code) {
  switch (code) {
    case 0xFD: return "ERROR_BAD_CRC";
    case 0xFE: return "ERROR_BAD_LENGTH";
    case 0xFF: return "ERROR_UNKNOWN";
    case 0x10: return "P_ERROR_NOT_PAIRING";
    case 0x11: return "P_ERROR_BAD_AUTHENTICATOR";
    case 0x12: return "P_ERROR_BAD_PARAMETER";
    case 0x13: return "P_ERROR_MAX_USER";
    case 0x20: return "K_ERROR_NOT_AUTHORIZED";
    case 0x21: return "K_ERROR_BAD_PIN";
    case 0x22: return "K_ERROR_BAD_NONCE";
    case 0x23: return "K_ERROR_BAD_PARAMETER";
    case 0x24: return "K_ERROR_INVALID_AUTH_ID";
    case 0x25: return "K_ERROR_DISABLED";
    case 0x26: return "K_ERROR_REMOTE_NOT_ALLOWED";
    case 0x27: return "K_ERROR_TIME_NOT_ALLOWED";
    case 0x28: return "K_ERROR_TOO_MANY_PIN_ATTEMPTS";
    case 0x29: return "K_ERROR_TOO_MANY_ENTRIES";
    case 0x2A: return "K_ERROR_CODE_ALREADY_EXISTS";
    case 0x2B: return "K_ERROR_CODE_INVALID";
    case 0x2C: return "K_ERROR_CODE_INVALID_TIMEOUT_1";
    case 0x2D: return "K_ERROR_CODE_INVALID_TIMEOUT_2";
    case 0x2E: return "K_ERROR_CODE_INVALID_TIMEOUT_3";
    case 0x40: return "K_ERROR_AUTO_UNLOCK_TOO_RECENT";
    case 0x41: return "K_ERROR_POSITION_UNKNOWN";
    case 0x42: return "K_ERROR_MOTOR_BLOCKED";
    case 0x43: return "K_ERROR_CLUTCH_FAILURE";
    case 0x44: return "K_ERROR_MOTOR_TIMEOUT";
    case 0x45: return "K_ERROR_BUSY";
    case 0x46: return "K_ERROR_CANCELED";
    case 0x47: return "K_ERROR_NOT_CALIBRATED";
    case 0x48: return "K_ERROR_MOTOR_POSITION_LIMIT";
    case 0x49: return "K_ERROR_MOTOR_LOW_VOLTAGE";
    case 0x4A: return "K_ERROR_MOTOR_POWER_FAILURE";
    case 0x4B: return "K_ERROR_CLUTCH_POWER_FAILURE";
    case 0x4C: return "K_ERROR_VOLTAGE_TOO_LOW";
    case 0x4D: return "K_ERROR_FIRMWARE_UPDATE_NEEDED";
    default: return "unknown";
  }
}

void NukiHubComponent::publish_result_(const char *topic, const std::string &request_id, const std::string &action,
                                       const char *result, bool refused_by_device, uint32_t received_at,
                                       const std::function<void(JsonObject)> &extra) {
  JsonDocument doc;
  JsonObject json = doc.to<JsonObject>();
  if (!request_id.empty()) {
    json["requestId"] = request_id;
  }
  if (!action.empty()) {
    json["action"] = action;
  }
  json["result"] = result;
  if (refused_by_device) {
    const uint8_t code = this->ble()->get_last_error_code();
    json["error"] = error_name(code);
    json["errorCode"] = code;
  }
  json["durationMs"] = millis() - received_at;
  if (extra) {
    extra(json);
  }
  this->publish_json_(topic, doc, false);
}

void NukiHubComponent::answer_command_(const char *base, const std::string &request_id, const std::string &action,
                                       const char *result, bool refused_by_device, uint32_t received_at) {
  std::string topic = base;
  this->publish_((topic + "/commandResult").c_str(), result);
  this->publish_result_((topic + "/result").c_str(), request_id, action, result, refused_by_device, received_at);
}

// --------------------------------------------------------------------------------------------
// Queue
// --------------------------------------------------------------------------------------------

void NukiHubComponent::enqueue(Operation op, bool front) {
  if (this->queue_.size() >= MAX_QUEUE) {
    ESP_LOGW(TAG, "Command queue full, dropping '%s'", op.name);
    if (op.done) {
      op.done(CmdResult::Lock_Busy);
    }
    return;
  }
  if (front) {
    this->queue_.push_front(std::move(op));
  } else {
    this->queue_.push_back(std::move(op));
  }
}

bool NukiHubComponent::start_next_operation_() {
  if (!this->queue_.empty()) {
    this->op_ = std::move(this->queue_.front());
    this->queue_.pop_front();
  } else if (this->want_pin_check_) {
    this->want_pin_check_ = false;
    this->op_ = this->make_pin_operation_();
  } else if (this->want_state_) {
    this->want_state_ = false;
    this->op_ = this->make_state_operation();
  } else if (this->want_config_) {
    this->want_config_ = false;
    this->op_ = this->make_config_operation();
  } else if (this->want_time_) {
    this->want_time_ = false;
    this->op_ = this->make_time_operation_();
    if (!this->op_.step) {
      return false;
    }
  } else if (this->want_keypad_ && this->config_valid()) {
    this->want_keypad_ = false;
    if (!this->has_keypad() || !this->pin_valid_) {
      return false;
    }
    this->op_ = this->make_keypad_operation_();
  } else if (this->want_log_ && this->log_marker_ready_()) {
    this->want_log_ = false;
    if (!this->pin_valid_) {
      return false;
    }
    this->op_ = this->make_log_operation();
  } else if (this->want_battery_) {
    this->want_battery_ = false;
    this->op_ = this->make_battery_operation();
  } else if (this->want_auth_) {
    this->want_auth_ = false;
    if (!this->pin_valid_) {
      return false;
    }
    this->op_ = this->make_auth_operation_();
  } else if (this->want_time_control_) {
    this->want_time_control_ = false;
    if (!this->pin_valid_) {
      return false;
    }
    this->op_ = this->make_time_control_operation();
  } else {
    return false;
  }
  this->op_active_ = true;
  this->op_retry_pending_ = false;
  this->op_retries_left_ = this->op_.retry ? this->command_retries_ : 0;
  ESP_LOGD(TAG, "Starting: %s", this->op_.name);
  return true;
}

void NukiHubComponent::finish_operation_(CmdResult result) {
  // A refusal by the device (Failed) or missing pairing won't change by asking again
  const bool retryable = result != CmdResult::Success && result != CmdResult::Failed && result != CmdResult::NotPaired;

  if (result == CmdResult::Success) {
    this->consecutive_failures_ = 0;
    this->set_available_(true);
  } else if (result != CmdResult::Failed && this->consecutive_failures_ < 255) {
    this->consecutive_failures_++;
  }

  if (retryable && this->op_retries_left_ > 0) {
    const uint8_t attempt = this->command_retries_ - this->op_retries_left_ + 1;
    this->op_retries_left_--;
    ESP_LOGW(TAG, "%s: Last command failed (%s), retrying after %" PRIu32 " milliseconds. Retry %u of %u", this->kind(),
             result_to_string(result), this->command_retry_delay_, attempt, this->command_retries_);
    this->publish_int_("/retry", attempt);
    this->op_retry_pending_ = true;
    this->next_op_time_ = millis() + this->command_retry_delay_;
    return;
  }

  if (result != CmdResult::Success) {
    ESP_LOGD(TAG, "%s: %s", this->op_.name, result_to_string(result));
    if (this->consecutive_failures_ >= 5) {
      this->set_available_(false);
    }
  } else {
    ESP_LOGD(TAG, "%s: success", this->op_.name);
  }
  if (this->op_retries_left_ != this->command_retries_ && this->op_.retry) {
    this->publish_int_("/retry", 0);
  }

  this->op_active_ = false;
  this->op_retry_pending_ = false;
  this->next_op_time_ = millis() + 20;
  std::function<void(CmdResult)> done = std::move(this->op_.done);
  this->op_ = Operation{};
  if (done) {
    done(result);
  }
}

void NukiHubComponent::loop() {
  NukiBle *ble = this->ble();
  // Drives the registration and the connection lifecycle of the Bluetooth client
  ble->loop();

  const uint32_t now = millis();
  const bool busy = this->op_active_ && !this->op_retry_pending_;
  if (now - this->last_loop_ < (busy ? 20 : 100)) {
    return;
  }
  this->last_loop_ = now;

#ifdef USE_MQTT
  const bool mqtt_connected = mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected();
  if (mqtt_connected && !this->mqtt_was_connected_) {
    this->on_mqtt_connected_();
  }
  this->mqtt_was_connected_ = mqtt_connected;
  if (mqtt_connected && now - this->last_maintenance_ >= 60000) {
    this->last_maintenance_ = now;
    this->publish_maintenance_(false);
  }
#endif
  if (now - this->last_beacon_check_ >= 10000) {
    this->last_beacon_check_ = now;
    this->check_beacon_age_();
  }

  ble->update_connection_state();

  if (!ble->is_paired_with_lock()) {
    if (this->pairing_mode_) {
      AuthorizationIdType type = this->pairing_as_app_ ? AuthorizationIdType::App : AuthorizationIdType::Bridge;
      if (ble->pair_nuki(type) == PairingResult::Success) {
        ESP_LOGI(TAG, "Paired with the Nuki %s", this->kind());
        this->set_pairing_mode(false);
        if (this->security_pin_ != 0) {
          if (ble->is_lock_ultra()) {
            ble->save_ultra_pincode(this->security_pin_);
          } else {
            ble->save_security_pincode((uint16_t) this->security_pin_);
          }
        }
        this->want_state_ = this->want_config_ = this->want_battery_ = true;
        this->want_keypad_ = this->want_auth_ = this->want_time_control_ = true;
        this->want_pin_check_ = this->security_pin_ != 0;
        this->want_log_ = this->publish_auth_log_;
        char address[18] = {0};
        ble->get_mac_address(address);
        this->publish_("/address", address);
      }
    }
    return;
  }

  if (this->keypad_refresh_at_ != 0 && (int32_t) (now - this->keypad_refresh_at_) >= 0 &&
      this->keypad_commands_pending_ == 0) {
    this->keypad_refresh_at_ = 0;
    this->want_keypad_ = true;
  }
  if (this->log_refresh_at_ != 0 && (int32_t) (now - this->log_refresh_at_) >= 0) {
    this->log_refresh_at_ = 0;
    this->want_log_ = this->publish_auth_log_;
  }

  if (this->op_active_ && !this->op_retry_pending_) {
    CmdResult result = this->op_.step();
    if (result == CmdResult::Working) {
      result = this->check_attempt_deadline_(now);
    }
    if (result != CmdResult::Working) {
      this->finish_operation_(result);
    }
    return;
  }

  if ((int32_t) (now - this->next_op_time_) < 0) {
    return;
  }

  if (this->op_retry_pending_) {
    this->op_retry_pending_ = false;
  } else if (!this->start_next_operation_()) {
    return;
  }
  this->attempt_started_ = now;
  this->attempt_connected_ = false;
  CmdResult result = this->op_.step();
  if (result != CmdResult::Working) {
    this->finish_operation_(result);
  }
}

// The Bluetooth code waits for the connection without a limit of its own: a device that is
// out of reach, switched off or held by another peer would block the queue for good.
CmdResult NukiHubComponent::check_attempt_deadline_(uint32_t now) {
  NukiBle *ble = this->ble();
  if (!this->attempt_connected_ && ble->connected()) {
    this->attempt_connected_ = true;
  }
  const uint32_t elapsed = now - this->attempt_started_;
  if (!this->attempt_connected_ && elapsed > CONNECT_DEADLINE_MS) {
    ESP_LOGW(TAG, "%s: Send encr msg failed due to unable to connect", this->kind());
    ble->disconnect();
    return CmdResult::TimeOut;
  }
  if (elapsed > ATTEMPT_DEADLINE_MS) {
    ESP_LOGW(TAG, "%s: '%s' did not finish within %u seconds", this->kind(), this->op_.name,
             (unsigned) (ATTEMPT_DEADLINE_MS / 1000));
    ble->disconnect();
    return CmdResult::TimeOut;
  }
  return CmdResult::Working;
}

void NukiHubComponent::notify(EventType event_type) {
  switch (event_type) {
    case EventType::KeyTurnerStatusUpdated:
      if (!this->status_flag_) {
        this->status_flag_ = true;
        ESP_LOGD(TAG, "The %s announces a state change", this->kind());
        this->publish_("/statusUpdated", "1");
        this->want_state_ = true;
        this->want_log_ = this->publish_auth_log_;
      }
      break;
    case EventType::KeyTurnerStatusReset:
      this->status_flag_ = false;
      this->publish_("/statusUpdated", "0");
      break;
    case EventType::ERROR_BAD_PIN:
      ESP_LOGW(TAG, "The %s refused the security PIN", this->kind());
      this->pin_valid_ = false;
      break;
    case EventType::BLE_ERROR_ON_DISCONNECT:
      ESP_LOGW(TAG, "Bluetooth connection ended unexpectedly");
      break;
  }
}

void NukiHubComponent::set_pairing_mode(bool enabled) {
  this->pairing_mode_ = enabled;
  this->ble()->set_pairing_mode_active(enabled);
  ESP_LOGI(TAG, "Pairing mode %s", enabled ? "on" : "off");
  this->state_callback_.call();
}

void NukiHubComponent::unpair() {
  this->ble()->unpair_nuki();
  this->queue_.clear();
  this->op_active_ = false;
  this->pin_valid_ = false;
  this->pin_checked_ = false;
  this->set_available_(false);
  ESP_LOGI(TAG, "Pairing removed");
}

bool NukiHubComponent::request_action(const std::string &action) {
  if (!this->is_paired()) {
    return false;
  }
  return this->queue_action(action, "");
}

// --------------------------------------------------------------------------------------------
// PIN and time
// --------------------------------------------------------------------------------------------

Operation NukiHubComponent::make_pin_operation_() {
  Operation op;
  op.name = "verify security pin";
  op.step = [this] { return this->ble()->verify_security_pin(); };
  op.done = [this](CmdResult result) {
    this->pin_checked_ = true;
    this->pin_valid_ = result == CmdResult::Success;
    if (this->pin_valid_) {
      ESP_LOGI(TAG, "Security PIN is valid");
    } else if (result == CmdResult::Failed) {
      ESP_LOGE(TAG, "Security PIN is not valid, keypad, authorizations, time control and the log stay unavailable");
    } else {
      // Could not ask, try again with the next state update
      this->pin_checked_ = false;
      this->want_pin_check_ = true;
    }
  };
  return op;
}

Operation NukiHubComponent::make_time_operation_() {
  Operation op;
  op.name = "update time";
#ifdef USE_TIME
  if (this->clock_ == nullptr || !this->pin_valid_) {
    return op;
  }
  ESPTime now = this->clock_->now();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "NTP Time not valid, not updating the time of the %s", this->kind());
    this->want_time_ = false;
    this->set_timeout("time_retry", 60000, [this] { this->want_time_ = this->update_time_; });
    return op;
  }
  op.step = [this] {
    ESPTime t = this->clock_->now();
    TimeValue value{};
    value.year = t.year;
    value.month = t.month;
    value.day = t.day_of_month;
    value.hour = t.hour;
    value.minute = t.minute;
    value.second = t.second;
    return this->ble()->update_time(value);
  };
#endif
  return op;
}

// --------------------------------------------------------------------------------------------
// Keypad
// --------------------------------------------------------------------------------------------

const KeypadEntry *NukiHubComponent::find_keypad_entry_(uint16_t code_id) const {
  for (const auto &entry : this->keypad_entries_) {
    if (entry.codeId == code_id) {
      return &entry;
    }
  }
  return nullptr;
}

Operation NukiHubComponent::make_keypad_operation_() {
  Operation op;
  op.name = "read keypad codes";
  op.step = [this] { return this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    this->ble()->get_keypad_entries(&this->keypad_entries_);
    std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
              [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
    this->keypad_cache_valid_ = true;
    ESP_LOGI(TAG, "%s keypad codes: %u", this->kind(), (unsigned) this->keypad_entries_.size());
    this->publish_keypad_();
  };
  return op;
}

static void keypad_entry_to_json(const KeypadEntry &entry, JsonObject json, bool include_code) {
  char dt[20];
  char tod[6];
  json["codeId"] = entry.codeId;
  if (include_code) {
    json["code"] = entry.code;
  }
  json["enabled"] = entry.enabled;
  json["name"] = util::fixed_string(entry.name, sizeof(entry.name));
  util::date_time(dt, entry.dateCreatedYear, entry.dateCreatedMonth, entry.dateCreatedDay, entry.dateCreatedHour,
                  entry.dateCreatedMin, entry.dateCreatedSec);
  json["dateCreated"] = dt;
  json["lockCount"] = entry.lockCount;
  util::date_time(dt, entry.dateLastActiveYear, entry.dateLastActiveMonth, entry.dateLastActiveDay,
                  entry.dateLastActiveHour, entry.dateLastActiveMin, entry.dateLastActiveSec);
  json["dateLastActive"] = dt;
  json["timeLimited"] = entry.timeLimited;
  util::date_time(dt, entry.allowedFromYear, entry.allowedFromMonth, entry.allowedFromDay, entry.allowedFromHour,
                  entry.allowedFromMin, entry.allowedFromSec);
  json["allowedFrom"] = dt;
  util::date_time(dt, entry.allowedUntilYear, entry.allowedUntilMonth, entry.allowedUntilDay, entry.allowedUntilHour,
                  entry.allowedUntilMin, entry.allowedUntilSec);
  json["allowedUntil"] = dt;
  util::weekdays_to_json(entry.allowedWeekdays, json["allowedWeekdays"].to<JsonArray>());
  util::time_of_day(tod, entry.allowedFromTimeHour, entry.allowedFromTimeMin);
  json["allowedFromTime"] = tod;
  util::time_of_day(tod, entry.allowedUntilTimeHour, entry.allowedUntilTimeMin);
  json["allowedUntilTime"] = tod;
}

void NukiHubComponent::publish_keypad_() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const auto &entry : this->keypad_entries_) {
    keypad_entry_to_json(entry, list.add<JsonObject>(), this->keypad_publish_code_);
  }
  this->publish_json_("/keypad/json", doc);
}

void NukiHubComponent::handle_keypad_action_(const std::string &payload) {
  const std::string request_id = util::request_id(payload);
  if (!this->is_paired()) {
    this->publish_("/keypad/commandResultJson", "notPaired");
    this->publish_result_("/keypad/result", request_id, "", "notPaired", false, millis());
    return;
  }
  auto cmd = std::make_shared<KeypadCommand>();
  cmd->payload = payload;
  cmd->request_id = request_id;
  cmd->received_at = millis();

  Operation op;
  op.name = "keypad command";
  op.step = [this, cmd] { return this->keypad_command_step_(*cmd); };
  op.done = [this, cmd](CmdResult result) { this->keypad_command_done_(*cmd, result); };

  if (this->queue_.size() >= MAX_QUEUE) {
    this->publish_("/keypad/commandResultJson", "busy");
    this->publish_result_("/keypad/result", request_id, "", "busy", false, cmd->received_at);
    return;
  }
  this->keypad_commands_pending_++;
  this->enqueue(std::move(op));
}

void NukiHubComponent::keypad_command_done_(KeypadCommand &cmd, CmdResult result) {
  if (this->keypad_commands_pending_ > 0) {
    this->keypad_commands_pending_--;
  }
  const char *text = cmd.verdict != nullptr ? cmd.verdict : result_to_string(result);
  if (cmd.verdict != nullptr) {
    ESP_LOGI(TAG, "Keypad command not executed: %s", cmd.verdict);
  }
  if (cmd.verdict == nullptr) {
    ESP_LOGI(TAG, "%s keypad code: %s", cmd.action.c_str(), text);
    if (result == CmdResult::Success) {
      if (cmd.action == "delete") {
        this->keypad_entries_.erase(std::remove_if(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                                                   [&cmd](const KeypadEntry &e) { return e.codeId == cmd.code_id; }),
                                    this->keypad_entries_.end());
      } else if (cmd.action == "add") {
        // The device answers an add with the id it gave the code. Without it the id is
        // only known after the next read.
        cmd.code_id = this->ble()->get_last_keypad_code_id();
        if (cmd.code_id != 0 && this->find_keypad_entry_(cmd.code_id) == nullptr) {
          KeypadEntry e{};
          e.codeId = cmd.code_id;
          e.code = cmd.add.code;
          memcpy(e.name, cmd.add.name, sizeof(e.name));
          e.enabled = 1;
          e.timeLimited = cmd.add.timeLimited;
          e.allowedFromYear = cmd.add.allowedFromYear;
          e.allowedFromMonth = cmd.add.allowedFromMonth;
          e.allowedFromDay = cmd.add.allowedFromDay;
          e.allowedFromHour = cmd.add.allowedFromHour;
          e.allowedFromMin = cmd.add.allowedFromMin;
          e.allowedFromSec = cmd.add.allowedFromSec;
          e.allowedUntilYear = cmd.add.allowedUntilYear;
          e.allowedUntilMonth = cmd.add.allowedUntilMonth;
          e.allowedUntilDay = cmd.add.allowedUntilDay;
          e.allowedUntilHour = cmd.add.allowedUntilHour;
          e.allowedUntilMin = cmd.add.allowedUntilMin;
          e.allowedUntilSec = cmd.add.allowedUntilSec;
          e.allowedWeekdays = cmd.add.allowedWeekdays;
          e.allowedFromTimeHour = cmd.add.allowedFromTimeHour;
          e.allowedFromTimeMin = cmd.add.allowedFromTimeMin;
          e.allowedUntilTimeHour = cmd.add.allowedUntilTimeHour;
          e.allowedUntilTimeMin = cmd.add.allowedUntilTimeMin;
          this->keypad_entries_.push_back(e);
        } else {
          this->keypad_cache_valid_ = false;
        }
      } else {
        for (auto &e : this->keypad_entries_) {
          if (e.codeId != cmd.update.codeId) {
            continue;
          }
          e.code = cmd.update.code;
          memcpy(e.name, cmd.update.name, sizeof(e.name));
          e.enabled = cmd.update.enabled;
          e.timeLimited = cmd.update.timeLimited;
          e.allowedFromYear = cmd.update.allowedFromYear;
          e.allowedFromMonth = cmd.update.allowedFromMonth;
          e.allowedFromDay = cmd.update.allowedFromDay;
          e.allowedFromHour = cmd.update.allowedFromHour;
          e.allowedFromMin = cmd.update.allowedFromMin;
          e.allowedFromSec = cmd.update.allowedFromSec;
          e.allowedUntilYear = cmd.update.allowedUntilYear;
          e.allowedUntilMonth = cmd.update.allowedUntilMonth;
          e.allowedUntilDay = cmd.update.allowedUntilDay;
          e.allowedUntilHour = cmd.update.allowedUntilHour;
          e.allowedUntilMin = cmd.update.allowedUntilMin;
          e.allowedUntilSec = cmd.update.allowedUntilSec;
          e.allowedWeekdays = cmd.update.allowedWeekdays;
          e.allowedFromTimeHour = cmd.update.allowedFromTimeHour;
          e.allowedFromTimeMin = cmd.update.allowedFromTimeMin;
          e.allowedUntilTimeHour = cmd.update.allowedUntilTimeHour;
          e.allowedUntilTimeMin = cmd.update.allowedUntilTimeMin;
        }
      }
    }
    // Read the list back from the device once the commands are through
    this->keypad_refresh_at_ = millis() + 500;
    if (this->keypad_refresh_at_ == 0) {
      this->keypad_refresh_at_ = 1;
    }
  }
  this->publish_("/keypad/commandResultJson", text);

  const bool executed = cmd.verdict == nullptr;
  const uint16_t code_id = cmd.code_id;
  this->publish_result_("/keypad/result", cmd.request_id, cmd.action, text, executed && result == CmdResult::Failed,
                        cmd.received_at, [code_id](JsonObject json) {
                          if (code_id != 0) {
                            json["codeId"] = code_id;
                          }
                        });
  if (executed && result == CmdResult::Success && code_id != 0) {
    // The one entry that changed, for clients that keep a copy of the list
    JsonDocument doc;
    JsonObject json = doc.to<JsonObject>();
    json["change"] = cmd.action == "add" ? "added" : (cmd.action == "delete" ? "deleted" : "updated");
    json["codeId"] = code_id;
    const KeypadEntry *entry = this->find_keypad_entry_(code_id);
    if (entry != nullptr && cmd.action != "delete") {
      keypad_entry_to_json(*entry, json["entry"].to<JsonObject>(), this->keypad_publish_code_);
      if (cmd.action == "add") {
        // Not known before the next read of the list
        json["entry"].remove("dateCreated");
        json["entry"].remove("dateLastActive");
        json["entry"].remove("lockCount");
      }
    }
    this->publish_json_("/keypad/changed", doc, false);
  }
}

// Fills the date and time fields `prefix`Year..Sec of a packed entry from six values
#define NUKI_SET_DATE(entry, prefix, v) \
  do { \
    (entry).prefix##Year = (v)[0]; \
    (entry).prefix##Month = (v)[1]; \
    (entry).prefix##Day = (v)[2]; \
    (entry).prefix##Hour = (v)[3]; \
    (entry).prefix##Min = (v)[4]; \
    (entry).prefix##Sec = (v)[5]; \
  } while (0)

CmdResult NukiHubComponent::keypad_command_step_(KeypadCommand &cmd) {
  auto refuse = [&cmd](const char *verdict) {
    cmd.verdict = verdict;
    return CmdResult::Failed;
  };

  if (cmd.phase == KeypadCommand::Phase::Parse) {
    if (!this->pin_valid_) {
      return refuse(this->security_pin_ == 0 ? "noPinSet" : "noValidPinSet");
    }
    if (!this->keypad_control_) {
      return refuse("keypadControlDisabled");
    }
    if (!this->has_keypad()) {
      return refuse(this->config_valid() ? "keypadNotAvailable" : "configNotReady");
    }

    JsonDocument json;
    if (deserializeJson(json, cmd.payload) || !json.is<JsonObjectConst>()) {
      return refuse("invalidJson");
    }
    if (!json["action"].is<const char *>()) {
      return refuse("noActionSet");
    }
    cmd.action = json["action"].as<const char *>();
    cmd.code_id = json["codeId"].as<unsigned int>();

    if (cmd.action == "check") {
      return refuse("checkingKeypadCodesDisabled");
    }
    if (cmd.action != "add" && cmd.action != "update" && cmd.action != "delete") {
      return refuse("invalidAction");
    }

    if (cmd.action == "delete") {
      if (cmd.code_id == 0) {
        return refuse("noCodeIdSet");
      }
      cmd.phase = KeypadCommand::Phase::Fetch;
    } else {
      std::string name = json["name"].is<JsonVariantConst>() ? util::as_text(json["name"]) : "";
      cmd.has_name = !name.empty();
      if (!cmd.has_name && cmd.action == "add") {
        return refuse("noNameSet");
      }

      uint32_t code = 0;
      if (json["code"].is<JsonVariantConst>()) {
        const std::string code_text = util::as_text(json["code"]);
        code = json["code"].as<unsigned int>();
        if (!(code > 100000 && code < 1000000 && code_text.find('0') == std::string::npos)) {
          return refuse("noValidCodeSet");
        }
        cmd.has_code = true;
      } else if (cmd.action == "add") {
        return refuse("noCodeSet");
      }

      cmd.has_enabled = json["enabled"].is<JsonVariantConst>();
      const uint8_t enabled = cmd.has_enabled ? (json["enabled"].as<unsigned int>() ? 1 : 0) : 1;
      cmd.has_time_limited = json["timeLimited"].is<JsonVariantConst>();
      const uint8_t time_limited = cmd.has_time_limited ? (json["timeLimited"].as<unsigned int>() == 1 ? 1 : 0) : 0;

      uint16_t from[6] = {0}, until[6] = {0};
      uint8_t from_time[2] = {0}, until_time[2] = {0};
      uint8_t weekdays = 0;
      if (time_limited == 1) {
        const std::string allowed_from = util::as_text(json["allowedFrom"]);
        if (!allowed_from.empty()) {
          if (!util::parse_date_time(allowed_from, from)) {
            return refuse("invalidAllowedFrom");
          }
          cmd.has_from = true;
        }
        const std::string allowed_until = util::as_text(json["allowedUntil"]);
        if (!allowed_until.empty()) {
          if (!util::parse_date_time(allowed_until, until)) {
            return refuse("invalidAllowedUntil");
          }
          cmd.has_until = true;
        }
        const std::string allowed_from_time = util::as_text(json["allowedFromTime"]);
        if (!allowed_from_time.empty()) {
          if (!util::parse_time_of_day(allowed_from_time, from_time)) {
            return refuse("invalidAllowedFromTime");
          }
          cmd.has_from_time = true;
        }
        const std::string allowed_until_time = util::as_text(json["allowedUntilTime"]);
        if (!allowed_until_time.empty()) {
          if (!util::parse_time_of_day(allowed_until_time, until_time)) {
            return refuse("invalidAllowedUntilTime");
          }
          cmd.has_until_time = true;
        }
        if (json["allowedWeekdays"].is<JsonVariantConst>()) {
          weekdays = util::weekdays_from_json(json["allowedWeekdays"]);
          cmd.has_weekdays = true;
        }
      }

      if (cmd.action == "add") {
        NewKeypadEntry &e = cmd.add;
        memset(&e, 0, sizeof(e));
        e.code = code;
        memcpy(e.name, name.c_str(), std::min(name.size(), sizeof(e.name)));
        e.timeLimited = time_limited;
        NUKI_SET_DATE(e, allowedFrom, from);
        NUKI_SET_DATE(e, allowedUntil, until);
        e.allowedWeekdays = weekdays;
        e.allowedFromTimeHour = from_time[0];
        e.allowedFromTimeMin = from_time[1];
        e.allowedUntilTimeHour = until_time[0];
        e.allowedUntilTimeMin = until_time[1];
        cmd.phase = KeypadCommand::Phase::Execute;
      } else {
        if (cmd.code_id == 0) {
          return refuse("noCodeIdSet");
        }
        UpdatedKeypadEntry &e = cmd.update;
        memset(&e, 0, sizeof(e));
        e.codeId = cmd.code_id;
        e.code = code;
        memcpy(e.name, name.c_str(), std::min(name.size(), sizeof(e.name)));
        e.enabled = enabled;
        e.timeLimited = time_limited;
        NUKI_SET_DATE(e, allowedFrom, from);
        NUKI_SET_DATE(e, allowedUntil, until);
        e.allowedWeekdays = weekdays;
        e.allowedFromTimeHour = from_time[0];
        e.allowedFromTimeMin = from_time[1];
        e.allowedUntilTimeHour = until_time[0];
        e.allowedUntilTimeMin = until_time[1];
        cmd.phase = KeypadCommand::Phase::Fetch;
      }
    }
  }

  if (cmd.phase == KeypadCommand::Phase::Fetch) {
    // delete and update refer to an existing entry: look it up, in a fresh list if the
    // cached one doesn't have it
    const KeypadEntry *existing = this->keypad_cache_valid_ ? this->find_keypad_entry_(cmd.code_id) : nullptr;
    if (existing == nullptr && !cmd.fetched) {
      CmdResult result = this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_);
      if (result == CmdResult::Working) {
        return result;
      }
      if (result != CmdResult::Success) {
        if (result == CmdResult::Failed) {
          return refuse("failedToRetrieveExistingKeypadEntry");
        }
        return result;
      }
      cmd.fetched = true;
      this->ble()->get_keypad_entries(&this->keypad_entries_);
      std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
      this->keypad_cache_valid_ = true;
      existing = this->find_keypad_entry_(cmd.code_id);
    }
    if (existing == nullptr) {
      return refuse("noExistingCodeIdSet");
    }

    if (cmd.action == "update") {
      // Everything the command left out keeps its current value
      UpdatedKeypadEntry &e = cmd.update;
      if (!cmd.has_name) {
        memcpy(e.name, existing->name, sizeof(e.name));
      }
      if (!cmd.has_code) {
        e.code = existing->code;
      }
      if (!cmd.has_enabled) {
        e.enabled = existing->enabled;
      }
      if (!cmd.has_time_limited) {
        e.timeLimited = existing->timeLimited;
      }
      if (e.timeLimited == 1) {
        if (!cmd.has_from) {
          e.allowedFromYear = existing->allowedFromYear;
          e.allowedFromMonth = existing->allowedFromMonth;
          e.allowedFromDay = existing->allowedFromDay;
          e.allowedFromHour = existing->allowedFromHour;
          e.allowedFromMin = existing->allowedFromMin;
          e.allowedFromSec = existing->allowedFromSec;
        }
        if (!cmd.has_until) {
          e.allowedUntilYear = existing->allowedUntilYear;
          e.allowedUntilMonth = existing->allowedUntilMonth;
          e.allowedUntilDay = existing->allowedUntilDay;
          e.allowedUntilHour = existing->allowedUntilHour;
          e.allowedUntilMin = existing->allowedUntilMin;
          e.allowedUntilSec = existing->allowedUntilSec;
        }
        if (!cmd.has_weekdays) {
          e.allowedWeekdays = existing->allowedWeekdays;
        }
        if (!cmd.has_from_time) {
          e.allowedFromTimeHour = existing->allowedFromTimeHour;
          e.allowedFromTimeMin = existing->allowedFromTimeMin;
        }
        if (!cmd.has_until_time) {
          e.allowedUntilTimeHour = existing->allowedUntilTimeHour;
          e.allowedUntilTimeMin = existing->allowedUntilTimeMin;
        }
      }
    }
    cmd.phase = KeypadCommand::Phase::Execute;
  }

  if (cmd.action == "add") {
    if (!cmd.fetched) {
      // First step of the add itself: the id the device answers with belongs to this command
      cmd.fetched = true;
      this->ble()->clear_last_keypad_code_id();
    }
    return this->ble()->add_keypad_entry(cmd.add);
  }
  if (cmd.action == "update") {
    return this->ble()->update_keypad_entry(cmd.update);
  }
  return this->ble()->delete_keypad_entry(cmd.code_id);
}

// --------------------------------------------------------------------------------------------
// Authorizations
// --------------------------------------------------------------------------------------------

const char *NukiHubComponent::auth_name_(uint32_t auth_id) const {
  for (const auto &entry : this->auth_entries_) {
    if (entry.authId == auth_id) {
      return reinterpret_cast<const char *>(entry.name);
    }
  }
  return nullptr;
}

Operation NukiHubComponent::make_auth_operation_() {
  Operation op;
  op.name = "read authorizations";
  op.step = [this] { return this->ble()->retrieve_authorization_entries(0, this->max_auth_entries_); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    this->ble()->get_authorization_entries(&this->auth_entries_);
    std::sort(this->auth_entries_.begin(), this->auth_entries_.end(),
              [](const AuthorizationEntry &a, const AuthorizationEntry &b) { return a.authId < b.authId; });
    for (auto &entry : this->auth_entries_) {
      entry.name[sizeof(entry.name) - 1] = 0;
    }
    this->publish_auth_();
  };
  return op;
}

void NukiHubComponent::publish_auth_() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  char dt[20];
  char tod[6];
  for (const auto &entry : this->auth_entries_) {
    JsonObject json = list.add<JsonObject>();
    json["authId"] = entry.authId;
    json["idType"] = entry.idType;
    json["enabled"] = entry.enabled;
    json["name"] = util::fixed_string(entry.name, sizeof(entry.name));
    json["remoteAllowed"] = entry.remoteAllowed;
    util::date_time(dt, entry.createdYear, entry.createdMonth, entry.createdDay, entry.createdHour,
                    entry.createdMinute, entry.createdSecond);
    json["dateCreated"] = dt;
    json["lockCount"] = entry.lockCount;
    util::date_time(dt, entry.lastActYear, entry.lastActMonth, entry.lastActDay, entry.lastActHour,
                    entry.lastActMinute, entry.lastActSecond);
    json["dateLastActive"] = dt;
    json["timeLimited"] = entry.timeLimited;
    util::date_time(dt, entry.allowedFromYear, entry.allowedFromMonth, entry.allowedFromDay, entry.allowedFromHour,
                    entry.allowedFromMinute, entry.allowedFromSecond);
    json["allowedFrom"] = dt;
    util::date_time(dt, entry.allowedUntilYear, entry.allowedUntilMonth, entry.allowedUntilDay,
                    entry.allowedUntilHour, entry.allowedUntilMinute, entry.allowedUntilSecond);
    json["allowedUntil"] = dt;
    util::weekdays_to_json(entry.allowedWeekdays, json["allowedWeekdays"].to<JsonArray>());
    util::time_of_day(tod, entry.allowedFromTimeHour, entry.allowedFromTimeMin);
    json["allowedFromTime"] = tod;
    util::time_of_day(tod, entry.allowedUntilTimeHour, entry.allowedUntilTimeMin);
    json["allowedUntilTime"] = tod;
  }
  this->publish_json_("/authorization/json", doc);
}

void NukiHubComponent::handle_auth_action_(const std::string &payload) {
  const std::string request_id = util::request_id(payload);
  const uint32_t received_at = millis();
  auto answer = [this, &request_id, received_at](const char *text) {
    this->answer_command_("/authorization", request_id, "", text, false, received_at);
  };

  if (!this->pin_valid_) {
    answer(this->security_pin_ == 0 ? "noPinSet" : "noValidPinSet");
    return;
  }
  JsonDocument json;
  if (deserializeJson(json, payload) || !json.is<JsonObjectConst>()) {
    answer("invalidJson");
    return;
  }
  if (!json["action"].is<const char *>()) {
    answer("noActionSet");
    return;
  }
  const std::string action = json["action"].as<const char *>();
  const uint32_t auth_id = json["authId"].as<unsigned int>();
  if (action != "update" && action != "delete") {
    answer(action == "add" ? "addNotSupported" : "invalidAction");
    return;
  }
  if (auth_id == 0) {
    answer("noAuthIdSet");
    return;
  }
  const AuthorizationEntry *existing = nullptr;
  for (const auto &entry : this->auth_entries_) {
    if (entry.authId == auth_id) {
      existing = &entry;
    }
  }
  if (existing == nullptr) {
    answer("noExistingAuthIdSet");
    return;
  }

  Operation op;
  if (action == "delete") {
    op.name = "delete authorization";
    op.step = [this, auth_id] { return this->ble()->delete_authorization_entry(auth_id); };
  } else {
    UpdatedAuthorizationEntry e{};
    e.authId = auth_id;
    memcpy(e.name, existing->name, sizeof(e.name));
    e.enabled = existing->enabled;
    e.remoteAllowed = existing->remoteAllowed;
    e.timeLimited = existing->timeLimited;
    e.allowedFromYear = existing->allowedFromYear;
    e.allowedFromMonth = existing->allowedFromMonth;
    e.allowedFromDay = existing->allowedFromDay;
    e.allowedFromHour = existing->allowedFromHour;
    e.allowedFromMinute = existing->allowedFromMinute;
    e.allowedFromSecond = existing->allowedFromSecond;
    e.allowedUntilYear = existing->allowedUntilYear;
    e.allowedUntilMonth = existing->allowedUntilMonth;
    e.allowedUntilDay = existing->allowedUntilDay;
    e.allowedUntilHour = existing->allowedUntilHour;
    e.allowedUntilMinute = existing->allowedUntilMinute;
    e.allowedUntilSecond = existing->allowedUntilSecond;
    e.allowedWeekdays = existing->allowedWeekdays;
    e.allowedFromTimeHour = existing->allowedFromTimeHour;
    e.allowedFromTimeMin = existing->allowedFromTimeMin;
    e.allowedUntilTimeHour = existing->allowedUntilTimeHour;
    e.allowedUntilTimeMin = existing->allowedUntilTimeMin;

    if (json["name"].is<JsonVariantConst>()) {
      const std::string name = util::as_text(json["name"]);
      if (!name.empty()) {
        memset(e.name, 0, sizeof(e.name));
        memcpy(e.name, name.c_str(), std::min(name.size(), sizeof(e.name)));
      }
    }
    if (json["enabled"].is<JsonVariantConst>()) {
      e.enabled = json["enabled"].as<unsigned int>() ? 1 : 0;
    }
    if (json["remoteAllowed"].is<JsonVariantConst>()) {
      e.remoteAllowed = json["remoteAllowed"].as<unsigned int>() ? 1 : 0;
    }
    if (json["timeLimited"].is<JsonVariantConst>()) {
      e.timeLimited = json["timeLimited"].as<unsigned int>() == 1 ? 1 : 0;
    }
    if (e.timeLimited == 1) {
      uint16_t dt[6];
      uint8_t tod[2];
      const std::string from = util::as_text(json["allowedFrom"]);
      if (!from.empty()) {
        if (!util::parse_date_time(from, dt)) {
          answer("invalidAllowedFrom");
          return;
        }
        e.allowedFromYear = dt[0];
        e.allowedFromMonth = dt[1];
        e.allowedFromDay = dt[2];
        e.allowedFromHour = dt[3];
        e.allowedFromMinute = dt[4];
        e.allowedFromSecond = dt[5];
      }
      const std::string until = util::as_text(json["allowedUntil"]);
      if (!until.empty()) {
        if (!util::parse_date_time(until, dt)) {
          answer("invalidAllowedUntil");
          return;
        }
        e.allowedUntilYear = dt[0];
        e.allowedUntilMonth = dt[1];
        e.allowedUntilDay = dt[2];
        e.allowedUntilHour = dt[3];
        e.allowedUntilMinute = dt[4];
        e.allowedUntilSecond = dt[5];
      }
      const std::string from_time = util::as_text(json["allowedFromTime"]);
      if (!from_time.empty()) {
        if (!util::parse_time_of_day(from_time, tod)) {
          answer("invalidAllowedFromTime");
          return;
        }
        e.allowedFromTimeHour = tod[0];
        e.allowedFromTimeMin = tod[1];
      }
      const std::string until_time = util::as_text(json["allowedUntilTime"]);
      if (!until_time.empty()) {
        if (!util::parse_time_of_day(until_time, tod)) {
          answer("invalidAllowedUntilTime");
          return;
        }
        e.allowedUntilTimeHour = tod[0];
        e.allowedUntilTimeMin = tod[1];
      }
      if (json["allowedWeekdays"].is<JsonVariantConst>()) {
        e.allowedWeekdays = util::weekdays_from_json(json["allowedWeekdays"]);
      }
    }
    op.name = "update authorization";
    op.step = [this, e] { return this->ble()->update_authorization_entry(e); };
  }
  op.done = [this, request_id, action, received_at](CmdResult result) {
    this->answer_command_("/authorization", request_id, action, result_to_string(result), result == CmdResult::Failed,
                          received_at);
    this->want_auth_ = true;
  };
  this->enqueue(std::move(op));
}

// --------------------------------------------------------------------------------------------
// Activity log
// --------------------------------------------------------------------------------------------

bool NukiHubComponent::log_marker_ready_() const {
#ifdef USE_MQTT
  // Wait for the retained marker of the previous run, so entries it covers aren't sent again
  if (this->log_marker_known_) {
    return true;
  }
  return this->mqtt_was_connected_ && (int32_t) (millis() - this->log_marker_deadline_) >= 0;
#else
  return true;
#endif
}

void NukiHubComponent::publish_log_marker_() { this->publish_int_("/lastRollingLog", this->log_marker_); }

// --------------------------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------------------------

namespace util {

void date_time(char *out, int year, int month, int day, int hour, int minute, int second) {
  snprintf(out, 20, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned) year % 10000, (unsigned) month % 100,
           (unsigned) day % 100, (unsigned) hour % 100, (unsigned) minute % 100, (unsigned) second % 100);
}

void time_of_day(char *out, int hour, int minute) {
  snprintf(out, 6, "%02u:%02u", (unsigned) hour % 100, (unsigned) minute % 100);
}

static const char *const WEEKDAYS[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

void weekdays_to_json(uint8_t weekdays, JsonArray array) {
  for (int i = 0; i < 7; i++) {
    if (weekdays & (0x40 >> i)) {
      array.add(WEEKDAYS[i]);
    }
  }
}

uint8_t weekdays_from_json(JsonVariantConst value) {
  std::string text;
  if (value.is<JsonArrayConst>()) {
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      text += as_text(item);
      text += ' ';
    }
  } else {
    text = as_text(value);
  }
  uint8_t weekdays = 0;
  for (int i = 0; i < 7; i++) {
    if (text.find(WEEKDAYS[i]) != std::string::npos) {
      weekdays |= (0x40 >> i);
    }
  }
  return weekdays;
}

std::string fixed_string(const uint8_t *data, size_t size) {
  size_t len = 0;
  while (len < size && data[len] != 0) {
    len++;
  }
  return std::string(reinterpret_cast<const char *>(data), len);
}

static bool digits(const std::string &text, size_t pos, size_t count, unsigned *out) {
  unsigned value = 0;
  for (size_t i = pos; i < pos + count; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    value = value * 10 + (text[i] - '0');
  }
  *out = value;
  return true;
}

bool parse_date_time(const std::string &text, uint16_t *out) {
  unsigned v[6];
  if (text.size() != 19 || !digits(text, 0, 4, &v[0]) || !digits(text, 5, 2, &v[1]) || !digits(text, 8, 2, &v[2]) ||
      !digits(text, 11, 2, &v[3]) || !digits(text, 14, 2, &v[4]) || !digits(text, 17, 2, &v[5])) {
    return false;
  }
  if (v[0] < 2000 || v[0] > 3000 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 ||
      v[5] > 59) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    out[i] = v[i];
  }
  return true;
}

bool parse_time_of_day(const std::string &text, uint8_t *out) {
  unsigned hour, minute;
  if (text.size() != 5 || !digits(text, 0, 2, &hour) || !digits(text, 3, 2, &minute) || hour > 23 || minute > 59) {
    return false;
  }
  out[0] = hour;
  out[1] = minute;
  return true;
}

std::string as_text(JsonVariantConst value) {
  if (value.isNull()) {
    return "";
  }
  if (value.is<const char *>()) {
    return value.as<const char *>();
  }
  std::string out;
  serializeJson(value, out);
  return out;
}

std::string request_id(const std::string &payload) {
  if (payload.find("requestId") == std::string::npos) {
    return "";
  }
  JsonDocument json;
  if (deserializeJson(json, payload)) {
    return "";
  }
  return as_text(json["requestId"]);
}

bool is_number(const std::string &text) {
  if (text.empty() || text.size() > 10) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace util

}  // namespace esphome::nuki_hub
