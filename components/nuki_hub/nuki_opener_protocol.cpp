/**
 * @file nuki_opener_protocol.cpp
 * Communication with a Nuki Opener, the counterpart of nuki_lock_protocol.cpp
 *
 * Ported from NukiBleEsp32 (NukiOpener.cpp), MIT (see LICENSE)
 */

#include "nuki_opener_protocol.h"
#include "nuki_ble_utils.h"

#include "esphome/core/log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace esphome::nuki_hub::opener {

static const char *const TAG = "NukiOpener";

// Copies a received payload into `target`. The Opener may send fewer bytes than the struct
// holds (older firmware) or more (newer firmware): bytes the payload doesn't have keep their
// defaults, bytes the struct doesn't know are dropped. Payloads shorter than `min_len` are
// refused.
template<typename T>
static bool copy_payload(T *target, size_t min_len, const unsigned char *data, uint16_t data_len, const char *what) {
  if (data_len < min_len) {
    ESP_LOGW(TAG, "Ignoring %s of %u bytes, at least %u expected", what, (unsigned) data_len, (unsigned) min_len);
    return false;
  }
  *target = T();
  memcpy(static_cast<void *>(target), data, std::min<size_t>(data_len, sizeof(T)));
  return true;
}

NukiOpener::NukiOpener(const std::string &device_name, const uint32_t device_id)
    : NukiBle(device_name, device_id, openerPairingServiceUUID, openerPairingServiceUUID, openerServiceUUID,
              openerGdioUUID, openerGdioUUID, openerUserDataUUID, device_name + "opener") {
  this->error_code_ = 0xFF;
}

CmdResult NukiOpener::lock_action(const LockAction lock_action, const uint32_t nukiAppId, const uint8_t flags,
                                  const char *nameSuffix, const uint8_t nameSuffixLen) {
  NukiAction action{};

  if ((lock_action == LockAction::ActivateCM || lock_action == LockAction::DeactivateCM) && nukiAppId != 1) {
    ContinuousModeAction continuous_mode_action{};
    continuous_mode_action.enabled = (lock_action == LockAction::ActivateCM ? 1 : 0);

    action.cmdType = CommandType::CommandWithChallengeAndPin;
    action.command = Command::ContinuousModeAction;
    memcpy(action.payload, &continuous_mode_action, sizeof(ContinuousModeAction));
    action.payloadLen = sizeof(ContinuousModeAction);
    return this->execute_action(action);
  }

  const uint8_t suffix_len = nameSuffix != nullptr ? std::min<uint8_t>(nameSuffixLen, 20) : 0;
  memcpy(action.payload, &lock_action, sizeof(LockAction));
  memcpy(&action.payload[sizeof(LockAction)], &nukiAppId, 4);
  memcpy(&action.payload[sizeof(LockAction) + 4], &flags, 1);
  if (suffix_len > 0) {
    memcpy(&action.payload[sizeof(LockAction) + 5], nameSuffix, suffix_len);
  }
  action.cmdType = CommandType::CommandWithChallengeAndAccept;
  action.command = Command::LockAction;
  action.payloadLen = sizeof(LockAction) + 5 + suffix_len;
  return this->execute_action(action);
}

CmdResult NukiOpener::continuous_mode(bool enabled, uint8_t timeoutMinutes) {
  NukiAction action{};
  ContinuousModeAction continuous_mode_action{};
  continuous_mode_action.enabled = enabled ? 1 : 0;
  continuous_mode_action.timeout = timeoutMinutes;
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::ContinuousModeAction;
  memcpy(action.payload, &continuous_mode_action, sizeof(ContinuousModeAction));
  action.payloadLen = sizeof(ContinuousModeAction);
  return this->execute_action(action);
}

CmdResult NukiOpener::request_opener_state(OpenerState *state) {
  NukiAction action{};
  uint16_t payload = (uint16_t) Command::KeyturnerStates;

  action.cmdType = CommandType::Command;
  action.command = Command::RequestData;
  memcpy(&action.payload[0], &payload, sizeof(payload));
  action.payloadLen = sizeof(payload);

  CmdResult result = this->execute_action(action);
  if (result == CmdResult::Success) {
    *state = this->opener_state_;
  }
  return result;
}

