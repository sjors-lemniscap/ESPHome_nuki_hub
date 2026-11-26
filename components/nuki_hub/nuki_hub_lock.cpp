#include "nuki_hub_lock.h"
#include "nuki_hub_names.h"

#include "esphome/core/hal.h"
#include "esphome/core/defines.h"
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif
#include "esphome/core/log.h"
#include "nuki_lock_utils.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::nuki_hub {


static const char *const TAG = "nuki_hub.lock";

static const Name LOCK_ACTIONS[] = {
    {0x01, "Unlock"},  {0x02, "Lock"},   {0x03, "Unlatch"},    {0x04, "LockNgo"},    {0x05, "LockNgoUnlatch"},
    {0x06, "FullLock"}, {0x81, "FobAction1"}, {0x82, "FobAction2"}, {0x83, "FobAction3"},
};
static const Name FOB_ACTIONS[] = {{0, "No Action"}, {1, "Unlock"}, {2, "Lock"}, {3, "Lock n Go"}, {4, "Intelligent"}};
static const Name BUTTON_PRESS_ACTIONS[] = {
    {0, "No Action"}, {1, "Intelligent"}, {2, "Unlock"}, {3, "Lock"}, {4, "Unlatch"}, {5, "Lock n Go"},
    {6, "Show Status"},
};
static const Name MOTOR_SPEEDS[] = {{0, "Standard"}, {1, "Insane"}, {2, "Gentle"}};

template<typename T, typename F> static std::string text_of(F to_string, T value) {
  char buf[50] = {0};
  to_string(value, buf);
  return buf;
}

void NukiHubLock::setup_device() {
  // Several settings written in a row share one read of the config
  this->nuki_lock_.set_config_cache_ttl(10000);
#ifdef USE_BINARY_SENSOR
  if (this->door_sensor_input_ != nullptr) {
    // Every state, the first one included
    this->door_sensor_input_->add_full_state_callback([this](optional<bool> previous, optional<bool> current) {
      if (current.has_value()) {
        this->door_report_open_ = *current;
        this->door_report_open_owed_ |= *current;
        this->door_report_pending_ = true;
        // A new door event goes out now, not after the delay of a retry
        this->door_report_failing_since_ = 0;
        this->cancel_timeout("door_report_retry");
      }
    });
    if (this->door_sensor_input_->has_state()) {
      this->door_report_open_ = this->door_sensor_input_->state;
      this->door_report_pending_ = true;
    }
  }
#endif
}

// --------------------------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------------------------

Operation NukiHubLock::make_state_operation() {
  Operation op;
  op.name = "query lock state";
  op.step = [this] { return this->nuki_lock_.request_key_turner_state(&this->state_); };
  op.done = [this](CmdResult result) {
    this->publish_("/query/lockstateCommandResult", result_to_string(result));
    if (result != CmdResult::Success) {
      this->want_state_ = true;
      this->next_op_time_ = millis() + 1000;
      return;
    }
    this->state_known_ = true;
    this->publish_state_();
    const LockState lock_state = this->state_.lockState;
    if (lock_state == LockState::Locking || lock_state == LockState::Unlocking ||
        lock_state == LockState::Unlatching || lock_state == LockState::Calibration) {
      // In transition, look again shortly
      this->set_timeout("state_follow_up", 2000, [this] { this->want_state_ = true; });
    }
    if (!this->pin_checked_ && this->security_pin_ != 0) {
      this->want_pin_check_ = true;
    }
  };
  return op;
}

void NukiHubLock::publish_state_() {
  const KeyTurnerState &s = this->state_;
#ifdef USE_BINARY_SENSOR
  // The lock shows another door state than the sensor has (a report that failed for good, a
  // lock that restarted): send it again, at most once a minute so a lock that does not take
  // reports is not asked forever.
  if (this->door_sensor_input_ != nullptr && this->door_sensor_input_->has_state() && !this->door_report_pending_ &&
      !this->door_report_queued_ && this->door_report_failing_since_ == 0 &&
      (s.doorSensorState == DoorSensorState::DoorOpened || s.doorSensorState == DoorSensorState::DoorClosed) &&
      this->door_report_open_ != (s.doorSensorState == DoorSensorState::DoorOpened)) {
    const uint32_t now = millis();
    if (now - this->door_report_accepted_at_ > 2000 &&
        (this->door_report_resent_at_ == 0 || now - this->door_report_resent_at_ > 60000)) {
      ESP_LOGI(TAG, "Door sensor: the lock has the door %s, reporting %s again",
               this->door_report_open_ ? "closed" : "open", this->door_report_open_ ? "open" : "closed");
      this->door_report_resent_at_ = now | 1;
      this->door_report_pending_ = true;
    }
  }
#endif
  const std::string lock_state = text_of(lock_state_to_string, s.lockState);
  const std::string trigger = text_of(trigger_to_string, s.trigger);
  const std::string last_action = text_of(lock_action_to_string, s.lastLockAction);
  const std::string completion = text_of(completion_status_to_string, s.lastLockActionCompletionStatus);
  const std::string door_sensor = text_of(door_sensor_state_to_string, s.doorSensorState);

  ESP_LOGI(TAG, "Lock state: %s, trigger %s, battery %u%%", lock_state.c_str(), trigger.c_str(),
           this->nuki_lock_.get_battery_perc());

  this->publish_("/state", lock_state);
  this->publish_("/trigger", trigger);
  this->publish_("/lastLockAction", last_action);
  this->publish_("/completionStatus", completion);
  this->publish_("/doorSensorState", door_sensor);

  const char *ha_state = nullptr;
  const char *binary = nullptr;
  switch (s.lockState) {
    case LockState::Locked: ha_state = "locked", binary = "locked"; break;
    case LockState::Locking: ha_state = "locking", binary = "locked"; break;
    case LockState::Unlocking: ha_state = "unlocking", binary = "unlocked"; break;
    case LockState::Unlocked:
    case LockState::UnlockedLnga: ha_state = "unlocked", binary = "unlocked"; break;
    case LockState::Unlatched: ha_state = "open", binary = "unlocked"; break;
    case LockState::Unlatching: ha_state = "opening", binary = "unlocked"; break;
    case LockState::Uncalibrated:
    case LockState::Calibration:
    case LockState::BootRun:
    case LockState::MotorBlocked: ha_state = "jammed"; break;
    default: break;
  }
  if (ha_state != nullptr) {
    this->publish_("/hastate", ha_state);
  }
  if (binary != nullptr) {
    this->publish_("/binaryState", binary);
  }

  JsonDocument json(json_memory());
  char time[20];
  json["lock_state"] = lock_state;
  json["lockngo_state"] = s.lockNgoTimer != 255 ? s.lockNgoTimer : 0;
  json["trigger"] = trigger;
  util::date_time(time, s.currentTimeYear, s.currentTimeMonth, s.currentTimeDay, s.currentTimeHour,
                  s.currentTimeMinute, s.currentTimeSecond);
  json["currentTime"] = time;
  json["timeZoneOffset"] = s.timeZoneOffset;
  json["nightModeActive"] = s.nightModeActive != 255 ? s.nightModeActive : 0;
  json["last_lock_action"] = last_action;
  json["last_lock_action_trigger"] = text_of(trigger_to_string, s.lastLockActionTrigger);
  json["lock_completion_status"] = completion;
  json["door_sensor_state"] = door_sensor;
  if (s.remoteAccessStatus != 255) {
    json["remoteAccessEnabled"] = (s.remoteAccessStatus & 1) ? 1 : 0;
    json["bridgePaired"] = ((s.remoteAccessStatus >> 1) & 1) ? 1 : 0;
    json["sseConnectedViaWifi"] = ((s.remoteAccessStatus >> 2) & 1) ? 1 : 0;
    json["sseConnectionEstablished"] = ((s.remoteAccessStatus >> 3) & 1) ? 1 : 0;
    json["isSseConnectedViaThread"] = ((s.remoteAccessStatus >> 4) & 1) ? 1 : 0;
  }
  if (s.wifiConnectionStrength != 1) {
    json["wifiConnectionStrength"] = s.wifiConnectionStrength;
  }
  json["auth_id"] = this->last_auth_id_;
  json["auth_name"] = this->last_auth_name_;
  this->publish_json_("/json", json);

  JsonDocument battery(json_memory());
  battery["level"] = this->nuki_lock_.get_battery_perc();
  battery["critical"] = this->nuki_lock_.is_battery_critical() ? "1" : "0";
  battery["charging"] = this->nuki_lock_.is_battery_charging() ? "1" : "0";
  battery["keypadCritical"] = this->nuki_lock_.is_keypad_battery_critical() ? "1" : "0";
  battery["doorSensorCritical"] = this->nuki_lock_.is_door_sensor_battery_critical() ? "1" : "0";
  this->publish_json_("/battery/basicJson", battery);

  this->publish_int_("/rssi", this->nuki_lock_.get_rssi());
  this->state_callback_.call();
}

