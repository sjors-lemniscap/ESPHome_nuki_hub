#include "nuki_hub_opener.h"
#include "nuki_hub_names.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/nuki_lock/nuki_lock_utils.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::nuki_hub {

using namespace esphome::nuki_lock;

static const char *const TAG = "nuki_hub.opener";

// --------------------------------------------------------------------------------------------
// Names, as Nuki Hub publishes and accepts them
// --------------------------------------------------------------------------------------------

static const Name LOCK_ACTIONS[] = {
    {0x01, "ActivateRTO"}, {0x02, "DeactivateRTO"}, {0x03, "ElectricStrikeActuation"}, {0x04, "ActivateCM"},
    {0x05, "DeactivateCM"}, {0x81, "FobAction1"},   {0x82, "FobAction2"},              {0x83, "FobAction3"},
};
static const Name LOCK_STATES[] = {
    {0x00, "uncalibrated"}, {0x01, "locked"}, {0x03, "RTOactive"}, {0x05, "open"}, {0x07, "opening"},
};
static const Name TRIGGERS[] = {{0x00, "system"}, {0x01, "manual"}, {0x02, "button"}, {0x03, "automatic"}};
static const Name COMPLETION[] = {
    {0x00, "success"}, {0x02, "canceled"}, {0x03, "tooRecent"}, {0x04, "busy"},
    {0x08, "incomplete"}, {0xFE, "otherError"}, {0xFF, "unknown"},
};
static const Name LOGGING_TYPES[] = {
    {0x01, "LoggingEnabled"}, {0x02, "LockAction"}, {0x03, "Calibration"}, {0x05, "KeypadAction"},
    {0x06, "DoorbellRecognition"},
};
static const Name FOB_ACTIONS[] = {
    {0, "No Action"}, {1, "Toggle RTO"}, {2, "Activate RTO"}, {3, "Deactivate RTO"}, {7, "Open"}, {8, "Ring"},
};
static const Name CAPABILITIES[] = {{0, "Door opener"}, {1, "Both"}, {2, "RTO"}};
static const Name OPERATING_MODES[] = {
    {0, "Generic door opener"}, {1, "Analogue intercom"}, {2, "Digital intercom"}, {3, "Siedle"},
    {4, "TCS"},                 {5, "Bticino"},           {6, "Siedle HTS"},       {7, "STR"},
    {8, "Ritto"},               {9, "Fermax"},            {10, "Comelit"},         {11, "Urmet BiBus"},
    {12, "Urmet 2Voice"},       {13, "Golmar"},           {14, "SKS"},             {15, "Spare"},
};
static const Name DOORBELL_SUPPRESSION[] = {
    {0, "Off"}, {1, "CM"}, {2, "RTO"}, {3, "CM & RTO"}, {4, "Ring"}, {5, "CM & Ring"}, {6, "RTO & Ring"},
    {7, "CM & RTO & Ring"},
};
static const Name SOUNDS[] = {{0, "No Sound"}, {1, "Sound 1"}, {2, "Sound 2"}, {3, "Sound 3"}};
static const Name BUTTON_PRESS_ACTIONS[] = {
    {0, "No Action"}, {1, "Toggle RTO"}, {2, "Activate RTO"}, {3, "Deactivate RTO"},
    {4, "Toggle CM"}, {5, "Activate CM"}, {6, "Deactivate CM"}, {7, "Open"},
};
static const Name LOG_SOURCES[] = {
    {0, "Doorbell"}, {1, "Timecontrol"}, {2, "App"}, {3, "Button"}, {4, "Fob"}, {5, "Bridge"}, {6, "Keypad"},
};

static bool parse_lock_action(const std::string &text, opener::LockAction *action) {
  uint8_t value;
  if (!action_of(LOCK_ACTIONS, text, &value)) {
    return false;
  }
  *action = (opener::LockAction) value;
  return true;
}

// --------------------------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------------------------

Operation NukiHubOpener::make_state_operation() {
  Operation op;
  op.name = "query opener state";
  op.step = [this] { return this->nuki_opener_.request_opener_state(&this->state_); };
  op.done = [this](CmdResult result) {
    this->publish_("/query/lockstateCommandResult", result_to_string(result));
    if (result != CmdResult::Success) {
      this->want_state_ = true;
      this->next_op_time_ = millis() + 1000;
      return;
    }
    this->state_known_ = true;
    this->publish_state_();
    if (this->state_.lockState == opener::LockState::Opening || this->state_.lockState == opener::LockState::Open) {
      // In transition, look again shortly
      this->set_timeout("state_follow_up", 2000, [this] { this->want_state_ = true; });
    }
    if (!this->pin_checked_ && this->security_pin_ != 0) {
      this->want_pin_check_ = true;
    }
  };
  return op;
}