void NukiOpener::retrieve_opener_state(OpenerState *state) { *state = this->opener_state_; }

bool NukiOpener::take_pushed_opener_state(OpenerState *state) {
  if (!this->opener_state_pushed_) {
    return false;
  }
  this->opener_state_pushed_ = false;
  *state = this->opener_state_;
  return true;
}

CmdResult NukiOpener::request_battery_report(BatteryReport *report) {
  NukiAction action{};
  uint16_t payload = (uint16_t) Command::BatteryReport;

  action.cmdType = CommandType::Command;
  action.command = Command::RequestData;
  memcpy(&action.payload[0], &payload, sizeof(payload));
  action.payloadLen = sizeof(payload);

  CmdResult result = this->execute_action(action);
  if (result == CmdResult::Success) {
    *report = this->battery_report_;
  }
  return result;
}

CmdResult NukiOpener::request_config(Config *config) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallenge;
  action.command = Command::RequestConfig;

  CmdResult result = this->execute_action(action);
  if (result == CmdResult::Success) {
    *config = this->config_;
  }
  return result;
}

CmdResult NukiOpener::request_advanced_config(AdvancedConfig *config) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallenge;
  action.command = Command::RequestAdvancedConfig;

  CmdResult result = this->execute_action(action);
  if (result == CmdResult::Success) {
    *config = this->advanced_config_;
  }
  return result;
}

CmdResult NukiOpener::write_config(const std::function<void(Config &)> &mutate) {
  if (!this->config_write_pending_) {
    CmdResult result = this->request_config(&this->config_write_buffer_);
    if (result != CmdResult::Success) {
      return result;
    }
    mutate(this->config_write_buffer_);
    this->config_write_pending_ = true;
  }
  NewConfig new_config{};
  create_new_config(&this->config_write_buffer_, &new_config);
  CmdResult result = this->set_config(new_config);
  if (result != CmdResult::Working) {
    this->config_write_pending_ = false;
  }
  return result;
}

CmdResult NukiOpener::write_advanced_config(const std::function<void(AdvancedConfig &)> &mutate) {
  if (!this->advanced_config_write_pending_) {
    CmdResult result = this->request_advanced_config(&this->advanced_config_write_buffer_);
    if (result != CmdResult::Success) {
      return result;
    }
    mutate(this->advanced_config_write_buffer_);
    this->advanced_config_write_pending_ = true;
  }
  NewAdvancedConfig new_config{};
  create_new_advanced_config(&this->advanced_config_write_buffer_, &new_config);
  CmdResult result = this->set_advanced_config(new_config);
  if (result != CmdResult::Working) {
    this->advanced_config_write_pending_ = false;
  }
  return result;
}

CmdResult NukiOpener::add_time_control_entry(NewTimeControlEntry entry) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::AddTimeControlEntry;
  memcpy(action.payload, &entry, sizeof(NewTimeControlEntry));
  action.payloadLen = sizeof(NewTimeControlEntry);
  return this->execute_action(action);
}

CmdResult NukiOpener::update_time_control_entry(TimeControlEntry entry) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::UpdateTimeControlEntry;
  memcpy(action.payload, &entry, sizeof(TimeControlEntry));
  action.payloadLen = sizeof(TimeControlEntry);
  return this->execute_action(action);
}

CmdResult NukiOpener::remove_time_control_entry(uint8_t entryId) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::RemoveTimeControlEntry;
  action.payload[0] = entryId;
  action.payloadLen = 1;
  return this->execute_action(action);
}