void NukiHubLock::publish_unavailable() { this->state_known_ = false; }

// The names of the time zones the lock knows, made once and kept
static const std::vector<const char *> &timezone_names() {
  static std::vector<const char *> names;
  if (names.empty()) {
    for (uint16_t id = 0; id < 128; id++) {
      char name[40] = {0};
      timezone_to_string((TimeZoneId) id, name);
      if (name[0] != 0 && strcmp(name, "None") != 0 && (uint16_t) timezone_to_enum(name) == id) {
        names.push_back(strdup(name));
      }
    }
  }
  return names;
}

std::vector<NukiHubComponent::DiscoverySetting> NukiHubLock::discovery_settings() const {
  return {
      {"ledEnabled", "LED enabled", false, 0, 0},
      {"buttonEnabled", "Button enabled", false, 0, 0},
      {"autoUnlatch", "Auto unlatch", false, 0, 0},
      {"singleLock", "Single lock", false, 0, 0},
      {"ledBrightness", "LED brightness", false, 0, 5},
      {"autoLockEnabled", "Auto lock", true, 0, 0},
      {"immediateAutoLockEnabled", "Immediate auto lock", true, 0, 0},
      {"nightModeEnabled", "Night mode", true, 0, 0},
      {"autoLockTimeOut", "Auto lock timeout", true, 30, 1800},
      {"unlatchDuration", "Unlatch duration", true, 1, 30},
      {"fobAction1", "Fob action 1", false, 0, 0, names_of(FOB_ACTIONS)},
      {"fobAction2", "Fob action 2", false, 0, 0, names_of(FOB_ACTIONS)},
      {"fobAction3", "Fob action 3", false, 0, 0, names_of(FOB_ACTIONS)},
      {"advertisingMode", "Advertising mode", false, 0, 0, names_of(ADVERTISING_MODES)},
      {"singleButtonPressAction", "Single button press", true, 0, 0, names_of(BUTTON_PRESS_ACTIONS)},
      {"doubleButtonPressAction", "Double button press", true, 0, 0, names_of(BUTTON_PRESS_ACTIONS)},
      {"batteryType", "Battery type", true, 0, 0, names_of(BATTERY_TYPES)},
      {"motorSpeed", "Motor speed", true, 0, 0, names_of(MOTOR_SPEEDS)},
      {"pairingEnabled", "Bluetooth pairing", false, 0, 0},
      {"loggingEnabled", "Activity log", false, 0, 0},
      {"dstMode", "Daylight saving time", false, 0, 0},
      {"timeZoneOffset", "Time zone offset", false, 0, 60},
      {"timeZone", "Time zone", false, 0, 0, timezone_names()},
      {"autoUnLockDisabled", "Auto unlock disabled", true, 0, 0},
      {"nightModeAutoLockEnabled", "Night mode auto lock", true, 0, 0},
      {"nightModeAutoUnlockDisabled", "Night mode auto unlock disabled", true, 0, 0},
      {"nightModeImmediateLockOnStart", "Night mode lock at start", true, 0, 0},
      {"autoUpdateEnabled", "Automatic updates", true, 0, 0},
      {"automaticBatteryTypeDetection", "Automatic battery type detection", true, 0, 0},
      {"enableSlowSpeedDuringNightMode", "Slow speed during night mode", true, 0, 0},
      {"detachedCylinder", "Detached cylinder", true, 0, 0},
      {"lockNgoTimeout", "Lock 'n' Go timeout", true, 5, 60},
      {"unlockedPositionOffsetDegrees", "Unlocked position offset", true, -90, 180},
      {"lockedPositionOffsetDegrees", "Locked position offset", true, -180, 90},
      {"singleLockedPositionOffsetDegrees", "Single locked position offset", true, -180, 180},
      {"unlockedToLockedTransitionOffsetDegrees", "Unlocked to locked transition offset", true, -180, 180},
  };
}

std::string NukiHubLock::state_text() const {
  return this->state_known_ ? text_of(lock_state_to_string, this->state_.lockState) : "undefined";
}

void NukiHubLock::describe_for_discovery(JsonObject lock) {
  lock["payload_lock"] = "lock";
  lock["payload_unlock"] = "unlock";
  lock["payload_open"] = "unlatch";
}

lock::LockState NukiHubLock::entity_state() const {
  if (!this->state_known_) {
    return lock::LOCK_STATE_NONE;
  }
  switch (this->state_.lockState) {
    case LockState::Locked: return lock::LOCK_STATE_LOCKED;
    case LockState::Locking: return lock::LOCK_STATE_LOCKING;
    case LockState::Unlocking:
    case LockState::Unlatching: return lock::LOCK_STATE_UNLOCKING;
    case LockState::Unlocked:
    case LockState::UnlockedLnga:
    case LockState::Unlatched: return lock::LOCK_STATE_UNLOCKED;
    case LockState::Undefined: return lock::LOCK_STATE_NONE;
    default: return lock::LOCK_STATE_JAMMED;
  }
}

// --------------------------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------------------------

bool NukiHubLock::queue_action(const std::string &text, const std::string &request_id, uint8_t timeout) {
  const uint32_t received_at = millis();
  uint8_t value;
  if (!action_of(LOCK_ACTIONS, text, &value)) {
    return false;
  }
  const LockAction action = (LockAction) value;
  // Options are for this action only, and the lock's own MQTT has no place for them
  const uint8_t flags = this->action_flags_;
  const std::string suffix = std::move(this->action_suffix_);
  this->action_flags_ = 0;
  this->action_suffix_.clear();
  if (flags == 0 && suffix.empty() && this->hybrid_send_action_(action, text, request_id, received_at)) {
    return true;
  }
  this->queue_ble_action_(action, text, request_id, received_at, flags, suffix);
  return true;
}

void NukiHubLock::queue_ble_action_(LockAction action, const std::string &text, const std::string &request_id,
                                    uint32_t received_at, uint8_t flags, const std::string &suffix) {
  Operation op;
  op.name = "lock action";
  auto started = std::make_shared<bool>(false);
  op.step = [this, action, started, flags, suffix] {
    if (!*started) {
      // Only what the lock pushes during this action counts as its outcome
      *started = true;
      this->nuki_lock_.clear_pushed_key_turner_state();
    }
    CmdResult result = this->nuki_lock_.lock_action(action, 1, flags, suffix.empty() ? nullptr : suffix.c_str(),
                                                    (uint8_t) suffix.size());
    if (result != CmdResult::Working) {
      this->publish_("/commandResult", result_to_string(result));
    }
    return result;
  };
  op.done = [this, text, request_id, received_at](CmdResult result) {
    this->publish_result_("/actionResult", request_id, text, result_to_string(result), result == CmdResult::Failed,
                          received_at);
    if (result == CmdResult::Success) {
      KeyTurnerState pushed;
      if (this->nuki_lock_.take_pushed_key_turner_state(&pushed)) {
        this->state_ = pushed;
        this->state_known_ = true;
        this->publish_state_();
      }
      // Some devices don't answer a state request on the connection the action ran on: what
      // follows gets a connection of its own
      this->nuki_lock_.disconnect();
      this->set_timeout("state_follow_up", 1000, [this] { this->want_state_ = true; });
      this->log_refresh_at_ = millis() + 1500;
    } else {
      ESP_LOGW(TAG, "Lock: Maximum number of retries exceeded, aborting.");
      this->want_state_ = true;
    }
  };
  size_t position = 0;
  while (position < this->queue_.size() && strcmp(this->queue_[position].name, "lock action") == 0) {
    position++;
  }
  if (this->queue_.size() >= MAX_QUEUE) {
    this->publish_("/commandResult", "busy");
    this->publish_result_("/actionResult", request_id, text, "busy", false, received_at);
    return;
  }
  this->queue_.insert(this->queue_.begin() + position, std::move(op));
}