void NukiHubOpener::publish_state_() {
  const opener::OpenerState &s = this->state_;
  const bool continuous = s.nukiState == opener::State::ContinuousMode;
  const char *lock_state = name_of(LOCK_STATES, (uint8_t) s.lockState, "undefined");

  ESP_LOGI(TAG, "Opener state: %s%s, trigger %s, battery critical %s", lock_state, continuous ? " (continuous mode)" : "",
           name_of(TRIGGERS, (uint8_t) s.trigger, "undefined"), YESNO(this->nuki_opener_.is_battery_critical()));

  this->publish_("/state", lock_state);
  this->publish_("/continuousMode", continuous ? "on" : "off");
  this->publish_("/trigger", name_of(TRIGGERS, (uint8_t) s.trigger, "undefined"));
  this->publish_("/lastLockAction", name_of(LOCK_ACTIONS, (uint8_t) s.lastLockAction, "Unknown"));
  this->publish_("/completionStatus",
                 name_of(COMPLETION, (uint8_t) s.lastLockActionCompletionStatus, "undefined"));

  const char *ha_state = "jammed";
  bool unlocked = true;
  if (continuous) {
    ha_state = "unlocked";
  } else {
    switch (s.lockState) {
      case opener::LockState::Locked:
        ha_state = "locked";
        unlocked = false;
        break;
      case opener::LockState::RTOactive: ha_state = "unlocked"; break;
      case opener::LockState::Open: ha_state = "open"; break;
      case opener::LockState::Opening: ha_state = "opening"; break;
      default: unlocked = false; break;
    }
  }
  this->publish_("/hastate", ha_state);
  if (strcmp(ha_state, "jammed") != 0) {
    this->publish_("/binaryState", unlocked ? "unlocked" : "locked");
  }

  JsonDocument json;
  char time[20];
  json["lock_state"] = lock_state;
  json["continuous_mode"] = continuous ? 1 : 0;
  json["trigger"] = name_of(TRIGGERS, (uint8_t) s.trigger, "undefined");
  json["ringToOpenTimer"] = s.ringToOpenTimer != 255 ? s.ringToOpenTimer : 0;
  util::date_time(time, s.currentTimeYear, s.currentTimeMonth, s.currentTimeDay, s.currentTimeHour,
                  s.currentTimeMinute, s.currentTimeSecond);
  json["currentTime"] = time;
  json["timeZoneOffset"] = s.timeZoneOffset;
  json["last_lock_action"] = name_of(LOCK_ACTIONS, (uint8_t) s.lastLockAction, "Unknown");
  json["last_lock_action_trigger"] = name_of(TRIGGERS, (uint8_t) s.lastLockActionTrigger, "undefined");
  json["lock_completion_status"] = name_of(COMPLETION, (uint8_t) s.lastLockActionCompletionStatus, "undefined");
  json["auth_id"] = this->last_auth_id_;
  json["auth_name"] = this->last_auth_name_;
  this->publish_json_("/json", json);

  JsonDocument battery;
  battery["critical"] = this->nuki_opener_.is_battery_critical() ? "1" : "0";
  battery["keypadCritical"] = this->nuki_opener_.is_keypad_battery_critical() ? "1" : "0";
  this->publish_json_("/battery/basicJson", battery);

  this->publish_int_("/rssi", this->nuki_opener_.get_rssi());
  this->state_callback_.call();
}

void NukiHubOpener::publish_unavailable() { this->state_known_ = false; }

lock::LockState NukiHubOpener::entity_state() const {
  if (!this->state_known_) {
    return lock::LOCK_STATE_NONE;
  }
  if (this->state_.nukiState == opener::State::ContinuousMode) {
    return lock::LOCK_STATE_UNLOCKED;
  }
  switch (this->state_.lockState) {
    case opener::LockState::Locked: return lock::LOCK_STATE_LOCKED;
    case opener::LockState::RTOactive:
    case opener::LockState::Open: return lock::LOCK_STATE_UNLOCKED;
    case opener::LockState::Opening: return lock::LOCK_STATE_UNLOCKING;
    default: return lock::LOCK_STATE_JAMMED;
  }
}

void NukiHubOpener::publish_ring_(bool locked) {
  ESP_LOGI(TAG, "Ring detected (%s)", locked ? "locked" : "open");
  this->publish_("/ring", locked ? "ringlocked" : "ring");
  this->publish_("/binaryRing", "ring");
  this->set_timeout("ring_reset", 2000, [this] {
    this->publish_("/ring", "standby");
    this->publish_("/binaryRing", "standby");
  });
}

// --------------------------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------------------------

