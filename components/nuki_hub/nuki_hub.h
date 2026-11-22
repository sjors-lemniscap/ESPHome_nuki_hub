#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/lock/lock.h"
#include "esphome/components/nuki_lock/nuki_ble.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace esphome::nuki_hub {

using nuki_lock::CmdResult;

static const char *const NUKI_HUB_VERSION = "0.1.0";

/// One unit of work on the Bluetooth link. `step` is called on every tick until it returns
/// something other than CmdResult::Working, `done` once with the final result.
struct Operation {
  const char *name;
  std::function<CmdResult()> step;
  std::function<void(CmdResult)> done;
  bool retry{true};
};

/// What the last executed keypad command decided without talking to the device
struct KeypadCommand {
  std::string payload;
  enum class Phase : uint8_t { Parse, Fetch, Execute } phase{Phase::Parse};
  std::string action;
  uint16_t code_id{0};
  nuki_lock::NewKeypadEntry add{};
  nuki_lock::UpdatedKeypadEntry update{};
  bool has_name{false}, has_code{false}, has_enabled{false}, has_time_limited{false};
  bool has_from{false}, has_until{false}, has_weekdays{false}, has_from_time{false}, has_until_time{false};
  bool fetched{false};
  /// Set when the command ends without a Bluetooth result (validation), published as is
  const char *verdict{nullptr};
};

/**
 * Makes a Nuki device available over MQTT with the topics and payloads of Nuki Hub
 * (https://github.com/technyon/nuki_hub), so software written against Nuki Hub keeps working.
 *
 * This class holds everything both device types share: the command queue, the keypad,
 * authorizations, the hub's own maintenance topics. NukiHubOpener and NukiHubLock add the
 * device specific state, actions, configuration, time control and activity log.
 */
class NukiHubComponent : public Component, public nuki_lock::SmartlockEventHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void notify(nuki_lock::EventType event_type) override;

  // Configuration
  void set_topic_prefix(const std::string &prefix) { this->topic_prefix_ = prefix; }
  void set_security_pin(uint32_t pin) { this->security_pin_ = pin; }
  void set_pairing_as_app(bool as_app) { this->pairing_as_app_ = as_app; }
  void set_pairing_mode_at_boot(bool enabled) { this->pairing_mode_ = enabled; }
  void set_import_credentials(const std::vector<uint8_t> &address, const std::vector<uint8_t> &key,
                              const std::vector<uint8_t> &auth_id) {
    this->import_address_ = address;
    this->import_key_ = key;
    this->import_auth_id_ = auth_id;
  }
  void set_query_interval_state(uint32_t seconds) { this->interval_state_ = seconds; }
  void set_query_interval_config(uint32_t seconds) { this->interval_config_ = seconds; }
  void set_query_interval_battery(uint32_t seconds) { this->interval_battery_ = seconds; }
  void set_query_interval_keypad(uint32_t seconds) { this->interval_keypad_ = seconds; }
  void set_query_interval_auth(uint32_t seconds) { this->interval_auth_ = seconds; }
  void set_query_interval_time_control(uint32_t seconds) { this->interval_time_control_ = seconds; }
  void set_keypad_control(bool enabled) { this->keypad_control_ = enabled; }
  void set_keypad_publish_code(bool enabled) { this->keypad_publish_code_ = enabled; }
  void set_max_keypad_entries(uint16_t count) { this->max_keypad_entries_ = count; }
  void set_max_auth_entries(uint16_t count) { this->max_auth_entries_ = count; }
  void set_max_log_entries(uint16_t count) { this->max_log_entries_ = count; }
  void set_publish_auth_log(bool enabled) { this->publish_auth_log_ = enabled; }
  void set_command_retries(uint8_t retries) { this->command_retries_ = retries; }
  void set_command_retry_delay(uint32_t ms) { this->command_retry_delay_ = ms; }
  void set_ble_general_timeout(uint32_t ms) { this->ble_general_timeout_ = ms; }
  void set_ble_command_timeout(uint32_t ms) { this->ble_command_timeout_ = ms; }
  void set_update_time(bool enabled) { this->update_time_ = enabled; }
  void set_publish_maintenance(bool enabled) { this->maintenance_enabled_ = enabled; }
  void set_debug(bool connect, bool communication, bool readable_data, bool hex_data, bool command) {
    this->debug_connect_ = connect;
    this->debug_communication_ = communication;
    this->debug_readable_data_ = readable_data;
    this->debug_hex_data_ = hex_data;
    this->debug_command_ = command;
  }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