// --------------------------------------------------------------------------------------------
// Hybrid mode
//
// A lock with Wi-Fi or Thread can publish its state to the broker itself (Nuki's official MQTT
// API, topics "nuki/<Nuki ID in hex>/..."). Like Nuki Hub (HYBRID.md) the component listens
// there: a state change arrives at once and without a Bluetooth connection, also when paired
// as app, where the advertisement does not announce changes. What those topics do not carry
// is still read over Bluetooth once the lock has reached a final state. Lock actions can go
// out over the same way ("lockAction", needs "Allow locking" in the Nuki app) and are sent
// over Bluetooth if the lock does not confirm them in time.
// --------------------------------------------------------------------------------------------

void NukiHubLock::loop() {
  NukiHubComponent::loop();
#ifdef USE_BINARY_SENSOR
  // One report at a time: a change while one is on its way is sent after it, with the state
  // the door has then
  if (this->door_report_pending_ && !this->door_report_queued_ && this->is_paired()) {
    this->door_report_pending_ = false;
    this->door_report_queued_ = true;
    const bool open = this->door_report_open_owed_ || this->door_report_open_;
    Operation op;
    op.name = "report door sensor";
    op.step = [this, open] { return this->nuki_lock_.report_door_sensor_state(open); };
    op.done = [this, open](CmdResult result) {
      this->door_report_queued_ = false;
      if (result == CmdResult::Success) {
        ESP_LOGI(TAG, "Door sensor: reported %s", open ? "open" : "closed");
        this->door_report_failing_since_ = 0;
        this->door_report_accepted_at_ = millis();
        if (open) {
          this->door_report_open_owed_ = false;
          // Closed again while the "open" was on its way
          this->door_report_pending_ |= !this->door_report_open_;
        }
        this->request_state_update();
        return;
      }
      // A busy lock (motor running) leaves a report unanswered: again after a second, then
      // every three, for half a minute. After that the next state read shows the difference
      // and sends it once more, see publish_state_().
      if (this->door_report_failing_since_ == 0) {
        this->door_report_failing_since_ = millis() | 1;
      }
      const uint32_t failing = millis() - this->door_report_failing_since_;
      if (failing < 30000) {
        ESP_LOGW(TAG, "Door sensor: report of %s failed (%s), trying again", open ? "open" : "closed",
                 result_to_string(result));
        this->set_timeout("door_report_retry", failing < 1000 ? 1000 : 3000,
                          [this] { this->door_report_pending_ = true; });
      } else {
        ESP_LOGW(TAG, "Door sensor: report of %s failed (%s), giving up", open ? "open" : "closed",
                 result_to_string(result));
        this->door_report_failing_since_ = 0;
        this->door_report_open_owed_ = false;
      }
    };
    // Before queued commands: the lock holds the latch back until it hears the door open
    this->enqueue(std::move(op), true);
  }
#endif
  if (!this->hybrid_) {
    return;
  }
  if (!this->hybrid_subscribed_) {
    this->hybrid_subscribe_();
  }
  const uint32_t now = millis();
  if (this->hybrid_action_pending_ && now - this->hybrid_action_sent_at_ > this->hybrid_fallback_timeout_) {
    this->hybrid_action_failed_("not confirmed in time");
  }
  // Without the lock's own messages, and paired as app, nothing announces a change: look now
  // and then
  if (this->hybrid_subscribed_ && !this->hybrid_connected_ && this->hybrid_offline_interval_ > 0 &&
      now - this->hybrid_last_offline_update_ > this->hybrid_offline_interval_) {
    this->hybrid_last_offline_update_ = now;
    ESP_LOGD(TAG, "Official MQTT offline, reading the state over Bluetooth");
    this->want_state_ = true;
  }
}

void NukiHubLock::hybrid_subscribe_() {
#ifdef USE_MQTT
  if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    return;
  }
  if (this->hybrid_prefix_.empty()) {
    // The path the lock uses by itself: "nuki/" and its id in upper case hexadecimal, known
    // once the configuration has been read
    if (!this->config_valid_) {
      return;
    }
    char prefix[sizeof("nuki/FFFFFFFF")];
    snprintf(prefix, sizeof(prefix), "nuki/%" PRIX32, this->config_.nukiId);
    this->hybrid_prefix_ = prefix;
  }
  ESP_LOGI(TAG, "Hybrid mode: listening on %s/...", this->hybrid_prefix_.c_str());
  // ESPHome keeps the subscription and renews it after a reconnect. The lock publishes
  // retained, so the current values arrive right away.
  const size_t skip = this->hybrid_prefix_.size() + 1;
  mqtt::global_mqtt_client->subscribe(
      this->hybrid_prefix_ + "/+",
      [this, skip](const std::string &topic, const std::string &payload) {
        if (topic.size() > skip) {
          this->hybrid_on_message_(topic.c_str() + skip, payload);
        }
      },
      1);
  this->hybrid_subscribed_ = true;
  this->hybrid_last_offline_update_ = millis();
#endif
}

void NukiHubLock::hybrid_set_connected_(bool connected) {
  if (connected != this->hybrid_connected_) {
    ESP_LOGI(TAG, "Official MQTT of the lock is %s", connected ? "online" : "offline");
  }
  this->hybrid_connected_ = connected;
  this->publish_("/hybridConnected", connected ? "1" : "0");
  if (!connected) {
    this->hybrid_last_offline_update_ = millis();
    if (this->hybrid_action_pending_) {
      this->hybrid_action_failed_("lost the lock's connection");
    }
  }
}

void NukiHubLock::hybrid_on_message_(const char *key, const std::string &payload) {
  const optional<int> value = parse_number<int>(payload);
  ESP_LOGV(TAG, "Official MQTT %s: %s", key, payload.c_str());

  if (strcmp(key, "connected") == 0) {
    this->hybrid_set_connected_(payload == "true");
  } else if (strcmp(key, "state") == 0) {
    if (!value.has_value()) {
      return;
    }
    const LockState state = (LockState) *value;
    const bool moving = state == LockState::Locking || state == LockState::Unlocking ||
                        state == LockState::Unlatching || state == LockState::Calibration;
    if (!this->state_known_ || this->state_.lockState != state) {
      if (!this->state_known_) {
        this->state_ = KeyTurnerState{};
        this->state_.doorSensorState = DoorSensorState::Unavailable;
      }
      this->state_.lockState = state;
      this->state_known_ = true;
      this->publish_state_();
    }
    if (this->hybrid_action_pending_ && moving) {
      this->hybrid_action_confirmed_();
    }
    // The topic has the state alone: battery, trigger and the rest come over Bluetooth once
    // the lock stands still
    if (!moving) {
      this->cancel_timeout("state_follow_up");
      this->want_state_ = true;
      this->log_refresh_at_ = millis() + 1500;
    }
  } else if (strcmp(key, "commandResponse") == 0) {
    if (!value.has_value() || !this->hybrid_action_pending_) {
      return;
    }
    if (*value == 0) {
      this->hybrid_action_confirmed_();
    } else {
      ESP_LOGW(TAG, "Lock action over official MQTT refused (%d)", *value);
      this->hybrid_action_failed_("refused");
    }
  } else if (strcmp(key, "lockActionEvent") == 0) {
    // "<lock action>,<trigger>,<auth id>,<code id>,<context>"
    unsigned int action = 0, trigger = 0;
    if (sscanf(payload.c_str(), "%u,%u", &action, &trigger) < 2) {
      return;
    }
    if (this->hybrid_action_pending_ && action == (unsigned int) this->hybrid_pending_action_) {
      this->hybrid_action_confirmed_();
    }
  } else if (strcmp(key, "doorsensorState") == 0) {
    if (!value.has_value() || !this->state_known_) {
      return;
    }
    const DoorSensorState door = (DoorSensorState) *value;
    if (this->state_.doorSensorState != door) {
      this->state_.doorSensorState = door;
      this->publish_state_();
    }
  }
}