bool NukiHubOpener::queue_action(const std::string &text, const std::string &request_id) {
  const uint32_t received_at = millis();
  opener::LockAction action;
  if (!parse_lock_action(text, &action)) {
    return false;
  }
  Operation op;
  op.name = "opener action";
  auto started = std::make_shared<bool>(false);
  op.step = [this, action, started] {
    if (!*started) {
      // Only what the Opener pushes during this action counts as its outcome
      *started = true;
      this->nuki_opener_.clear_pushed_opener_state();
    }
    CmdResult result = this->nuki_opener_.lock_action(action, 0, 0);
    if (result != CmdResult::Working) {
      this->publish_("/commandResult", result_to_string(result));
    }
    return result;
  };
  op.done = [this, text, request_id, received_at](CmdResult result) {
    this->publish_result_("/actionResult", request_id, text, result_to_string(result), result == CmdResult::Failed,
                          received_at);
    if (result == CmdResult::Success) {
      opener::OpenerState pushed;
      if (this->nuki_opener_.take_pushed_opener_state(&pushed)) {
        this->state_ = pushed;
        this->state_known_ = true;
        this->publish_state_();
      }
      // The Opener doesn't answer a state request on the connection the action ran on (the
      // request times out): what follows gets a connection of its own
      this->nuki_opener_.disconnect();
      this->set_timeout("state_follow_up", 1000, [this] { this->want_state_ = true; });
      this->log_refresh_at_ = millis() + 1500;
    } else {
      ESP_LOGW(TAG, "Opener: Maximum number of retries exceeded, aborting.");
      this->want_state_ = true;
    }
  };
  // Actions go before whatever else is waiting
  size_t position = 0;
  while (position < this->queue_.size() && strcmp(this->queue_[position].name, "opener action") == 0) {
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

Operation NukiHubOpener::make_config_operation() {
  Operation op;
  op.name = "query opener config";
  this->config_stage_ = 0;
  op.step = [this] {
    if (this->config_stage_ == 0) {
      CmdResult result = this->nuki_opener_.request_config(&this->config_);
      if (result != CmdResult::Success) {
        return result;
      }
      this->config_valid_ = true;
      this->publish_config_();
      this->config_stage_ = 1;
    }
    CmdResult result = this->nuki_opener_.request_advanced_config(&this->advanced_config_);
    if (result == CmdResult::Success) {
      this->advanced_config_valid_ = true;
      this->publish_advanced_config_();
      // "complete" follows an action only after the strike was released again
      this->nuki_opener_.set_action_timeout(std::max<uint32_t>(
          15000, (uint32_t) this->advanced_config_.electricStrikeDelay +
                     this->advanced_config_.electricStrikeDuration + 5000));
    }
    return result;
  };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      ESP_LOGW(TAG, "Invalid/Unexpected opener config and/or advanced config received, retrying in 10 seconds");
      this->set_timeout("config_retry", 10000, [this] { this->want_config_ = true; });
    }
  };
  return op;
}

void NukiHubOpener::publish_config_() {
  const opener::Config &c = this->config_;
  JsonDocument json;
  char buf[40];
  snprintf(buf, sizeof(buf), "%" PRIx32, c.nukiId);
  json["nukiID"] = buf;
  json["name"] = util::fixed_string(c.name, sizeof(c.name));
  json["capabilities"] = name_of(CAPABILITIES, c.capabilities, "undefined");
  json["pairingEnabled"] = c.pairingEnabled;
  json["buttonEnabled"] = c.buttonEnabled;
  json["ledFlashEnabled"] = c.ledFlashEnabled;
  util::date_time(buf, c.currentTimeYear, c.currentTimeMonth, c.currentTimeDay, c.currentTimeHour, c.currentTimeMinute,
                  c.currentTimeSecond);
  json["currentTime"] = buf;
  json["timeZoneOffset"] = c.timeZoneOffset;
  json["dstMode"] = c.dstMode;
  json["hasFob"] = c.hasFob;
  json["fobAction1"] = name_of(FOB_ACTIONS, c.fobAction1, "undefined");
  json["fobAction2"] = name_of(FOB_ACTIONS, c.fobAction2, "undefined");
  json["fobAction3"] = name_of(FOB_ACTIONS, c.fobAction3, "undefined");
  json["operatingMode"] = name_of(OPERATING_MODES, c.operatingMode, "undefined");
  json["advertisingMode"] = name_of(ADVERTISING_MODES, (uint8_t) c.advertisingMode, "undefined");
  json["hasKeypad"] = c.hasKeypad;
  json["hasKeypadV2"] = c.hasKeypadV2 == 255 ? 0 : c.hasKeypadV2;
  snprintf(buf, sizeof(buf), "%d.%d.%d", c.firmwareVersion[0], c.firmwareVersion[1], c.firmwareVersion[2]);
  json["firmwareVersion"] = buf;
  this->publish_("/info/firmwareVersion", buf);
  snprintf(buf, sizeof(buf), "%d.%d", c.hardwareRevision[0], c.hardwareRevision[1]);
  json["hardwareRevision"] = buf;
  this->publish_("/info/hardwareVersion", buf);
  memset(buf, 0, sizeof(buf));
  timezone_to_string(c.timeZoneId, buf);
  json["timeZone"] = buf;
  this->publish_json_("/configuration/basicJson", json);
  ESP_LOGI(TAG, "Opener config read: firmware %d.%d.%d, keypad %s", c.firmwareVersion[0], c.firmwareVersion[1],
           c.firmwareVersion[2], YESNO(this->has_keypad()));
}