// The Opener answers a list request with a count (not for every list), the entries one by
// one and a final "complete" status. The command state machine reports success on the first
// of those, so the rest is awaited here: until the status arrives, or all announced entries
// are in, or nothing more came for list_timeout_ms_.
CmdResult NukiOpener::retrieve_list(const NukiAction &request, const std::function<void()> &clear,
                                    const std::function<size_t()> &size, const char *what) {
  this->extend_disconnect_timeout();
  const int64_t now = esp_timer_get_time() / 1000;

  if (!this->list_waiting_) {
    if (!this->list_request_started_) {
      clear();
      this->list_complete_ = false;
      this->list_count_known_ = false;
      this->list_expected_count_ = 0;
      this->list_request_started_ = true;
    }
    CmdResult result = this->execute_action(request);
    if (result == CmdResult::Working) {
      return result;
    }
    this->list_request_started_ = false;
    if (result != CmdResult::Success) {
      return result;
    }
    this->list_waiting_ = true;
    this->list_wait_since_ = now;
    this->list_last_size_ = size();
  }

  const size_t received = size();
  if (this->list_complete_ || (this->list_count_known_ && received >= this->list_expected_count_)) {
    this->list_waiting_ = false;
    return CmdResult::Success;
  }
  if (received != this->list_last_size_) {
    this->list_last_size_ = received;
    this->list_wait_since_ = now;
  }
  if (now - this->list_wait_since_ > this->list_timeout_ms_) {
    this->list_waiting_ = false;
    ESP_LOGW(TAG, "Receive %s timeout after %u entries", what, (unsigned) received);
    this->disconnect();
    return CmdResult::TimeOut;
  }
  return CmdResult::Working;
}

CmdResult NukiOpener::retrieve_time_control_entries() {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::RequestTimeControlEntries;
  action.payloadLen = 0;
  return this->retrieve_list(
      action, [this] { this->list_of_time_control_entries_.clear(); },
      [this] { return this->list_of_time_control_entries_.size(); }, "time control entries");
}

void NukiOpener::get_time_control_entries(std::vector<TimeControlEntry> *entries) {
  *entries = this->list_of_time_control_entries_;
}

CmdResult NukiOpener::retrieve_log_entries(const uint32_t startIndex, const uint16_t count, const uint8_t sortOrder,
                                           const bool totalCount) {
  NukiAction action{};
  const uint8_t total_count = totalCount ? 1 : 0;
  memcpy(action.payload, &startIndex, 4);
  memcpy(&action.payload[4], &count, 2);
  memcpy(&action.payload[6], &sortOrder, 1);
  memcpy(&action.payload[7], &total_count, 1);
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::RequestLogEntries;
  action.payloadLen = 8;
  return this->retrieve_list(
      action, [this] { this->list_of_log_entries_.clear(); }, [this] { return this->list_of_log_entries_.size(); },
      "log entries");
}

void NukiOpener::get_log_entries(std::vector<LogEntry> *entries) { *entries = this->list_of_log_entries_; }

bool NukiOpener::is_battery_critical() { return (this->opener_state_.criticalBatteryState & 1) != 0; }

bool NukiOpener::is_keypad_battery_critical() {
  if (this->opener_state_.accessoryBatteryState != 255 && (this->opener_state_.accessoryBatteryState & 1) == 1) {
    return (this->opener_state_.accessoryBatteryState & 3) == 3;
  }
  return false;
}

CmdResult NukiOpener::set_config(NewConfig newConfig) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::SetConfig;
  memcpy(action.payload, &newConfig, sizeof(NewConfig));
  action.payloadLen = sizeof(NewConfig);
  return this->execute_action(action);
}

CmdResult NukiOpener::set_advanced_config(NewAdvancedConfig newAdvancedConfig) {
  NukiAction action{};
  action.cmdType = CommandType::CommandWithChallengeAndPin;
  action.command = Command::SetAdvancedConfig;
  memcpy(action.payload, &newAdvancedConfig, sizeof(NewAdvancedConfig));
  action.payloadLen = sizeof(NewAdvancedConfig);
  return this->execute_action(action);
}

void NukiOpener::create_new_config(const Config *oldConfig, NewConfig *newConfig) {
  memcpy(newConfig->name, oldConfig->name, sizeof(newConfig->name));
  newConfig->latitude = oldConfig->latitude;
  newConfig->longitude = oldConfig->longitude;
  newConfig->capabilities = oldConfig->capabilities;
  newConfig->pairingEnabled = oldConfig->pairingEnabled;
  newConfig->buttonEnabled = oldConfig->buttonEnabled;
  newConfig->ledFlashEnabled = oldConfig->ledFlashEnabled;
  newConfig->timeZoneOffset = oldConfig->timeZoneOffset;
  newConfig->dstMode = oldConfig->dstMode;
  newConfig->fobAction1 = oldConfig->fobAction1;
  newConfig->fobAction2 = oldConfig->fobAction2;
  newConfig->fobAction3 = oldConfig->fobAction3;
  newConfig->operatingMode = oldConfig->operatingMode;
  newConfig->advertisingMode = oldConfig->advertisingMode;
  newConfig->timeZoneId = oldConfig->timeZoneId;
}