bool NukiHubLock::hybrid_send_action_(LockAction action, const std::string &text, const std::string &request_id,
                                      uint32_t received_at) {
#ifdef USE_MQTT
  if (!this->hybrid_ || !this->hybrid_send_actions_ || !this->hybrid_connected_ || this->hybrid_action_pending_ ||
      mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    return false;
  }
  // The topic takes the plain lock actions; fob actions are Bluetooth only
  if ((uint8_t) action < 1 || (uint8_t) action > 6) {
    return false;
  }
  char payload[4];
  const int length = snprintf(payload, sizeof(payload), "%u", (unsigned int) action);
  const std::string topic = this->hybrid_prefix_ + "/lockAction";
  if (!mqtt::global_mqtt_client->publish(topic.c_str(), payload, length, 1, false)) {
    ESP_LOGW(TAG, "Publishing the lock action failed, using Bluetooth");
    return false;
  }
  ESP_LOGI(TAG, "Lock action %s sent over official MQTT", text.c_str());
  this->hybrid_action_pending_ = true;
  this->hybrid_pending_action_ = action;
  this->hybrid_pending_text_ = text;
  this->hybrid_pending_request_id_ = request_id;
  this->hybrid_action_received_at_ = received_at;
  this->hybrid_action_sent_at_ = millis();
  return true;
#else
  return false;
#endif
}

void NukiHubLock::hybrid_action_confirmed_() {
  this->hybrid_action_pending_ = false;
  this->publish_("/commandResult", "success");
  this->publish_result_("/actionResult", this->hybrid_pending_request_id_, this->hybrid_pending_text_, "success",
                        false, this->hybrid_action_received_at_);
}

void NukiHubLock::hybrid_action_failed_(const char *reason) {
  this->hybrid_action_pending_ = false;
  if (this->hybrid_ble_fallback_) {
    ESP_LOGW(TAG, "Lock action %s over official MQTT %s, sending it over Bluetooth",
             this->hybrid_pending_text_.c_str(), reason);
    this->queue_ble_action_(this->hybrid_pending_action_, this->hybrid_pending_text_,
                            this->hybrid_pending_request_id_, this->hybrid_action_received_at_);
    return;
  }
  ESP_LOGW(TAG, "Lock action %s over official MQTT %s", this->hybrid_pending_text_.c_str(), reason);
  const char *result = strcmp(reason, "refused") == 0 ? "failed" : "timeOut";
  this->publish_("/commandResult", result);
  this->publish_result_("/actionResult", this->hybrid_pending_request_id_, this->hybrid_pending_text_, result,
                        strcmp(reason, "refused") == 0, this->hybrid_action_received_at_);
  this->want_state_ = true;
}

// --------------------------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------------------------

Operation NukiHubLock::make_config_operation() {
  Operation op;
  op.name = "query lock config";
  this->config_stage_ = 0;
  op.step = [this] {
    if (this->config_stage_ == 0) {
      CmdResult result = this->nuki_lock_.request_config(&this->config_);
      if (result != CmdResult::Success) {
        return result;
      }
      this->config_valid_ = true;
      this->publish_config_();
      this->config_stage_ = 1;
    }
    CmdResult result = this->nuki_lock_.request_advanced_config(&this->advanced_config_);
    if (result == CmdResult::Success) {
      this->advanced_config_valid_ = true;
      this->publish_advanced_config_();
    }
    return result;
  };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      ESP_LOGW(TAG, "Invalid/Unexpected lock config and/or advanced config received, retrying in 10 seconds");
      this->set_timeout("config_retry", 10000, [this] { this->want_config_ = true; });
    }
  };
  return op;
}

void NukiHubLock::publish_config_() {
  const Config &c = this->config_;
  JsonDocument json(json_memory());
  char buf[40];
  snprintf(buf, sizeof(buf), "%" PRIx32, c.nukiId);
  json["nukiID"] = buf;
  json["name"] = util::fixed_string(c.name, sizeof(c.name));
  json["latitude"] = c.latitude;
  json["longitude"] = c.longitude;
  json["autoUnlatch"] = c.autoUnlatch;
  json["pairingEnabled"] = c.pairing_enabled;
  if (this->nuki_lock_.is_logging_known()) {
    // Told with the activity log, so only known once that was read (needs the PIN)
    json["loggingEnabled"] = this->nuki_lock_.get_logging_enabled() ? 1 : 0;
  }
  json["buttonEnabled"] = c.buttonEnabled;
  json["ledEnabled"] = c.ledEnabled;
  json["ledBrightness"] = c.ledBrightness;
  util::date_time(buf, c.currentTimeYear, c.currentTimeMonth, c.currentTimeDay, c.currentTimeHour, c.currentTimeMinute,
                  c.currentTimeSecond);
  json["currentTime"] = buf;
  json["timeZoneOffset"] = c.timeZoneOffset;
  json["dstMode"] = c.dstMode;
  json["hasFob"] = c.hasFob;
  json["fobAction1"] = name_of(FOB_ACTIONS, c.fobAction1, "undefined");
  json["fobAction2"] = name_of(FOB_ACTIONS, c.fobAction2, "undefined");
  json["fobAction3"] = name_of(FOB_ACTIONS, c.fobAction3, "undefined");
  json["singleLock"] = c.singleLock;
  json["advertisingMode"] = name_of(ADVERTISING_MODES, (uint8_t) c.advertisingMode, "undefined");
  json["hasKeypad"] = c.hasKeypad == 255 ? 0 : c.hasKeypad;
  json["hasKeypadV2"] = c.hasKeypadV2 == 255 ? 0 : c.hasKeypadV2;
  snprintf(buf, sizeof(buf), "%d.%d.%d", c.firmwareVersion[0], c.firmwareVersion[1], c.firmwareVersion[2]);
  json["firmwareVersion"] = buf;
  this->publish_("/info/firmwareVersion", buf);
  snprintf(buf, sizeof(buf), "%d.%d", c.hardwareRevision[0], c.hardwareRevision[1]);
  json["hardwareRevision"] = buf;
  this->publish_("/info/hardwareVersion", buf);
  json["homeKitStatus"] = text_of(homekit_status_to_string, (int) c.homeKitStatus);
  json["timeZone"] = text_of(timezone_to_string, c.timeZoneId);
  if (c.deviceType != 255) {
    json["deviceType"] = c.deviceType;
  }
  if (c.capabilities != 255) {
    json["wifiCapable"] = c.capabilities & 1;
    json["threadCapable"] = (c.capabilities >> 1) & 1;
  }
  if (c.matterStatus != 255) {
    json["matterStatus"] = c.matterStatus;
  }
  this->publish_json_("/configuration/basicJson", json);
  char firmware[16];
  snprintf(firmware, sizeof(firmware), "%d.%d.%d", c.firmwareVersion[0], c.firmwareVersion[1], c.firmwareVersion[2]);
  this->publish_discovery_(c.nukiId, util::fixed_string(c.name, sizeof(c.name)), firmware);
  ESP_LOGI(TAG, "Lock config read: firmware %d.%d.%d, keypad %s", c.firmwareVersion[0], c.firmwareVersion[1],
           c.firmwareVersion[2], YESNO(this->has_keypad()));
}