void NukiHubOpener::publish_advanced_config_() {
  const opener::AdvancedConfig &c = this->advanced_config_;
  JsonDocument json;
  json["intercomID"] = c.intercomID;
  json["busModeSwitch"] = c.busModeSwitch;
  json["shortCircuitDuration"] = c.shortCircuitDuration;
  json["electricStrikeDelay"] = c.electricStrikeDelay;
  json["randomElectricStrikeDelay"] = c.randomElectricStrikeDelay;
  json["electricStrikeDuration"] = c.electricStrikeDuration;
  json["disableRtoAfterRing"] = c.disableRtoAfterRing;
  json["rtoTimeout"] = c.rtoTimeout;
  json["doorbellSuppression"] = name_of(DOORBELL_SUPPRESSION, c.doorbellSuppression, "undefined");
  json["doorbellSuppressionDuration"] = c.doorbellSuppressionDuration;
  json["soundRing"] = name_of(SOUNDS, c.soundRing, "undefined");
  json["soundOpen"] = name_of(SOUNDS, c.soundOpen, "undefined");
  json["soundRto"] = name_of(SOUNDS, c.soundRto, "undefined");
  json["soundCm"] = name_of(SOUNDS, c.soundCm, "undefined");
  json["soundConfirmation"] = c.soundConfirmation;
  json["soundLevel"] = c.soundLevel;
  json["singleButtonPressAction"] = name_of(BUTTON_PRESS_ACTIONS, (uint8_t) c.singleButtonPressAction, "undefined");
  json["doubleButtonPressAction"] = name_of(BUTTON_PRESS_ACTIONS, (uint8_t) c.doubleButtonPressAction, "undefined");
  json["batteryType"] = name_of(BATTERY_TYPES, (uint8_t) c.batteryType, "undefined");
  json["automaticBatteryTypeDetection"] = c.automaticBatteryTypeDetection;
  json["rebootNuki"] = 0;
  json["recalibrateNuki"] = 0;
  this->publish_json_("/configuration/advancedJson", json);
}

// One requested change of a config value: how to validate it and how to apply it
template<typename TConfig> struct Change {
  std::string key;
  std::function<void(TConfig &)> apply;
};

