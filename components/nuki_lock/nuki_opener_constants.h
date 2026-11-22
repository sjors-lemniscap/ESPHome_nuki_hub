#pragma once
/**
 * @file nuki_opener_constants.h
 * Data types of the Nuki Opener, based on the Nuki Opener Bluetooth API
 * https://developer.nuki.io/page/nuki-opener-api-1/7/
 *
 * Ported from NukiBleEsp32 (NukiOpenerConstants.h), GNU GENERAL PUBLIC LICENSE (see LICENSE)
 */

#include "nuki_ble_constants.h"
#include "nuki_lock_protocol_constants.h"

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <cstdint>

namespace esphome::nuki_lock::opener {

const esphome::esp32_ble_tracker::ESPBTUUID openerPairingServiceUUID =
    esphome::esp32_ble_tracker::ESPBTUUID::from_raw("a92ae100-5501-11e4-916c-0800200c9a66");
const esphome::esp32_ble_tracker::ESPBTUUID openerServiceUUID =
    esphome::esp32_ble_tracker::ESPBTUUID::from_raw("a92ae200-5501-11e4-916c-0800200c9a66");
const esphome::esp32_ble_tracker::ESPBTUUID openerGdioUUID =
    esphome::esp32_ble_tracker::ESPBTUUID::from_raw("a92ae101-5501-11e4-916c-0800200c9a66");
const esphome::esp32_ble_tracker::ESPBTUUID openerUserDataUUID =
    esphome::esp32_ble_tracker::ESPBTUUID::from_raw("a92ae202-5501-11e4-916c-0800200c9a66");

enum class LockAction : uint8_t {
  ActivateRTO             = 0x01,
  DeactivateRTO           = 0x02,
  ElectricStrikeActuation = 0x03,
  ActivateCM              = 0x04,
  DeactivateCM            = 0x05,
  FobAction1              = 0x81,
  FobAction2              = 0x82,
  FobAction3              = 0x83,
  Undefined               = 0xFF
};

enum class CompletionStatus : uint8_t {
  Success    = 0x00,
  Canceled   = 0x02,
  TooRecent  = 0x03,
  Busy       = 0x04,
  Incomplete = 0x08,
  OtherError = 0xFE,
  Unknown    = 0xFF
};

enum class State : uint8_t {
  Uninitialized   = 0x00,
  PairingMode     = 0x01,
  DoorMode        = 0x02,
  ContinuousMode  = 0x03,
  MaintenanceMode = 0x04
};

enum class LockState : uint8_t {
  Uncalibrated = 0x00,
  Locked       = 0x01,
  RTOactive    = 0x03,
  Open         = 0x05,
  Opening      = 0x07,
  Undefined    = 0xFF
};

enum class Trigger : uint8_t {
  System          = 0x00,
  Manual          = 0x01,
  Button          = 0x02,
  Automatic       = 0x03,
  Undefined       = 0xFF
};

enum class ButtonPressAction : uint8_t {
  NoAction      = 0x00,
  ToggleRTO     = 0x01,
  ActivateRTO   = 0x02,
  DeactivateRTO = 0x03,
  ToggleCM      = 0x04,
  ActivateCM    = 0x05,
  DectivateCM   = 0x06,
  Open          = 0x07
};

struct __attribute__((packed)) OpenerState {
  State nukiState = State::Uninitialized;
  LockState lockState = LockState::Undefined;
  Trigger trigger = Trigger::Undefined;
  uint16_t currentTimeYear = 0;
  uint8_t currentTimeMonth = 0;
  uint8_t currentTimeDay = 0;
  uint8_t currentTimeHour = 0;
  uint8_t currentTimeMinute = 0;
  uint8_t currentTimeSecond = 0;
  int16_t timeZoneOffset = 0;
  uint8_t criticalBatteryState = 0;
  uint8_t configUpdateCount = 255;
  uint8_t ringToOpenTimer = 255;
  LockAction lastLockAction = LockAction::Undefined;
  Trigger lastLockActionTrigger = Trigger::Undefined;
  CompletionStatus lastLockActionCompletionStatus = CompletionStatus::Unknown;
  uint16_t unused1 = 255;
  uint8_t unused2 = 255;
  uint8_t unused3 = 255;
  uint8_t unused4 = 255;
  uint8_t unused5 = 255;
  uint8_t unused6 = 255;
  uint8_t accessoryBatteryState = 255;
};

struct __attribute__((packed)) Config {
  uint32_t nukiId = 0;
  unsigned char name[32] = {0};
  float latitude = 0;
  float longitude = 0;
  uint8_t capabilities = 0;
  uint8_t pairingEnabled = 0;
  uint8_t buttonEnabled = 0;
  uint8_t ledFlashEnabled = 0;
  uint16_t currentTimeYear = 0;
  uint8_t currentTimeMonth = 0;
  uint8_t currentTimeDay = 0;
  uint8_t currentTimeHour = 0;
  uint8_t currentTimeMinute = 0;
  uint8_t currentTimeSecond = 0;
  int16_t timeZoneOffset = 0;
  uint8_t dstMode = 0;
  uint8_t hasFob = 0;
  uint8_t fobAction1 = 0;
  uint8_t fobAction2 = 0;
  uint8_t fobAction3 = 0;
  uint8_t operatingMode = 0;
  AdvertisingMode advertisingMode = AdvertisingMode::Unknown;
  uint8_t hasKeypad = 0;
  unsigned char firmwareVersion[3] = {0, 0, 0};
  unsigned char hardwareRevision[2] = {0, 0};
  TimeZoneId timeZoneId = TimeZoneId::None;
  uint8_t hasKeypadV2 = 255;
};

struct __attribute__((packed)) NewConfig {
  unsigned char name[32];
  float latitude;
  float longitude;
  uint8_t capabilities;
  uint8_t pairingEnabled;
  uint8_t buttonEnabled;
  uint8_t ledFlashEnabled;
  int16_t timeZoneOffset;
  uint8_t dstMode;
  uint8_t fobAction1;
  uint8_t fobAction2;
  uint8_t fobAction3;
  uint8_t operatingMode;
  AdvertisingMode advertisingMode;
  TimeZoneId timeZoneId;
};

struct __attribute__((packed)) AdvancedConfig {
  uint16_t intercomID = 0;
  uint8_t busModeSwitch = 0;
  uint16_t shortCircuitDuration = 0;
  uint16_t electricStrikeDelay = 0;
  uint8_t randomElectricStrikeDelay = 0;
  uint16_t electricStrikeDuration = 0;
  uint8_t disableRtoAfterRing = 0;
  uint8_t rtoTimeout = 0;
  // Sent by the Opener between rtoTimeout and doorbellSuppression, meaning unknown. It has to
  // be written back unchanged or the Opener rejects the whole config (K_ERROR_BAD_PARAMETER).
  uint8_t unknown = 0;
  uint8_t doorbellSuppression = 0;
  uint16_t doorbellSuppressionDuration = 0;
  uint8_t soundRing = 0;
  uint8_t soundOpen = 0;
  uint8_t soundRto = 0;
  uint8_t soundCm = 0;
  uint8_t soundConfirmation = 0;
  uint8_t soundLevel = 0;
  ButtonPressAction singleButtonPressAction = ButtonPressAction::NoAction;
  ButtonPressAction doubleButtonPressAction = ButtonPressAction::NoAction;
  BatteryType batteryType = BatteryType::Unknown;
  uint8_t automaticBatteryTypeDetection = 0;
  uint8_t autoUpdateEnabled = 255;
};

struct __attribute__((packed)) NewAdvancedConfig {
  uint16_t intercomID;
  uint8_t busModeSwitch;
  uint16_t shortCircuitDuration;
  uint16_t electricStrikeDelay;
  uint8_t randomElectricStrikeDelay;
  uint16_t electricStrikeDuration;
  uint8_t disableRtoAfterRing;
  uint8_t rtoTimeout;
  uint8_t unknown;
  uint8_t doorbellSuppression;
  uint16_t doorbellSuppressionDuration;
  uint8_t soundRing;
  uint8_t soundOpen;
  uint8_t soundRto;
  uint8_t soundCm;
  uint8_t soundConfirmation;
  uint8_t soundLevel;
  ButtonPressAction singleButtonPressAction;
  ButtonPressAction doubleButtonPressAction;
  BatteryType batteryType;
  uint8_t automaticBatteryTypeDetection;
};

struct __attribute__((packed)) BatteryReport {
  uint16_t batteryDrain = 0;
  uint16_t batteryVoltage = 0;
  uint8_t criticalBatteryState = 0;
  LockAction lockAction = LockAction::Undefined;
  uint16_t startVoltage = 0;
  uint16_t lowestVoltage = 0;
  uint16_t lockDistance = 0;
  int8_t startTemperature = 0;
  uint16_t maxTurnCurrent = 0;
  uint16_t batteryResistance = 0;
};

struct __attribute__((packed)) TimeControlEntry {
  uint8_t entryId;
  uint8_t enabled;
  uint8_t weekdays;
  uint8_t timeHour;
  uint8_t timeMin;
  LockAction lockAction;
};

struct __attribute__((packed)) NewTimeControlEntry {
  uint8_t weekdays;
  uint8_t timeHour;
  uint8_t timeMin;
  LockAction lockAction;
};

struct __attribute__((packed)) ContinuousModeAction {
  uint8_t enabled;
  uint8_t timeout;
};

enum class LoggingType : uint8_t {
  LoggingEnabled            = 0x01,
  LockAction                = 0x02,
  Calibration               = 0x03,
  KeypadAction              = 0x05,
  DoorbellRecognition       = 0x06
};

struct __attribute__((packed)) LogEntry {
  uint32_t index = 0;
  uint16_t timeStampYear = 0;
  uint8_t timeStampMonth = 0;
  uint8_t timeStampDay = 0;
  uint8_t timeStampHour = 0;
  uint8_t timeStampMinute = 0;
  uint8_t timeStampSecond = 0;
  uint32_t authId = 0;
  uint8_t name[32] = {0};
  LoggingType loggingType = LoggingType::LoggingEnabled;
  uint8_t data[8] = {0};
};

enum class LogEntryCompletionStatus : uint8_t {
  InvalidCode        = 0xe0,
  InvalidFingerprint = 0xe1,
  NotAuthorized      = 0x09,
};

}  // namespace esphome::nuki_lock::opener