void NukiHubLock::publish_advanced_config_() {
  const AdvancedConfig &c = this->advanced_config_;
  JsonDocument json(json_memory());
  char tod[6];
  json["totalDegrees"] = c.totalDegrees;
  json["unlockedPositionOffsetDegrees"] = c.unlockedPositionOffsetDegrees;
  json["lockedPositionOffsetDegrees"] = c.lockedPositionOffsetDegrees;
  json["singleLockedPositionOffsetDegrees"] = c.singleLockedPositionOffsetDegrees;
  json["unlockedToLockedTransitionOffsetDegrees"] = c.unlockedToLockedTransitionOffsetDegrees;
  json["lockNgoTimeout"] = c.lockNgoTimeout;
  json["singleButtonPressAction"] = name_of(BUTTON_PRESS_ACTIONS, (uint8_t) c.singleButtonPressAction, "undefined");
  json["doubleButtonPressAction"] = name_of(BUTTON_PRESS_ACTIONS, (uint8_t) c.doubleButtonPressAction, "undefined");
  json["detachedCylinder"] = c.detachedCylinder;
  json["batteryType"] = name_of(BATTERY_TYPES, (uint8_t) c.batteryType, "undefined");
  json["automaticBatteryTypeDetection"] = c.automaticBatteryTypeDetection;
  json["unlatchDuration"] = c.unlatchDuration;
  json["autoLockTimeOut"] = c.autoLockTimeOut;
  json["autoUnLockDisabled"] = c.autoUnLockDisabled;
  json["nightModeEnabled"] = c.nightModeEnabled;
  util::time_of_day(tod, c.nightModeStartTime[0], c.nightModeStartTime[1]);
  json["nightModeStartTime"] = tod;
  util::time_of_day(tod, c.nightModeEndTime[0], c.nightModeEndTime[1]);
  json["nightModeEndTime"] = tod;
  json["nightModeAutoLockEnabled"] = c.nightModeAutoLockEnabled;
  json["nightModeAutoUnlockDisabled"] = c.nightModeAutoUnlockDisabled;
  json["nightModeImmediateLockOnStart"] = c.nightModeImmediateLockOnStart;
  json["autoLockEnabled"] = c.autoLockEnabled;
  json["immediateAutoLockEnabled"] = c.immediateAutoLockEnabled;
  json["autoUpdateEnabled"] = c.autoUpdateEnabled;
  if (c.motorSpeed != MotorSpeed::Unknown) {
    json["motorSpeed"] = name_of(MOTOR_SPEEDS, (uint8_t) c.motorSpeed, "undefined");
    json["enableSlowSpeedDuringNightMode"] = c.enable_slow_speed_during_night_mode;
  }
  json["rebootNuki"] = 0;
  json["recalibrateNuki"] = 0;
  this->publish_json_("/configuration/advancedJson", json);
}