void NukiHubOpener::handle_config_action(const std::string &payload) {
  JsonDocument result;
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
  JsonDocument json;
  if (deserializeJson(json, payload) || !json.is<JsonObjectConst>()) {
    result["general"] = "invalidJson";
    answer();
    return;
  }

  auto basic = std::make_shared<std::vector<Change<opener::Config>>>();
  auto advanced = std::make_shared<std::vector<Change<opener::AdvancedConfig>>>();
  auto pending = std::make_shared<JsonDocument>();
  bool reboot = false;

  const opener::Config &c = this->config_;
  const opener::AdvancedConfig &a = this->advanced_config_;

  for (JsonPairConst pair : json.as<JsonObjectConst>()) {
    const std::string key = pair.key().c_str();
    const std::string text = util::as_text(pair.value());
    if (key == "requestId") {
      result[key] = text;
      continue;
    }
    const long number = util::is_number(text) ? strtol(text.c_str(), nullptr, 10) : -1;
    if (text.empty()) {
      result[key] = "noValueSet";
      continue;
    }

    // Numeric value within [min, max]
    auto numeric = [&](long current, long min, long max, std::function<void(long)> queue) {
      const bool negative_ok = min < 0 && !text.empty() && text[0] == '-' && util::is_number(text.substr(1));
      const long value = negative_ok ? -strtol(text.c_str() + 1, nullptr, 10) : number;
      if ((number < 0 && !negative_ok) || value < min || value > max) {
        result[key] = "invalidValue";
      } else if (value == current) {
        result[key] = "unchanged";
      } else {
        queue(value);
      }
    };
    // Value named in a table
    auto named = [&](const Name *table, size_t size, uint8_t current, std::function<void(uint8_t)> queue) {
      for (size_t i = 0; i < size; i++) {
        if (text == table[i].text) {
          if (table[i].value == current) {
            result[key] = "unchanged";
          } else {
            queue(table[i].value);
          }
          return;
        }
      }
      result[key] = "invalidValue";
    };
#define BASIC(field, min, max) \
  numeric(c.field, min, max, [&, key](long v) { basic->push_back({key, [v](opener::Config &cfg) { cfg.field = v; }}); })
#define BASIC_NAMED(field, table) \
  named(table, sizeof(table) / sizeof(table[0]), (uint8_t) c.field, [&, key](uint8_t v) { \
    basic->push_back({key, [v](opener::Config &cfg) { cfg.field = (decltype(cfg.field)) v; }}); \
  })
#define ADVANCED(field, min, max) \
  numeric(a.field, min, max, \
          [&, key](long v) { advanced->push_back({key, [v](opener::AdvancedConfig &cfg) { cfg.field = v; }}); })
#define ADVANCED_NAMED(field, table) \
  named(table, sizeof(table) / sizeof(table[0]), (uint8_t) a.field, [&, key](uint8_t v) { \
    advanced->push_back({key, [v](opener::AdvancedConfig &cfg) { cfg.field = (decltype(cfg.field)) v; }}); \
  })

    if (key == "name") {
      if (text.size() > 32) {
        result[key] = "valueTooLong";
      } else if (text == util::fixed_string(c.name, sizeof(c.name))) {
        result[key] = "unchanged";
      } else {
        basic->push_back({key, [text](opener::Config &cfg) {
                            memset(cfg.name, 0, sizeof(cfg.name));
                            memcpy(cfg.name, text.c_str(), std::min(text.size(), sizeof(cfg.name)));
                          }});
      }
    } else if (key == "latitude" || key == "longitude") {
      char *end = nullptr;
      const float value = strtof(text.c_str(), &end);
      const float current = key == "latitude" ? c.latitude : c.longitude;
      if (end == text.c_str() || value < -180 || value > 180) {
        result[key] = "invalidValue";
      } else if (value == current) {
        result[key] = "unchanged";
      } else if (key == "latitude") {
        basic->push_back({key, [value](opener::Config &cfg) { cfg.latitude = value; }});
      } else {
        basic->push_back({key, [value](opener::Config &cfg) { cfg.longitude = value; }});
      }
    } else if (key == "pairingEnabled") {
      BASIC(pairingEnabled, 0, 1);
    } else if (key == "buttonEnabled") {
      BASIC(buttonEnabled, 0, 1);
    } else if (key == "ledFlashEnabled") {
      BASIC(ledFlashEnabled, 0, 1);
    } else if (key == "timeZoneOffset") {
      BASIC(timeZoneOffset, -720, 840);
    } else if (key == "dstMode") {
      BASIC(dstMode, 0, 1);
    } else if (key == "fobAction1") {
      BASIC_NAMED(fobAction1, FOB_ACTIONS);
    } else if (key == "fobAction2") {
      BASIC_NAMED(fobAction2, FOB_ACTIONS);
    } else if (key == "fobAction3") {
      BASIC_NAMED(fobAction3, FOB_ACTIONS);
    } else if (key == "operatingMode") {
      BASIC_NAMED(operatingMode, OPERATING_MODES);
    } else if (key == "advertisingMode") {
      BASIC_NAMED(advertisingMode, ADVERTISING_MODES);
    } else if (key == "timeZone") {
      const TimeZoneId zone = timezone_to_enum(text.c_str());
      char check[40] = {0};
      timezone_to_string(zone, check);
      if (text != check) {
        result[key] = "invalidValue";
      } else if (zone == c.timeZoneId) {
        result[key] = "unchanged";
      } else {
        basic->push_back({key, [zone](opener::Config &cfg) { cfg.timeZoneId = zone; }});
      }
    } else if (key == "intercomID") {
      ADVANCED(intercomID, 0, 65535);
    } else if (key == "busModeSwitch") {
      ADVANCED(busModeSwitch, 0, 1);
    } else if (key == "shortCircuitDuration") {
      ADVANCED(shortCircuitDuration, 0, 65535);
    } else if (key == "electricStrikeDelay") {
      ADVANCED(electricStrikeDelay, 0, 30000);
    } else if (key == "randomElectricStrikeDelay") {
      ADVANCED(randomElectricStrikeDelay, 0, 1);
    } else if (key == "electricStrikeDuration") {
      ADVANCED(electricStrikeDuration, 1000, 30000);
    } else if (key == "disableRtoAfterRing") {
      ADVANCED(disableRtoAfterRing, 0, 1);
    } else if (key == "rtoTimeout") {
      ADVANCED(rtoTimeout, 5, 60);
    } else if (key == "doorbellSuppression") {
      ADVANCED_NAMED(doorbellSuppression, DOORBELL_SUPPRESSION);
    } else if (key == "doorbellSuppressionDuration") {
      ADVANCED(doorbellSuppressionDuration, 500, 10000);
    } else if (key == "soundRing") {
      ADVANCED_NAMED(soundRing, SOUNDS);
    } else if (key == "soundOpen") {
      ADVANCED_NAMED(soundOpen, SOUNDS);
    } else if (key == "soundRto") {
      ADVANCED_NAMED(soundRto, SOUNDS);
    } else if (key == "soundCm") {
      ADVANCED_NAMED(soundCm, SOUNDS);
    } else if (key == "soundConfirmation") {
      ADVANCED(soundConfirmation, 0, 1);
    } else if (key == "soundLevel") {
      ADVANCED(soundLevel, 0, 255);
    } else if (key == "singleButtonPressAction") {
      ADVANCED_NAMED(singleButtonPressAction, BUTTON_PRESS_ACTIONS);
    } else if (key == "doubleButtonPressAction") {
      ADVANCED_NAMED(doubleButtonPressAction, BUTTON_PRESS_ACTIONS);
    } else if (key == "batteryType") {
      ADVANCED_NAMED(batteryType, BATTERY_TYPES);
    } else if (key == "automaticBatteryTypeDetection") {
      ADVANCED(automaticBatteryTypeDetection, 0, 1);
    } else if (key == "rebootNuki") {
      if (number == 1) {
        reboot = true;
      } else {
        result[key] = number == 0 ? "unchanged" : "invalidValue";
      }
    } else {
      result[key] = "invalidSetting";
    }
#undef BASIC
#undef BASIC_NAMED
#undef ADVANCED
#undef ADVANCED_NAMED
  }

  if (basic->empty() && advanced->empty() && !reboot) {
    result["general"] = "noChange";
    answer();
    return;
  }

  // Results known so far travel with the operations, the last one publishes them all
  *pending = result;
  auto remaining = std::make_shared<int>((basic->empty() ? 0 : 1) + (advanced->empty() ? 0 : 1) + (reboot ? 1 : 0));
  auto changed = std::make_shared<bool>(false);
  auto finish = [this, pending, remaining, changed] {
    if (--(*remaining) > 0) {
      return;
    }
    (*pending)["general"] = *changed ? "success" : "noChange";
    this->publish_json_("/configuration/commandResult", *pending);
    this->want_config_ = true;
  };

  if (!basic->empty()) {
    Operation op;
    op.name = "write opener config";
    op.step = [this, basic] {
      return this->nuki_opener_.write_config([basic](opener::Config &cfg) {
        for (const auto &change : *basic) {
          change.apply(cfg);
        }
      });
    };
    op.done = [basic, pending, changed, finish](CmdResult r) {
      for (const auto &change : *basic) {
        (*pending)[change.key] = result_to_string(r);
      }
      *changed = *changed || r == CmdResult::Success;
      finish();
    };
    this->enqueue(std::move(op));
  }
  if (!advanced->empty()) {
    Operation op;
    op.name = "write opener advanced config";
    op.step = [this, advanced] {
      return this->nuki_opener_.write_advanced_config([advanced](opener::AdvancedConfig &cfg) {
        for (const auto &change : *advanced) {
          change.apply(cfg);
        }
      });
    };
    op.done = [advanced, pending, changed, finish](CmdResult r) {
      for (const auto &change : *advanced) {
        (*pending)[change.key] = result_to_string(r);
      }
      *changed = *changed || r == CmdResult::Success;
      finish();
    };
    this->enqueue(std::move(op));
  }
  if (reboot) {
    Operation op;
    op.name = "reboot opener";
    op.step = [this] { return this->nuki_opener_.request_reboot(); };
    op.done = [pending, changed, finish](CmdResult r) {
      (*pending)["rebootNuki"] = result_to_string(r);
      *changed = *changed || r == CmdResult::Success;
      finish();
    };
    this->enqueue(std::move(op));
  }
}