void NukiOpener::create_new_advanced_config(const AdvancedConfig *oldConfig, NewAdvancedConfig *newConfig) {
  newConfig->intercomID = oldConfig->intercomID;
  newConfig->busModeSwitch = oldConfig->busModeSwitch;
  newConfig->shortCircuitDuration = oldConfig->shortCircuitDuration;
  newConfig->electricStrikeDelay = oldConfig->electricStrikeDelay;
  newConfig->randomElectricStrikeDelay = oldConfig->randomElectricStrikeDelay;
  newConfig->electricStrikeDuration = oldConfig->electricStrikeDuration;
  newConfig->disableRtoAfterRing = oldConfig->disableRtoAfterRing;
  newConfig->rtoTimeout = oldConfig->rtoTimeout;
  newConfig->unknown = oldConfig->unknown;
  newConfig->doorbellSuppression = oldConfig->doorbellSuppression;
  newConfig->doorbellSuppressionDuration = oldConfig->doorbellSuppressionDuration;
  newConfig->soundRing = oldConfig->soundRing;
  newConfig->soundOpen = oldConfig->soundOpen;
  newConfig->soundRto = oldConfig->soundRto;
  newConfig->soundCm = oldConfig->soundCm;
  newConfig->soundConfirmation = oldConfig->soundConfirmation;
  newConfig->soundLevel = oldConfig->soundLevel;
  newConfig->singleButtonPressAction = oldConfig->singleButtonPressAction;
  newConfig->doubleButtonPressAction = oldConfig->doubleButtonPressAction;
  newConfig->batteryType = oldConfig->batteryType;
  newConfig->automaticBatteryTypeDetection = oldConfig->automaticBatteryTypeDetection;
}

void NukiOpener::handle_return_message(Command returnCode, unsigned char *data, uint16_t dataLen) {
  this->extend_disconnect_timeout();

  switch (returnCode) {
    case Command::KeyturnerStates: {
      print_buffer((uint8_t *) data, dataLen, false, "keyturnerStates", this->debug_nuki_hex_data_);
      if (!copy_payload(&this->opener_state_,
                        offsetof(OpenerState, lastLockActionCompletionStatus) + sizeof(CompletionStatus), data, dataLen,
                        "opener state")) {
        return;
      }
      this->opener_state_pushed_ = true;
      break;
    }
    case Command::BatteryReport: {
      print_buffer((uint8_t *) data, dataLen, false, "batteryReport", this->debug_nuki_hex_data_);
      if (!copy_payload(&this->battery_report_, offsetof(BatteryReport, lockDistance), data, dataLen,
                        "battery report")) {
        return;
      }
      break;
    }
    case Command::Config: {
      print_buffer((uint8_t *) data, dataLen, false, "config", this->debug_nuki_hex_data_);
      if (!copy_payload(&this->config_, offsetof(Config, hardwareRevision) + sizeof(Config::hardwareRevision), data,
                        dataLen, "config")) {
        return;
      }
      break;
    }
    case Command::AdvancedConfig: {
      print_buffer((uint8_t *) data, dataLen, false, "advancedConfig", this->debug_nuki_hex_data_);
      if (!copy_payload(&this->advanced_config_, offsetof(AdvancedConfig, soundLevel) + sizeof(uint8_t), data, dataLen,
                        "advanced config")) {
        return;
      }
      break;
    }
    case Command::TimeControlEntryCount: {
      if (dataLen >= 1) {
        this->list_expected_count_ = data[0];
        this->list_count_known_ = true;
      }
      break;
    }
    case Command::TimeControlEntry: {
      print_buffer((uint8_t *) data, dataLen, false, "timeControlEntry", this->debug_nuki_hex_data_);
      TimeControlEntry entry{};
      if (!copy_payload(&entry, sizeof(TimeControlEntry), data, dataLen, "time control entry")) {
        return;
      }
      this->list_of_time_control_entries_.push_back(entry);
      break;
    }
    case Command::LogEntry: {
      print_buffer((uint8_t *) data, dataLen, false, "logEntry", this->debug_nuki_hex_data_);
      LogEntry entry{};
      if (!copy_payload(&entry, offsetof(LogEntry, loggingType) + sizeof(LoggingType), data, dataLen, "log entry")) {
        return;
      }
      this->list_of_log_entries_.push_back(entry);
      break;
    }
    case Command::Status: {
      // Only the "complete" that follows the list request ends the list. The one of the command
      // before can still be on its way when the next command starts right behind it, and
      // would end the list after its first entry.
      const CommandState state = this->get_command_state();
      const bool request_sent =
          this->list_waiting_ || state == CommandState::CmdSent || state == CommandState::CmdAccepted;
      if (dataLen >= 1 && data[0] == (uint8_t) CommandStatus::Complete && request_sent) {
        this->list_complete_ = true;
      }
      NukiBle::handle_return_message(returnCode, data, dataLen);
      break;
    }
    default:
      NukiBle::handle_return_message(returnCode, data, dataLen);
  }
  this->last_msg_code_received_ = returnCode;
}