#endif

  virtual nuki_lock::NukiBle *ble() = 0;
  /// "opener" or "lock", the topic level below the prefix
  virtual const char *kind() const = 0;

  bool is_paired() { return this->ble()->is_paired_with_lock(); }
  bool is_available() const { return this->available_; }
  void set_pairing_mode(bool enabled);
  void unpair();
  /// Queues a device action by its Nuki Hub name ("activateRTO", "unlock", ...). False if unknown.
  bool request_action(const std::string &action);
  void request_state_update() { this->want_state_ = true; }

  /// The device state for a lock entity
  virtual lock::LockState entity_state() const = 0;
  /// The action a lock entity asks for when locking, unlocking or opening
  virtual const char *entity_action(bool lock, bool open) const = 0;

  template<typename F> void add_on_state_callback(F &&callback) { this->state_callback_.add(std::forward<F>(callback)); }

 protected:
  // --- device specific half -------------------------------------------------------------
  virtual void setup_device() {}
  /// Reads the state from the device and publishes it
  virtual Operation make_state_operation() = 0;
  virtual Operation make_config_operation() = 0;
  virtual Operation make_battery_operation() = 0;
  virtual Operation make_time_control_operation() = 0;
  /// Reads the activity log from `start_index` on (0: only the newest entries)
  virtual Operation make_log_operation() = 0;
  virtual bool queue_action(const std::string &action) = 0;
  virtual void handle_config_action(const std::string &payload) = 0;
  virtual void handle_time_control_action(const std::string &payload) = 0;
  virtual bool has_keypad() const = 0;
  virtual bool config_valid() const = 0;
  virtual void publish_unavailable() {}

  // --- shared machinery -----------------------------------------------------------------
  void enqueue(Operation op, bool front = false);
  /// The first tick of the next operation, picked from the queue or the pending polls
  bool start_next_operation_();
  void finish_operation_(CmdResult result);

  std::string topic_(const char *suffix) const;
  std::string hub_topic_(const char *suffix) const;
  bool publish_(const char *suffix, const std::string &payload, bool retain = true);
  bool publish_(const char *suffix, const char *payload, bool retain = true);
  bool publish_int_(const char *suffix, int64_t value, bool retain = true);
  bool publish_json_(const char *suffix, JsonDocument &doc, bool retain = true);
  bool publish_hub_(const char *suffix, const std::string &payload, bool retain = true);
  void subscribe_(const char *suffix, std::function<void(const std::string &)> &&callback);
  void subscribe_hub_(const char *suffix, std::function<void(const std::string &)> &&callback);

  void on_mqtt_connected_();
  void publish_maintenance_(bool initial);
  void set_available_(bool available);

  // Keypad
  void handle_keypad_action_(const std::string &payload);
  CmdResult keypad_command_step_(KeypadCommand &cmd);
  void keypad_command_done_(KeypadCommand &cmd, CmdResult result);
  Operation make_keypad_operation_();
  void publish_keypad_();
  const nuki_lock::KeypadEntry *find_keypad_entry_(uint16_t code_id) const;

  // Authorizations
  void handle_auth_action_(const std::string &payload);
  Operation make_auth_operation_();
  void publish_auth_();
  const char *auth_name_(uint32_t auth_id) const;

  Operation make_time_operation_();
  Operation make_pin_operation_();

  // Activity log bookkeeping shared by both device types
  /// True once the marker of the last published entry is known (retained topic or first read)
  bool log_marker_ready_() const;
  void publish_log_marker_();

  static const char *result_to_string(CmdResult result);

  std::string topic_prefix_;
  uint32_t security_pin_{0};
  bool pairing_as_app_{false};
  bool pairing_mode_{false};
  std::vector<uint8_t> import_address_;
  std::vector<uint8_t> import_key_;
  std::vector<uint8_t> import_auth_id_;

  uint32_t interval_state_{1800};
  uint32_t interval_config_{3600};
  uint32_t interval_battery_{1800};
  uint32_t interval_keypad_{1800};
  uint32_t interval_auth_{3600};
  uint32_t interval_time_control_{3600};
  bool keypad_control_{true};
  bool keypad_publish_code_{true};
  uint16_t max_keypad_entries_{100};
  uint16_t max_auth_entries_{20};
  uint16_t max_log_entries_{5};
  bool publish_auth_log_{true};
  uint8_t command_retries_{3};
  uint32_t command_retry_delay_{1000};
  uint32_t ble_general_timeout_{10000};
  uint32_t ble_command_timeout_{3000};
  bool update_time_{false};
  bool maintenance_enabled_{true};
  bool debug_connect_{false}, debug_communication_{false}, debug_readable_data_{false}, debug_hex_data_{false},
      debug_command_{false};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif

  // Queue
  std::deque<Operation> queue_;
  bool op_active_{false};
  Operation op_;
  uint8_t op_retries_left_{0};
  bool op_retry_pending_{false};
  uint32_t next_op_time_{0};
  uint32_t last_loop_{0};
  static const size_t MAX_QUEUE = 24;

  // Polls waiting for a free slot
  bool want_state_{false};
  bool want_config_{false};
  bool want_battery_{false};
  bool want_keypad_{false};
  bool want_auth_{false};
  bool want_time_control_{false};
  bool want_log_{false};
  bool want_time_{false};
  bool want_pin_check_{false};
  uint32_t keypad_refresh_at_{0};
  uint32_t log_refresh_at_{0};

  // Device link
  bool available_{false};
  bool availability_published_{false};
  uint8_t consecutive_failures_{0};
  bool pin_valid_{false};
  bool pin_checked_{false};
  bool status_flag_{false};

  // MQTT
  bool mqtt_was_connected_{false};
  bool subscribed_{false};
  uint32_t last_maintenance_{0};
  uint32_t boot_millis_{0};
  ESPPreferenceObject restart_reason_pref_;
  uint8_t restart_reason_{0};

  // Keypad
  std::vector<nuki_lock::KeypadEntry> keypad_entries_;
  bool keypad_cache_valid_{false};
  uint16_t keypad_commands_pending_{0};

  // Authorizations
  std::vector<nuki_lock::AuthorizationEntry> auth_entries_;

  // Activity log
  uint32_t log_marker_{0};
  bool log_marker_known_{false};
  uint32_t log_marker_deadline_{0};
  uint32_t last_auth_id_{0xFFFFFFFF};
  std::string last_auth_name_;

  CallbackManager<void()> state_callback_;
};

// Helpers shared by the device classes
namespace util {
void date_time(char *out, int year, int month, int day, int hour, int minute, int second);
void time_of_day(char *out, int hour, int minute);
void weekdays_to_json(uint8_t weekdays, JsonArray array);
uint8_t weekdays_from_json(JsonVariantConst value);
/// Copies a fixed size, not necessarily terminated name field
std::string fixed_string(const uint8_t *data, size_t size);
bool parse_date_time(const std::string &text, uint16_t *out);
bool parse_time_of_day(const std::string &text, uint8_t *out);
/// A JSON value as text, whether it was sent as a string or a number
std::string as_text(JsonVariantConst value);
bool is_number(const std::string &text);
}  // namespace util

}  // namespace esphome::nuki_hub