// --------------------------------------------------------------------------------------------
// Battery
// --------------------------------------------------------------------------------------------

Operation NukiHubOpener::make_battery_operation() {
  Operation op;
  op.name = "query opener battery";
  op.step = [this] { return this->nuki_opener_.request_battery_report(&this->battery_report_); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    const opener::BatteryReport &b = this->battery_report_;
    JsonDocument json;
    json["batteryVoltage"] = b.batteryVoltage / 1000.0;
    json["critical"] = (b.criticalBatteryState & 1) ? 1 : 0;
    json["lockAction"] = name_of(LOCK_ACTIONS, (uint8_t) b.lockAction, "Unknown");
    json["startVoltage"] = b.startVoltage / 1000.0;
    json["lowestVoltage"] = b.lowestVoltage / 1000.0;
    this->publish_json_("/battery/advancedJson", json);
  };
  return op;
}

// --------------------------------------------------------------------------------------------
// Time control
// --------------------------------------------------------------------------------------------

Operation NukiHubOpener::make_time_control_operation() {
  Operation op;
  op.name = "read time control entries";
  op.step = [this] { return this->nuki_opener_.retrieve_time_control_entries(); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    this->nuki_opener_.get_time_control_entries(&this->time_control_entries_);
    std::sort(this->time_control_entries_.begin(), this->time_control_entries_.end(),
              [](const opener::TimeControlEntry &a, const opener::TimeControlEntry &b) { return a.entryId < b.entryId; });
    this->publish_time_control_();
  };
  return op;
}

