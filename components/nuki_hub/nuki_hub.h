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
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <vector>

namespace esphome::nuki_hub {

/// Where JSON documents live: PSRAM where the board has it, their size is not known beforehand
inline ArduinoJson::Allocator *json_memory() { return json::heap_json_allocator(); }

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

/// Keypad commands that arrived together and are answered together
struct KeypadBatch {
  std::string request_id;
  const char *result_topic{"/keypad/batchResult"};
  uint32_t received_at{0};
  size_t total{0};
  size_t remaining{0};
  size_t succeeded{0};
  bool silent{false};
  JsonDocument results{json_memory()};
  /// Extra fields of the combined result
  JsonDocument summary{json_memory()};
};

/// A keypad command on its way through the queue
struct KeypadCommand {
  std::string payload;
  enum class Phase : uint8_t { Parse, Fetch, Execute, Verify } phase{Phase::Parse};
  std::shared_ptr<KeypadBatch> batch;
  size_t batch_index{0};
  std::string name;
  /// From "timeoutMs": not executed if it waited longer than this
  uint32_t timeout_ms{0};
  /// An add that found its entry on the keypad already
  bool existed{false};
  /// A delete that only happens if the entry has run out by the time it is executed
  bool only_if_expired{false};
  std::string action;
  uint16_t code_id{0};
  nuki_lock::NewKeypadEntry add{};
  nuki_lock::UpdatedKeypadEntry update{};
  bool has_name{false}, has_code{false}, has_enabled{false}, has_time_limited{false};
  bool has_from{false}, has_until{false}, has_weekdays{false}, has_from_time{false}, has_until_time{false};
  bool fetched{false};
  /// Set when the command ends without a Bluetooth result (validation), published as is
  const char *verdict{nullptr};
  /// Echoed in the JSON result, so a client can tell its own commands from those of others
  std::string request_id;
  uint32_t received_at{0};
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
  void on_shutdown() override;
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
  void set_keypad_check_code(bool enabled) { this->keypad_check_code_ = enabled; }
  void set_keypad_fast_list(bool enabled) { this->keypad_fast_list_ = enabled; }
  void set_api_transport(bool enabled) { this->api_transport_ = enabled; }
  void set_mqtt_qos(uint8_t qos) { this->publish_qos_ = qos; }
  void set_authorization_add(bool enabled) { this->authorization_add_ = enabled; }
  /// "<topic_prefix>/<opener|lock>", what the topics of this device start with
  std::string device_path() const { return this->topic_prefix_ + "/" + this->kind(); }
  void set_keypad_topic_per_entry(bool enabled) { this->keypad_topic_per_entry_ = enabled; }
  void set_keypad_delete_expired(uint32_t after_seconds, const std::string &prefix) {
    this->keypad_delete_expired_after_ = after_seconds;
    this->keypad_delete_expired_prefix_ = prefix;
  }
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
  void set_mqtt_log_level(uint8_t level) { this->mqtt_log_level_ = level; }
  void set_log_marker_in_flash(bool enabled) { this->log_marker_in_flash_ = enabled; }
  void set_homeassistant_discovery(const std::string &prefix) { this->discovery_prefix_ = prefix; }
#ifdef USE_BINARY_SENSOR
  void set_available_binary_sensor(binary_sensor::BinarySensor *s) { this->available_binary_sensor_ = s; }
  void set_battery_critical_binary_sensor(binary_sensor::BinarySensor *s) { this->battery_critical_binary_sensor_ = s; }
  void set_keypad_battery_critical_binary_sensor(binary_sensor::BinarySensor *s) {
    this->keypad_battery_critical_binary_sensor_ = s;
  }
  void set_ring_binary_sensor(binary_sensor::BinarySensor *s) { this->ring_binary_sensor_ = s; }
  void add_topic_binary_sensor(const std::string &topic, const std::string &payload_on, binary_sensor::BinarySensor *s) {
    this->topic_binary_sensors_.push_back({topic, payload_on, s});
  }
#endif
#ifdef USE_SENSOR
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }
  void add_topic_sensor(const std::string &topic, sensor::Sensor *s) { this->topic_sensors_.emplace_back(topic, s); }
  void set_keypad_entries_sensor(sensor::Sensor *s) { this->keypad_entries_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_sensor_ = s; }
  void add_topic_text_sensor(const std::string &topic, text_sensor::TextSensor *s) {
    this->topic_text_sensors_.emplace_back(topic, s);
  }
  void set_last_user_text_sensor(text_sensor::TextSensor *s) { this->last_user_text_sensor_ = s; }
#endif
  void set_ble_disconnect_timeout(uint32_t ms) { this->ble_disconnect_timeout_ = ms; }
  void set_fast_connection(bool enabled) { this->fast_connection_ = enabled; }
  void set_bluetooth_watchdog(uint32_t seconds) { this->bluetooth_watchdog_ = seconds; }
  void set_invalid_code_alert(uint8_t count, uint32_t window_seconds) {
    this->invalid_code_alert_count_ = count;
    this->invalid_code_alert_window_ = window_seconds;
  }
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
  /// Does what a message on the command topic `topic` ("/keypad/actionJson", "/action", ...)
  /// does, without MQTT. False if there is no such topic.
  bool command(const std::string &topic, const std::string &payload);
  /// Called with the topic below `<prefix>/<kind>` and the payload of everything published
  template<typename F> void add_on_message_callback(F &&callback) {
    this->message_callback_.add(std::forward<F>(callback));
    this->has_message_callbacks_ = true;
  }
  /// The same without a copy of the payload: it is only valid during the call
  void set_raw_message_hook(std::function<void(const char *, const char *, size_t)> &&hook) {
    this->raw_message_hook_ = std::move(hook);
  }

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
  /// `timeout` in minutes for actions that take one (continuous mode of an Opener), 0 for none
  virtual bool queue_action(const std::string &action, const std::string &request_id, uint8_t timeout = 0) = 0;
  /// Reads the log entries `from`..`to` and publishes them on `logRange`
  virtual void queue_log_range(uint32_t from, uint32_t to, const std::string &request_id) = 0;
  virtual void handle_config_action(const std::string &payload) = 0;
  virtual void handle_time_control_action(const std::string &payload) = 0;
  virtual bool has_keypad() const = 0;
  virtual bool config_valid() const = 0;
  virtual void publish_unavailable() {}
  virtual bool battery_critical() = 0;
  virtual bool keypad_battery_critical() = 0;
  /// The state as Nuki Hub names it ("locked", "RTOactive", ...)
  virtual std::string state_text() const = 0;
  /// What a Home Assistant lock sends to lock, unlock and open, and whether the device rings
  virtual void describe_for_discovery(JsonObject lock) = 0;
  virtual bool has_ring() const { return false; }
  /// A setting announced to Home Assistant: a switch, a number when max > min, a select with options
  struct DiscoverySetting {
    const char *key;
    const char *name;
    bool advanced;
    int32_t min;
    int32_t max;
    /// The choices of a select, by the names the configuration topics use
    std::vector<const char *> options{};
  };
  virtual std::vector<DiscoverySetting> discovery_settings() const { return {}; }