void NukiHubLock::handle_config_action(const std::string &payload) {
  JsonDocument result(json_memory());
  auto answer = [this, &result] { this->publish_json_("/configuration/commandResult", result); };

  if (!this->config_valid_ || !this->advanced_config_valid_) {
    result["general"] = "configNotReady";
    answer();
    return;
  }
  if (!this->pin_valid_) {
    result["general"] = "noValidPinSet";
    answer();
    return;
  }
  JsonDocument json(json_memory());
  if (deserializeJson(json, payload) || !json.is<JsonObjectConst>()) {
    result["general"] = "invalidJson";
    answer();
    return;
  }

  // Each accepted setting becomes one write
  struct Write {
    std::string key;
    std::function<CmdResult()> step;
  };
  std::vector<Write> writes;
  NukiLock *lock = &this->nuki_lock_;
  const Config &c = this->config_;
  const AdvancedConfig &a = this->advanced_config_;

  for (JsonPairConst pair : json.as<JsonObjectConst>()) {
    const std::string key = pair.key().c_str();
    const std::string text = util::as_text(pair.value());
    if (key == "requestId") {
      result[key] = text;
      continue;
    }
    if (text.empty()) {
      result[key] = "noValueSet";
      continue;
    }
    const bool negative = text[0] == '-' && util::is_number(text.substr(1));
    const bool numeric = negative || util::is_number(text);
    const long number = numeric ? strtol(text.c_str(), nullptr, 10) : 0;

    auto number_in = [&](long current, long min, long max, std::function<CmdResult(long)> setter) {
      if (!numeric || number < min || number > max) {
        result[key] = "invalidValue";
      } else if (number == current) {
        result[key] = "unchanged";
      } else {
        const long value = number;
        writes.push_back({key, [setter, value] { return setter(value); }});
      }
    };
    auto named = [&](const Name *table, size_t size, uint8_t current, std::function<CmdResult(uint8_t)> setter) {
      for (size_t i = 0; i < size; i++) {
        if (text == table[i].text) {
          if (table[i].value == current) {
            result[key] = "unchanged";
          } else {
            const uint8_t value = table[i].value;
            writes.push_back({key, [setter, value] { return setter(value); }});
          }
          return;
        }
      }
      result[key] = "invalidValue";
    };
    auto time_of_day = [&](const unsigned char *current, std::function<CmdResult(unsigned char *)> setter) {
      uint8_t tod[2];
      if (!util::parse_time_of_day(text, tod)) {
        result[key] = "invalidValue";
      } else if (tod[0] == current[0] && tod[1] == current[1]) {
        result[key] = "unchanged";
      } else {
        const uint8_t hour = tod[0], minute = tod[1];
        writes.push_back({key, [setter, hour, minute] {
                            unsigned char value[2] = {hour, minute};
                            return setter(value);
                          }});
      }
    };
#define NAMED(table, current, setter) named(table, sizeof(table) / sizeof(table[0]), (uint8_t) (current), setter)

    if (key == "name") {
      if (text.size() > 32) {
        result[key] = "valueTooLong";
      } else if (text == util::fixed_string(c.name, sizeof(c.name))) {
        result[key] = "unchanged";
      } else {
        writes.push_back({key, [lock, text] { return lock->set_name(text); }});
      }
    } else if (key == "latitude" || key == "longitude") {
      char *end = nullptr;
      const float value = strtof(text.c_str(), &end);
      const bool latitude = key == "latitude";
      if (end == text.c_str() || value < -180 || value > 180) {
        result[key] = "invalidValue";
      } else if (value == (latitude ? c.latitude : c.longitude)) {
        result[key] = "unchanged";
      } else {
        writes.push_back({key, [lock, latitude, value] {
                            return latitude ? lock->set_latitude(value) : lock->set_longitude(value);
                          }});
      }
    } else if (key == "autoUnlatch") {
      number_in(c.autoUnlatch, 0, 1, [lock](long v) { return lock->enable_auto_unlatch(v); });
    } else if (key == "loggingEnabled") {
      number_in(lock->is_logging_known() ? lock->get_logging_enabled() : -1, 0, 1, [lock](long v) { return lock->enable_logging(v); });
    } else if (key == "pairingEnabled") {
      number_in(c.pairing_enabled, 0, 1, [lock](long v) { return lock->enable_pairing(v); });
    } else if (key == "buttonEnabled") {
      number_in(c.buttonEnabled, 0, 1, [lock](long v) { return lock->enable_button(v); });
    } else if (key == "ledEnabled") {
      number_in(c.ledEnabled, 0, 1, [lock](long v) { return lock->enable_led_flash(v); });
    } else if (key == "ledBrightness") {
      number_in(c.ledBrightness, 0, 5, [lock](long v) { return lock->set_led_brightness(v); });
    } else if (key == "timeZoneOffset") {
      number_in(c.timeZoneOffset, 0, 60, [lock](long v) { return lock->set_time_zone_offset(v); });
    } else if (key == "dstMode") {
      number_in(c.dstMode, 0, 1, [lock](long v) { return lock->enable_dst(v); });
    } else if (key == "fobAction1") {
      NAMED(FOB_ACTIONS, c.fobAction1, [lock](uint8_t v) { return lock->set_fob_action(1, v); });
    } else if (key == "fobAction2") {
      NAMED(FOB_ACTIONS, c.fobAction2, [lock](uint8_t v) { return lock->set_fob_action(2, v); });
    } else if (key == "fobAction3") {
      NAMED(FOB_ACTIONS, c.fobAction3, [lock](uint8_t v) { return lock->set_fob_action(3, v); });
    } else if (key == "singleLock") {
      number_in(c.singleLock, 0, 1, [lock](long v) { return lock->enable_single_lock(v); });
    } else if (key == "advertisingMode") {
      NAMED(ADVERTISING_MODES, c.advertisingMode,
            [lock](uint8_t v) { return lock->set_advertising_mode((AdvertisingMode) v); });
    } else if (key == "timeZone") {
      const TimeZoneId zone = timezone_to_enum(text.c_str());
      if (text != text_of(timezone_to_string, zone)) {
        result[key] = "invalidValue";
      } else if (zone == c.timeZoneId) {
        result[key] = "unchanged";
      } else {
        writes.push_back({key, [lock, zone] { return lock->set_time_zone_id(zone); }});
      }
    } else if (key == "unlockedPositionOffsetDegrees") {
      number_in(a.unlockedPositionOffsetDegrees, -90, 180,
                [lock](long v) { return lock->set_unlocked_position_offset_degrees(v); });
    } else if (key == "lockedPositionOffsetDegrees") {
      number_in(a.lockedPositionOffsetDegrees, -180, 90,
                [lock](long v) { return lock->set_locked_position_offset_degrees(v); });
    } else if (key == "singleLockedPositionOffsetDegrees") {
      number_in(a.singleLockedPositionOffsetDegrees, -180, 180,
                [lock](long v) { return lock->set_single_locked_position_offset_degrees(v); });
    } else if (key == "unlockedToLockedTransitionOffsetDegrees") {
      number_in(a.unlockedToLockedTransitionOffsetDegrees, -180, 180,
                [lock](long v) { return lock->set_unlocked_to_locked_transition_offset_degrees(v); });
    } else if (key == "lockNgoTimeout") {
      number_in(a.lockNgoTimeout, 5, 60, [lock](long v) { return lock->set_lock_ngo_timeout(v); });
    } else if (key == "singleButtonPressAction") {
      NAMED(BUTTON_PRESS_ACTIONS, a.singleButtonPressAction,
            [lock](uint8_t v) { return lock->set_single_button_press_action((ButtonPressAction) v); });
    } else if (key == "doubleButtonPressAction") {
      NAMED(BUTTON_PRESS_ACTIONS, a.doubleButtonPressAction,
            [lock](uint8_t v) { return lock->set_double_button_press_action((ButtonPressAction) v); });
    } else if (key == "detachedCylinder") {
      number_in(a.detachedCylinder, 0, 1, [lock](long v) { return lock->enable_detached_cylinder(v); });
    } else if (key == "batteryType") {
      NAMED(BATTERY_TYPES, a.batteryType, [lock](uint8_t v) { return lock->set_battery_type((BatteryType) v); });
    } else if (key == "automaticBatteryTypeDetection") {
      number_in(a.automaticBatteryTypeDetection, 0, 1,
                [lock](long v) { return lock->enable_auto_battery_type_detection(v); });
    } else if (key == "unlatchDuration") {
      number_in(a.unlatchDuration, 1, 30, [lock](long v) { return lock->set_unlatch_duration(v); });
    } else if (key == "autoLockTimeOut") {
      number_in(a.autoLockTimeOut, 30, 1800, [lock](long v) { return lock->set_auto_lock_time_out(v); });
    } else if (key == "autoUnLockDisabled") {
      number_in(a.autoUnLockDisabled, 0, 1, [lock](long v) { return lock->disable_auto_unlock(v); });
    } else if (key == "nightModeEnabled") {
      number_in(a.nightModeEnabled, 0, 1, [lock](long v) { return lock->enable_night_mode(v); });
    } else if (key == "nightModeStartTime") {
      time_of_day(a.nightModeStartTime, [lock](unsigned char *v) { return lock->set_night_mode_start_time(v); });
    } else if (key == "nightModeEndTime") {
      time_of_day(a.nightModeEndTime, [lock](unsigned char *v) { return lock->set_night_mode_end_time(v); });
    } else if (key == "nightModeAutoLockEnabled") {
      number_in(a.nightModeAutoLockEnabled, 0, 1, [lock](long v) { return lock->enable_night_mode_auto_lock(v); });
    } else if (key == "nightModeAutoUnlockDisabled") {
      number_in(a.nightModeAutoUnlockDisabled, 0, 1,
                [lock](long v) { return lock->disable_night_mode_auto_unlock(v); });
    } else if (key == "nightModeImmediateLockOnStart") {
      number_in(a.nightModeImmediateLockOnStart, 0, 1,
                [lock](long v) { return lock->enable_night_mode_immediate_lock_on_start(v); });
    } else if (key == "autoLockEnabled") {
      number_in(a.autoLockEnabled, 0, 1, [lock](long v) { return lock->enable_auto_lock(v); });
    } else if (key == "immediateAutoLockEnabled") {
      number_in(a.immediateAutoLockEnabled, 0, 1, [lock](long v) { return lock->enable_immediate_auto_lock(v); });
    } else if (key == "autoUpdateEnabled") {
      number_in(a.autoUpdateEnabled, 0, 1, [lock](long v) { return lock->enable_auto_update(v); });
    } else if (key == "motorSpeed") {
      NAMED(MOTOR_SPEEDS, a.motorSpeed, [lock](uint8_t v) { return lock->set_motor_speed((MotorSpeed) v); });
    } else if (key == "enableSlowSpeedDuringNightMode") {
      number_in(a.enable_slow_speed_during_night_mode, 0, 1,
                [lock](long v) { return lock->enable_slow_speed_during_night_mode(v); });
    } else if (key == "rebootNuki") {
      if (numeric && number == 1) {
        writes.push_back({key, [lock] { return lock->request_reboot(); }});
      } else {
        result[key] = numeric && number == 0 ? "unchanged" : "invalidValue";
      }
    } else if (key == "recalibrateNuki") {
      if (numeric && number == 1) {
        writes.push_back({key, [lock] { return lock->request_calibration(); }});
      } else {
        result[key] = numeric && number == 0 ? "unchanged" : "invalidValue";
      }
    } else {
      result[key] = "invalidSetting";
    }
#undef NAMED
  }

  if (writes.empty()) {
    result["general"] = "noChange";
    answer();
    return;
  }

  // Results known so far travel with the writes, the last one publishes them all
  auto pending = std::make_shared<JsonDocument>(result);
  auto remaining = std::make_shared<size_t>(writes.size());
  auto changed = std::make_shared<bool>(false);
  for (auto &write : writes) {
    Operation op;
    op.name = "write lock setting";
    op.step = std::move(write.step);
    const std::string key = write.key;
    op.done = [this, key, pending, remaining, changed](CmdResult r) {
      (*pending)[key] = result_to_string(r);
      *changed = *changed || r == CmdResult::Success;
      if (--(*remaining) > 0) {
        return;
      }
      (*pending)["general"] = *changed ? "success" : "noChange";
      this->publish_json_("/configuration/commandResult", *pending);
      // The next command is compared with what the lock reports, not with what was true before
      this->want_config_ = true;
      this->enqueue(this->make_config_operation(), true);
    };
    this->enqueue(std::move(op));
  }
}

// --------------------------------------------------------------------------------------------
// Battery
// --------------------------------------------------------------------------------------------

