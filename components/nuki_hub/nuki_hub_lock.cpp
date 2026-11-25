#include "nuki_hub_lock.h"
#include "nuki_hub_names.h"

#include "esphome/core/hal.h"
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
  Operation op;
  op.name = "lock action";
  auto started = std::make_shared<bool>(false);
  op.step = [this, action, started] {
    if (!*started) {
      // Only what the lock pushes during this action counts as its outcome
      *started = true;
      this->nuki_lock_.clear_pushed_key_turner_state();
    }
    CmdResult result = this->nuki_lock_.lock_action(action, 1, 0);
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
    return true;
  }
  this->queue_.insert(this->queue_.begin() + position, std::move(op));
  return true;
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