void NukiHubOpener::publish_time_control_() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  char tod[6];
  for (const auto &entry : this->time_control_entries_) {
    JsonObject json = list.add<JsonObject>();
    json["entryId"] = entry.entryId;
    json["enabled"] = entry.enabled;
    util::weekdays_to_json(entry.weekdays, json["weekdays"].to<JsonArray>());
    util::time_of_day(tod, entry.timeHour, entry.timeMin);
    json["time"] = tod;
    json["lockAction"] = name_of(LOCK_ACTIONS, (uint8_t) entry.lockAction, "Unknown");
  }
  this->publish_json_("/timecontrol/json", doc);
}

void NukiHubOpener::handle_time_control_action(const std::string &payload) {
  const std::string request_id = util::request_id(payload);
  const uint32_t received_at = millis();
  auto answer = [this, &request_id, received_at](const char *text) {
    this->answer_command_("/timecontrol", request_id, "", text, false, received_at);
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
  const uint8_t entry_id = json["entryId"].as<unsigned int>();
  if (action != "add" && action != "update" && action != "delete") {
    answer("invalidAction");
    return;
  }

  const opener::TimeControlEntry *existing = nullptr;
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
    op.step = [this, entry_id] { return this->nuki_opener_.remove_time_control_entry(entry_id); };
  } else {
    opener::TimeControlEntry entry{};
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
      opener::LockAction parsed;
      if (!parse_lock_action(lock_action, &parsed)) {
        answer("invalidLockAction");
        return;
      }
      entry.lockAction = parsed;
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
      opener::NewTimeControlEntry fresh{entry.weekdays, entry.timeHour, entry.timeMin, entry.lockAction};
      op.name = "add time control entry";
      op.step = [this, fresh] { return this->nuki_opener_.add_time_control_entry(fresh); };
    } else {
      op.name = "update time control entry";
      op.step = [this, entry] { return this->nuki_opener_.update_time_control_entry(entry); };
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
Operation NukiHubOpener::make_log_operation() {
  Operation op;
  op.name = "read activity log";
  this->log_stage_ = 0;
  this->log_batch_.clear();
  op.step = [this] {
    if (this->log_stage_ == 0) {
      CmdResult result = this->nuki_opener_.retrieve_log_entries(0, this->max_log_entries_, 1, false);
      if (result != CmdResult::Success) {
        return result;
      }
      this->nuki_opener_.get_log_entries(&this->log_batch_);
      uint32_t oldest = UINT32_MAX;
      for (const auto &entry : this->log_batch_) {
        oldest = std::min(oldest, entry.index);
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
      ESP_LOGD(TAG, "Retrieving %u missing log entries from %" PRIu32, this->log_fill_count_, this->log_fill_start_);
    }
    CmdResult result = this->nuki_opener_.retrieve_log_entries(this->log_fill_start_, this->log_fill_count_, 0, false);
    if (result == CmdResult::Working) {
      return result;
    }
    if (result == CmdResult::Success) {
      std::vector<opener::LogEntry> missing;
      this->nuki_opener_.get_log_entries(&missing);
      uint32_t low = UINT32_MAX, high = 0;
      for (const auto &entry : missing) {
        low = std::min(low, entry.index);
        high = std::max(high, entry.index);
      }
      ESP_LOGD(TAG, "Asked for %u log entries from %" PRIu32 ", received %u: %" PRIu32 "..%" PRIu32, this->log_fill_count_,
               this->log_fill_start_, (unsigned) missing.size(), missing.empty() ? 0 : low, high);
      for (const auto &entry : missing) {
        const bool known = std::any_of(this->log_batch_.begin(), this->log_batch_.end(),
                                       [&entry](const opener::LogEntry &e) { return e.index == entry.index; });
        if (!known) {
          this->log_batch_.push_back(entry);
        }
      }
    }
    // The newest entries are in hand either way
    return CmdResult::Success;
  };
  op.done = [this](CmdResult result) {
    if (result == CmdResult::Success) {
      this->process_log_();
    }
  };
  return op;
}

void NukiHubOpener::log_entry_to_json_(const opener::LogEntry &log, JsonObject entry) {
  std::string name = util::fixed_string(log.name, sizeof(log.name));
  if (name.empty() || name.size() == sizeof(log.name)) {
    const char *known = this->auth_name_(log.authId);
    if (known != nullptr) {
      name = known;
    }
  }
  entry["index"] = log.index;
  entry["authorizationId"] = log.authId;
  entry["authorizationName"] = name;
  entry["timeYear"] = log.timeStampYear;
  entry["timeMonth"] = log.timeStampMonth;
  entry["timeDay"] = log.timeStampDay;
  entry["timeHour"] = log.timeStampHour;
  entry["timeMinute"] = log.timeStampMinute;
  entry["timeSecond"] = log.timeStampSecond;
  entry["type"] = name_of(LOGGING_TYPES, (uint8_t) log.loggingType, "Unknown");

  switch (log.loggingType) {
    case opener::LoggingType::LockAction: {
      entry["action"] = name_of(LOCK_ACTIONS, log.data[0], "Unknown");
      entry["trigger"] = name_of(TRIGGERS, log.data[1], "undefined");
      const char *status = name_of(COMPLETION, log.data[3], nullptr);
      if (status != nullptr) {
        entry["completionStatus"] = status;
      } else {
        entry["completionStatus"] = to_string(log.data[3]);
      }
      break;
    }
    case opener::LoggingType::KeypadAction: {
      entry["action"] = name_of(LOCK_ACTIONS, log.data[0], "Unknown");
      entry["trigger"] = name_of(KEYPAD_TRIGGERS, log.data[1], "Unknown");
      if (log.data[2] == (uint8_t) opener::LogEntryCompletionStatus::NotAuthorized) {
        entry["completionStatus"] = "notAuthorized";
      } else if (log.data[2] == (uint8_t) opener::LogEntryCompletionStatus::InvalidCode) {
        entry["completionStatus"] = "invalidCode";
      } else if (log.data[2] == (uint8_t) opener::LogEntryCompletionStatus::InvalidFingerprint) {
        entry["completionStatus"] = "invalidFingerprint";
      } else {
        entry["completionStatus"] = name_of(COMPLETION, log.data[2], "undefined");
      }
      entry["codeId"] = 256U * log.data[4] + log.data[3];
      break;
    }
    case opener::LoggingType::DoorbellRecognition: {
      static const Name MODES[] = {{0, "None"}, {1, "RTO"}, {2, "CM"}};
      entry["mode"] = name_of(MODES, log.data[0] & 3, "Unknown");
      entry["source"] = name_of(LOG_SOURCES, log.data[1], "Unknown");
      entry["geofence"] = log.data[2] == 1 ? "active" : "inactive";
      entry["doorbellSuppression"] = log.data[3] == 1 ? "active" : "inactive";
      entry["soundId"] = log.data[4];
      entry["completionStatus"] = name_of(COMPLETION, log.data[5], "undefined");
      entry["codeId"] = 256U * log.data[7] + log.data[6];
      break;
    }
    default:
      break;
  }
}

void NukiHubOpener::process_log_() {
  std::sort(this->log_batch_.begin(), this->log_batch_.end(),
            [](const opener::LogEntry &a, const opener::LogEntry &b) { return a.index < b.index; });
  if (this->log_batch_.empty()) {
    return;
  }

  if (!this->log_marker_known_) {
    // No marker from an earlier run: what is in the log now counts as published
    this->log_marker_ = this->log_batch_.back().index;
    this->log_marker_known_ = true;
    this->publish_log_marker_();
  }

  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  const size_t first_listed =
      this->log_batch_.size() > this->max_log_entries_ ? this->log_batch_.size() - this->max_log_entries_ : 0;

  for (size_t i = 0; i < this->log_batch_.size(); i++) {
    const opener::LogEntry &log = this->log_batch_[i];
    JsonDocument single;
    JsonObject entry = single.to<JsonObject>();
    this->log_entry_to_json_(log, entry);

    if (log.loggingType == opener::LoggingType::LockAction || log.loggingType == opener::LoggingType::KeypadAction) {
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
      if (log.loggingType == opener::LoggingType::KeypadAction) {
        this->publish_event_("keypad", entry);
      } else if (log.loggingType == opener::LoggingType::LockAction) {
        this->publish_event_("action", entry);
      }
      if (log.loggingType == opener::LoggingType::DoorbellRecognition) {
        this->publish_event_("ring", entry);
      }
      if (log.loggingType == opener::LoggingType::DoorbellRecognition) {
        this->publish_ring_((log.data[0] & 3) == 0);
      }
    }
  }

  this->publish_json_("/log", doc);
  this->publish_json_("/shortLog", doc);
  if (this->last_auth_id_ != 0xFFFFFFFF) {
    this->publish_int_("/authorizationId", this->last_auth_id_);
    this->publish_("/authorizationName", this->last_auth_name_);
  }
  this->log_batch_.clear();
}

}  // namespace esphome::nuki_hub