  // --- shared machinery -----------------------------------------------------------------
  void enqueue(Operation op, bool front = false);
  /// The first tick of the next operation, picked from the queue or the pending polls
  bool start_next_operation_();
  void finish_operation_(CmdResult result);
  /// Ends an attempt that waits for a connection or an answer for too long
  CmdResult check_attempt_deadline_(uint32_t now);
  void check_beacon_age_();
  enum class Presence : uint8_t { PRESENT, WAITING, SILENT };
  Presence presence_(uint32_t now);
  void require_beacon_();
  void end_attempt_();
  void do_unpair_();
  void on_log_(uint8_t level, const char *message, size_t length);

  std::string topic_(const char *suffix) const;
  std::string hub_topic_(const char *suffix) const;
  bool publish_(const char *suffix, const std::string &payload, bool retain = true);
  bool publish_(const char *suffix, const char *payload, size_t length, bool retain);
  bool publish_(const char *suffix, const char *payload, bool retain = true);
  bool publish_int_(const char *suffix, int64_t value, bool retain = true);
  bool publish_json_(const char *suffix, JsonDocument &doc, bool retain = true);
  bool publish_hub_(const char *suffix, const std::string &payload, bool retain = true);
  void subscribe_(const char *suffix, std::function<void(const std::string &)> &&callback);
  void subscribe_hub_(const char *suffix, std::function<void(const std::string &)> &&callback);