void NukiOpener::abort_command() {
  NukiBle::abort_command();
  this->config_write_pending_ = false;
  this->advanced_config_write_pending_ = false;
  this->list_request_started_ = false;
  this->list_waiting_ = false;
}

void NukiOpener::log_error_code(uint8_t error_code) {
  const char *name = "unknown";
  switch (error_code) {
    case 0xFD: name = "ERROR_BAD_CRC"; break;
    case 0xFE: name = "ERROR_BAD_LENGTH"; break;
    case 0xFF: name = "ERROR_UNKNOWN"; break;
    case 0x10: name = "P_ERROR_NOT_PAIRING"; break;
    case 0x11: name = "P_ERROR_BAD_AUTHENTICATOR"; break;
    case 0x12: name = "P_ERROR_BAD_PARAMETER"; break;
    case 0x13: name = "P_ERROR_MAX_USER"; break;
    case 0x20: name = "K_ERROR_NOT_AUTHORIZED"; break;
    case 0x21: name = "K_ERROR_BAD_PIN"; break;
    case 0x22: name = "K_ERROR_BAD_NONCE"; break;
    case 0x23: name = "K_ERROR_BAD_PARAMETER"; break;
    case 0x24: name = "K_ERROR_INVALID_AUTH_ID"; break;
    case 0x25: name = "K_ERROR_DISABLED"; break;
    case 0x26: name = "K_ERROR_REMOTE_NOT_ALLOWED"; break;
    case 0x27: name = "K_ERROR_TIME_NOT_ALLOWED"; break;
    case 0x28: name = "K_ERROR_TOO_MANY_PIN_ATTEMPTS"; break;
    case 0x29: name = "K_ERROR_TOO_MANY_ENTRIES"; break;
    case 0x2A: name = "K_ERROR_CODE_ALREADY_EXISTS"; break;
    case 0x2B: name = "K_ERROR_CODE_INVALID"; break;
    case 0x2C: name = "K_ERROR_CODE_INVALID_TIMEOUT_1"; break;
    case 0x2D: name = "K_ERROR_CODE_INVALID_TIMEOUT_2"; break;
    case 0x2E: name = "K_ERROR_CODE_INVALID_TIMEOUT_3"; break;
    case 0x45: name = "K_ERROR_BUSY"; break;
    case 0x46: name = "K_ERROR_CANCELED"; break;
    case 0x47: name = "K_ERROR_NOT_CALIBRATED"; break;
    case 0x4C: name = "K_ERROR_VOLTAGE_TOO_LOW"; break;
    case 0x4D: name = "K_ERROR_FIRMWARE_UPDATE_NEEDED"; break;
    default: break;
  }
  ESP_LOGE(TAG, "%s", name);
}

}  // namespace esphome::nuki_hub::opener