Operation NukiHubLock::make_battery_operation() {
  Operation op;
  op.name = "query lock battery";
  op.step = [this] { return this->nuki_lock_.request_battery_report(&this->battery_report_); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    const BatteryReport &b = this->battery_report_;
    JsonDocument json(json_memory());
    json["batteryDrain"] = b.batteryDrain;
    json["batteryVoltage"] = b.batteryVoltage / 1000.0;
    json["critical"] = (b.criticalBatteryState & 1) ? 1 : 0;
    json["lockAction"] = text_of(lock_action_to_string, b.lock_action);
    json["startVoltage"] = b.startVoltage / 1000.0;
    json["lowestVoltage"] = b.lowestVoltage / 1000.0;
    json["lockDistance"] = b.lockDistance;
    json["startTemperature"] = b.startTemperature;
    json["maxTurnCurrent"] = b.maxTurnCurrent / 1000.0;
    json["batteryResistance"] = b.batteryResistance / 1000.0;
    this->publish_json_("/battery/advancedJson", json);
  };
  return op;
}

// --------------------------------------------------------------------------------------------
// Lists the lock keeps sending after its first answer
// --------------------------------------------------------------------------------------------

CmdResult NukiHubLock::settle_list_(CmdResult result, size_t size) {
  if (!this->list_settling_) {
    if (result != CmdResult::Success) {
      return result;
    }
    this->list_settling_ = true;
    this->list_last_size_ = size;
    this->list_last_change_ = millis();
    return CmdResult::Working;
  }
  if (size != this->list_last_size_) {
    this->list_last_size_ = size;
    this->list_last_change_ = millis();
  }
  if (millis() - this->list_last_change_ < 1000) {
    return CmdResult::Working;
  }
  this->list_settling_ = false;
  return CmdResult::Success;
}

// --------------------------------------------------------------------------------------------
// Time control
// --------------------------------------------------------------------------------------------

Operation NukiHubLock::make_time_control_operation() {
  Operation op;
  op.name = "read time control entries";
  this->list_settling_ = false;
  op.step = [this] {
    CmdResult result = this->list_settling_ ? CmdResult::Success : this->nuki_lock_.retrieve_time_control_entries();
    if (result == CmdResult::Working) {
      return result;
    }
    return this->settle_list_(result, this->nuki_lock_.get_time_control_entry_list_size());
  };
  op.done = [this](CmdResult result) {
    this->list_settling_ = false;
    if (result != CmdResult::Success) {
      return;
    }
    this->nuki_lock_.get_time_control_entries(&this->time_control_entries_);
    std::sort(this->time_control_entries_.begin(), this->time_control_entries_.end(),
              [](const TimeControlEntry &a, const TimeControlEntry &b) { return a.entryId < b.entryId; });
    this->publish_time_control_();
    std::vector<std::pair<uint32_t, std::string>> switches;
    char label[40];
    for (const auto &entry : this->time_control_entries_) {
      snprintf(label, sizeof(label), "Time control %u - %02u:%02u", entry.entryId, entry.timeHour, entry.timeMin);
      switches.emplace_back(entry.entryId, label);
    }
    this->publish_time_control_switches_(switches);
  };
  return op;
}

void NukiHubLock::publish_time_control_() {
  JsonDocument doc(json_memory());
  JsonArray list = doc.to<JsonArray>();
  char tod[6];
  for (const auto &entry : this->time_control_entries_) {
    JsonObject json = list.add<JsonObject>();
    json["entryId"] = entry.entryId;
    json["enabled"] = entry.enabled;
    util::weekdays_to_json(entry.weekdays, json["weekdays"].to<JsonArray>());
    util::time_of_day(tod, entry.timeHour, entry.timeMin);
    json["time"] = tod;
    json["lockAction"] = text_of(lock_action_to_string, entry.lock_action);
  }
  this->publish_json_("/timecontrol/json", doc);
}

void NukiHubLock::handle_time_control_action(const std::string &payload) {
  const std::string request_id = util::request_id(payload);
  const uint32_t received_at = millis();
  auto answer = [this, &request_id, received_at](const char *text) {
    this->answer_command_("/timecontrol", request_id, "", text, false, received_at);
  };

  if (!this->pin_valid_) {
    answer(this->security_pin_ == 0 ? "noPinSet" : "noValidPinSet");
    return;
  }
  JsonDocument json(json_memory());
  if (deserializeJson(json, payload) || !json.is<JsonObjectConst>()) {
    answer("invalidJson");
    return;
  }
  if (!json["action"].is<const char *>()) {
    answer("noActionSet");
    return;
  }
  const std::string action = json["action"].as<const char *>();
  const uint8_t entry_id = json["entryId"].as<unsigned int>();
  if (action != "add" && action != "update" && action != "delete") {
    answer("invalidAction");
    return;
  }

  const TimeControlEntry *existing = nullptr;
  for (const auto &entry : this->time_control_entries_) {
    if (entry.entryId == entry_id) {
      existing = &entry;
    }
  }
  if (action != "add") {
    if (!json["entryId"].is<JsonVariantConst>()) {
      answer("noEntryIdSet");
      return;
    }
    if (existing == nullptr) {
      answer("noExistingEntryIdSet");
      return;
    }
  }

  Operation op;
  if (action == "delete") {
    op.name = "delete time control entry";
    op.step = [this, entry_id] { return this->nuki_lock_.remove_time_control_entry(entry_id); };
  } else {
    TimeControlEntry entry{};
    if (existing != nullptr) {
      entry = *existing;
    } else {
      entry.enabled = 1;
    }
    entry.entryId = entry_id;

    const std::string time = util::as_text(json["time"]);
    uint8_t tod[2];
    if (!time.empty()) {
      if (!util::parse_time_of_day(time, tod)) {
        answer("invalidTime");
        return;
      }
      entry.timeHour = tod[0];
      entry.timeMin = tod[1];
    } else if (action == "add") {
      answer("invalidTime");
      return;
    }
    const std::string lock_action = util::as_text(json["lockAction"]);
    if (!lock_action.empty()) {
      uint8_t value;
      if (!action_of(LOCK_ACTIONS, lock_action, &value)) {
        answer("invalidLockAction");
        return;
      }
      entry.lock_action = (LockAction) value;
    } else if (action == "add") {
      answer("invalidLockAction");
      return;
    }
    if (json["weekdays"].is<JsonVariantConst>()) {
      entry.weekdays = util::weekdays_from_json(json["weekdays"]);
    }
    if (json["enabled"].is<JsonVariantConst>()) {
      entry.enabled = json["enabled"].as<unsigned int>() ? 1 : 0;
    }

    if (action == "add") {
      NewTimeControlEntry fresh{entry.weekdays, entry.timeHour, entry.timeMin, entry.lock_action};
      op.name = "add time control entry";
      op.step = [this, fresh] { return this->nuki_lock_.add_time_control_entry(fresh); };
    } else {
      op.name = "update time control entry";
      op.step = [this, entry] { return this->nuki_lock_.update_time_control_entry(entry); };
    }
  }
  op.done = [this, request_id, action, received_at](CmdResult result) {
    this->answer_command_("/timecontrol", request_id, action, result_to_string(result), result == CmdResult::Failed,
                          received_at);
    this->want_time_control_ = true;
  };
  this->enqueue(std::move(op));
}

// --------------------------------------------------------------------------------------------
// Activity log
// --------------------------------------------------------------------------------------------