  void register_handlers_();
  bool send_(const std::string &topic, const char *payload, size_t length, bool retain, uint8_t qos = 1);
  void flush_outbox_();
  void on_mqtt_connected_();
  void publish_maintenance_(bool initial);
  void set_available_(bool available);
  void publish_status_();
  /// The JSON form of a command result on `<topic>` next to Nuki Hub's plain word: with the
  /// request id of the command, the device's error if it refused, and how long it took
  void publish_result_(const char *topic, const std::string &request_id, const std::string &action,
                       const char *result, bool refused_by_device, uint32_t received_at,
                       const std::function<void(JsonObject)> &extra = nullptr);
  /// Publishes Nuki Hub's plain result on `<base>/commandResult` and the JSON one on `<base>/result`
  void answer_command_(const char *base, const std::string &request_id, const std::string &action,
                       const char *result, bool refused_by_device, uint32_t received_at);
  static const char *error_name(uint8_t code);

  // Keypad
  void handle_keypad_action_(const std::string &payload);
  void handle_keypad_batch_(const std::string &payload);
  void handle_keypad_sync_(const std::string &payload);
  bool enqueue_keypad_command_(const std::string &payload, const std::string &request_id,
                               std::shared_ptr<KeypadBatch> batch, size_t index);
  std::shared_ptr<KeypadBatch> start_keypad_batch_(const std::string &request_id, const char *result_topic,
                                                   const std::vector<std::string> &commands, uint32_t received_at);
  void finish_keypad_batch_(KeypadBatch &batch);
  void delete_expired_keypad_entries_();
  bool keypad_entry_expired_(const nuki_lock::KeypadEntry &entry);
  CmdResult keypad_command_step_(KeypadCommand &cmd);
  void keypad_command_done_(KeypadCommand &cmd, CmdResult result);
  Operation make_keypad_operation_();
  void publish_keypad_();
  void publish_keypad_entries_();
  void describe_availability_(JsonObject json);
  void update_topic_entities_(const char *suffix, const char *payload);
  void publish_keypad_entry_discovery_(uint16_t code_id, const std::string &name, bool remove);
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
  /// One activity log entry as an event: `event/keypad`, `event/action` or `event/ring`
  void publish_event_(const char *type, JsonObject entry);
  void note_invalid_code_();
  void check_bluetooth_watchdog_(uint32_t now);
  /// Brings the entities of the ESPHome API up to date
  void update_entities_();
  void set_ring_entity_(bool ringing);
  /// Announces the device to Home Assistant over MQTT
  void publish_discovery_(uint32_t nuki_id, const std::string &name, const std::string &firmware);
  Operation make_fingerprint_operation_();

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
  bool keypad_check_code_{false};
  bool keypad_fast_list_{false};
  bool api_transport_{false};
  bool authorization_add_{false};
  void attach_api_();
  bool keypad_topic_per_entry_{false};
  std::vector<uint16_t> keypad_published_ids_;
  std::map<uint16_t, uint32_t> keypad_published_hashes_;
  bool keypad_entries_published_{false};
  bool keypad_entries_more_{false};
  uint32_t keypad_entries_at_{0};
  bool interrupted_{false};
  uint8_t publish_qos_{0};
  bool last_publish_too_large_{false};
  uint8_t keypad_pages_published_{0};
  /// Switches in Home Assistant for the entries of a list (authorizations, time control)
  struct ListSwitches {
    const char *kind;         // "auth", "timecontrol": part of the entity's id
    const char *list_topic;   // the list the state is read from
    const char *id_field;
    const char *command_topic;
    std::vector<uint32_t> published;
    std::map<uint32_t, uint32_t> hashes;
    bool listed{false};
  };
  ListSwitches auth_switches_{"auth", "/authorization/json", "authId", "/authorization/action"};
  ListSwitches time_control_switches_{"timecontrol", "/timecontrol/json", "entryId", "/timecontrol/action"};
  void publish_list_switches_(ListSwitches &list, const std::vector<std::pair<uint32_t, std::string>> &entries);
  void publish_list_switch_(ListSwitches &list, uint32_t id, const std::string &name, bool remove);
  void publish_time_control_switches_(const std::vector<std::pair<uint32_t, std::string>> &entries) {
    this->publish_list_switches_(this->time_control_switches_, entries);
  }
  std::string discovery_device_name_;
  std::string discovery_firmware_;
  uint16_t discovery_cursor_{0};
  bool discovery_pending_{false};
  uint32_t discovery_at_{0};
  bool discovery_scan_active_{false};
  uint32_t discovery_nuki_id_{0};
  uint32_t keypad_delete_expired_after_{0};
  std::string keypad_delete_expired_prefix_;
  uint32_t keypad_expiry_not_before_{0};
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
  uint8_t mqtt_log_level_{0};
  bool log_marker_in_flash_{false};
  std::string discovery_prefix_;
  bool discovery_published_{false};
  bool want_fingerprints_{false};
  bool fingerprint_settling_{false};
  size_t fingerprint_last_size_{0};
  uint32_t fingerprint_last_change_{0};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *available_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *battery_critical_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *keypad_battery_critical_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *ring_binary_sensor_{nullptr};
  struct TopicBinarySensor {
    std::string topic;
    std::string payload_on;
    binary_sensor::BinarySensor *sensor;
  };
  std::vector<TopicBinarySensor> topic_binary_sensors_;
#endif
#ifdef USE_SENSOR
  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *keypad_entries_sensor_{nullptr};
  std::vector<std::pair<std::string, sensor::Sensor *>> topic_sensors_;
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_text_sensor_{nullptr};
  text_sensor::TextSensor *last_user_text_sensor_{nullptr};
  std::vector<std::pair<std::string, text_sensor::TextSensor *>> topic_text_sensors_;
#endif
  ESPPreferenceObject log_marker_pref_;
  uint32_t ble_disconnect_timeout_{2000};
  bool fast_connection_{true};
  // Runs the main loop without pause while a command is on its way
  HighFrequencyLoopRequester high_frequency_;
  uint32_t high_frequency_since_{0};
  uint32_t bluetooth_watchdog_{0};
  uint32_t last_success_{0};
  uint8_t invalid_code_alert_count_{0};
  uint32_t invalid_code_alert_window_{300};
  std::vector<uint32_t> invalid_code_times_;
  // Since boot, for maintenance/bluetoothJson
  uint32_t stat_commands_{0};
  uint32_t stat_failed_{0};
  uint32_t stat_retries_{0};
  uint32_t stat_total_ms_{0};
  uint32_t stat_max_ms_{0};
  uint32_t op_started_{0};
  std::string log_topic_;
  bool in_log_{false};
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
  static const size_t MAX_QUEUE = 64;
  static const size_t MAX_BATCH = 50;
  // Longest wait for the Bluetooth connection, and longest duration of one attempt
  static const uint32_t CONNECT_DEADLINE_MS = 10000;
  static const uint32_t ATTEMPT_DEADLINE_MS = 90000;
  static const int64_t BEACON_TIMEOUT_MS = 180000;
  // Listening this long for an advertisement without hearing one: the device is not there
  static const uint32_t SILENT_MS = 20000;
  static uint8_t radio_users_;
  bool need_beacon_{true};
  int64_t need_beacon_after_{0};
  uint32_t silent_listen_ms_{0};
  uint32_t presence_tick_{0};
  bool silent_logged_{false};
  uint32_t attempt_started_{0};
  bool unpair_requested_{false};
  bool attempt_connected_{false};
  bool beacon_lost_{false};
  uint32_t last_beacon_check_{0};
  uint32_t mqtt_connects_{0};

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
  CallbackManager<void(const std::string &, const std::string &)> message_callback_;
  bool has_message_callbacks_{false};
  std::function<void(const char *, const char *, size_t)> raw_message_hook_;
  struct Handler {
    std::string suffix;
    bool hub;
    std::function<void(const std::string &)> callback;
  };
  std::vector<Handler> handlers_;
  struct Outgoing {
    char *data;  // topic, 0, payload, 0
    uint32_t topic_length;
    uint32_t length;
    bool retain;
  };
  std::deque<Outgoing> outbox_;
  size_t outbox_bytes_{0};
  bool keep_(const std::string &topic, const char *payload, size_t length, bool retain);
  void drop_outgoing_(const Outgoing &message);
  uint32_t outbox_dropped_{0};
  static const size_t MAX_OUTBOX = 400;
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
/// The "requestId" of a JSON command, empty if it has none
std::string request_id(const std::string &payload);
}  // namespace util

/// A keypad entry in the JSON form of Nuki Hub
void keypad_entry_to_json(const nuki_lock::KeypadEntry &entry, JsonObject json, bool include_code);

namespace util {
}  // namespace util

}  // namespace esphome::nuki_hub