// Reads the newest entries, and whatever lies between them and the last published entry, so
// that every entry is published once and in order.
Operation NukiHubLock::make_log_operation() {
  Operation op;
  op.name = "read activity log";
  this->log_stage_ = 0;
  this->log_batch_.clear();
  this->list_settling_ = false;
  op.step = [this] {
    if (this->log_stage_ == 0) {
      CmdResult result = this->list_settling_
                             ? CmdResult::Success
                             : this->nuki_lock_.retrieve_log_entries(0, this->max_log_entries_, 1, false);
      if (result == CmdResult::Working) {
        return result;
      }
      result = this->settle_list_(result, this->nuki_lock_.get_log_entry_list_size());
      if (result != CmdResult::Success) {
        return result;
      }
      this->nuki_lock_.get_log_entries(&this->log_batch_);
      uint32_t oldest = UINT32_MAX;
      for (const auto &entry : this->log_batch_) {
        oldest = std::min<uint32_t>(oldest, entry.index);
      }
      if (!this->log_marker_known_ || this->log_batch_.empty() || oldest <= this->log_marker_ + 1) {
        return CmdResult::Success;
      }
      // Entries between the marker and this batch are missing, at most 100 are fetched. An
      // Opener (firmware 1.12.4) answers an ascending request with the entries after the
      // start index, not from it on: ask from one entry earlier and for one more, which
      // covers both readings. What comes twice is dropped below.
      const uint32_t missing = std::min<uint32_t>(oldest - this->log_marker_ - 1, 100);
      this->log_fill_count_ = missing + 1;
      this->log_fill_start_ = oldest - missing - 1;
      this->log_stage_ = 1;
    }
    CmdResult result = this->list_settling_ ? CmdResult::Success
                                            : this->nuki_lock_.retrieve_log_entries(this->log_fill_start_,
                                                                                    this->log_fill_count_, 0, false);
    if (result == CmdResult::Working) {
      return result;
    }
    result = this->settle_list_(result, this->nuki_lock_.get_log_entry_list_size());
    if (result == CmdResult::Working) {
      return result;
    }
    if (result == CmdResult::Success) {
      std::vector<LogEntry> missing;
      this->nuki_lock_.get_log_entries(&missing);
      for (const auto &entry : missing) {
        const bool known = std::any_of(this->log_batch_.begin(), this->log_batch_.end(),
                                       [&entry](const LogEntry &e) { return e.index == entry.index; });
        if (!known) {
          this->log_batch_.push_back(entry);
        }
      }
    }
    // The newest entries are in hand either way
    return CmdResult::Success;
  };
  op.done = [this](CmdResult result) {
    this->list_settling_ = false;
    if (result == CmdResult::Success) {
      this->process_log_();
    }
  };
  return op;
}

void NukiHubLock::queue_log_range(uint32_t from, uint32_t to, const std::string &request_id) {
  const uint32_t received_at = millis();
  Operation op;
  op.name = "read log range";
  auto started = std::make_shared<bool>(false);
  op.step = [this, from, to, started] {
    if (!*started) {
      *started = true;
      this->list_settling_ = false;
    }
    CmdResult result = this->list_settling_
                           ? CmdResult::Success
                           : this->nuki_lock_.retrieve_log_entries(from - 1, to - from + 2, 0, false);
    if (result == CmdResult::Working) {
      return result;
    }
    return this->settle_list_(result, this->nuki_lock_.get_log_entry_list_size());
  };
  op.done = [this, from, to, request_id, received_at](CmdResult result) {
    this->list_settling_ = false;
    std::vector<LogEntry> entries;
    if (result == CmdResult::Success) {
      this->nuki_lock_.get_log_entries(&entries);
      std::sort(entries.begin(), entries.end(), [](const LogEntry &a, const LogEntry &b) { return a.index < b.index; });
    }
    this->publish_result_("/logRange", request_id, "", result_to_string(result), result == CmdResult::Failed,
                          received_at, [&](JsonObject json) {
                            json["from"] = from;
                            json["to"] = to;
                            JsonArray list = json["entries"].to<JsonArray>();
                            for (const auto &entry : entries) {
                              if (entry.index >= from && entry.index <= to) {
                                this->log_entry_to_json_(entry, list.add<JsonObject>());
                              }
                            }
                          });
  };
  this->enqueue(std::move(op));
}

void NukiHubLock::log_entry_to_json_(const LogEntry &log, JsonObject entry) {
  std::string name = util::fixed_string(log.name, sizeof(log.name));
  if (name.empty() || name.size() == sizeof(log.name)) {
    const char *known = this->auth_name_(log.authId);
    if (known != nullptr) {
      name = known;
    }
  }
  entry["index"] = (uint32_t) log.index;
  entry["authorizationId"] = (uint32_t) log.authId;
  entry["authorizationName"] = name;
  entry["timeYear"] = log.timeStampYear;
  entry["timeMonth"] = log.timeStampMonth;
  entry["timeDay"] = log.timeStampDay;
  entry["timeHour"] = log.timeStampHour;
  entry["timeMinute"] = log.timeStampMinute;
  entry["timeSecond"] = log.timeStampSecond;
  entry["type"] = text_of(logging_type_to_string, log.loggingType);

  switch (log.loggingType) {
    case LoggingType::LockAction:
      entry["action"] = text_of(lock_action_to_string, (LockAction) log.data[0]);
      entry["trigger"] = text_of(trigger_to_string, (NukiTrigger) log.data[1]);
      entry["completionStatus"] = text_of(completion_status_to_string, (CompletionStatus) log.data[3]);
      break;
    case LoggingType::KeypadAction:
      entry["action"] = text_of(lock_action_to_string, (LockAction) log.data[0]);
      entry["trigger"] = name_of(KEYPAD_TRIGGERS, log.data[1], "Unknown");
      if (log.data[2] == (uint8_t) LogEntryCompletionStatus::NotAuthorized) {
        entry["completionStatus"] = "notAuthorized";
      } else if (log.data[2] == (uint8_t) LogEntryCompletionStatus::InvalidCode) {
        entry["completionStatus"] = "invalidCode";
      } else if (log.data[2] == (uint8_t) LogEntryCompletionStatus::InvalidFingerprint) {
        entry["completionStatus"] = "invalidFingerprint";
      } else {
        entry["completionStatus"] = text_of(completion_status_to_string, (CompletionStatus) log.data[2]);
      }
      entry["codeId"] = 256U * log.data[4] + log.data[3];
      break;
    case LoggingType::DoorSensor:
      switch (log.data[0]) {
        case 0: entry["action"] = "DoorOpened"; break;
        case 1: entry["action"] = "DoorClosed"; break;
        case 2: entry["action"] = "SensorJammed"; break;
        default: entry["action"] = "Unknown"; break;
      }
      break;
    default:
      break;
  }
}

void NukiHubLock::process_log_() {
  std::sort(this->log_batch_.begin(), this->log_batch_.end(),
            [](const LogEntry &a, const LogEntry &b) { return a.index < b.index; });
  if (this->log_batch_.empty()) {
    return;
  }

  if (!this->log_marker_known_) {
    // No marker from an earlier run: what is in the log now counts as published
    this->log_marker_ = this->log_batch_.back().index;
    this->log_marker_known_ = true;
    this->publish_log_marker_();
  }

  JsonDocument doc(json_memory());
  JsonArray list = doc.to<JsonArray>();
  const size_t first_listed =
      this->log_batch_.size() > this->max_log_entries_ ? this->log_batch_.size() - this->max_log_entries_ : 0;

  for (size_t i = 0; i < this->log_batch_.size(); i++) {
    const LogEntry &log = this->log_batch_[i];
    JsonDocument single(json_memory());
    JsonObject entry = single.to<JsonObject>();
    this->log_entry_to_json_(log, entry);

    if (log.loggingType == LoggingType::LockAction || log.loggingType == LoggingType::KeypadAction) {
      this->last_auth_id_ = log.authId;
      this->last_auth_name_ = entry["authorizationName"].as<std::string>();
    }
    if (i >= first_listed) {
      list.add(entry);
    }
    if (log.index > this->log_marker_) {
      this->publish_json_("/rollingLog", single, false);
      this->log_marker_ = log.index;
      this->publish_log_marker_();
      if (log.loggingType == LoggingType::KeypadAction) {
        this->publish_event_("keypad", entry);
      } else if (log.loggingType == LoggingType::LockAction) {
        this->publish_event_("action", entry);
      }
    }
  }

  this->publish_json_("/log", doc);
  this->publish_json_("/shortLog", doc);
  if (this->last_auth_id_ != 0xFFFFFFFF) {
    this->publish_int_("/authorizationId", this->last_auth_id_);
    this->publish_("/authorizationName", this->last_auth_name_);
  }
  this->update_entities_();
  this->log_batch_.clear();
}

}  // namespace esphome::nuki_hub
