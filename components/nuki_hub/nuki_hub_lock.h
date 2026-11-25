#pragma once

#include "nuki_hub.h"
#include "nuki_lock_protocol.h"

namespace esphome::nuki_hub {

/// A Nuki Smart Lock, published below `<prefix>/lock`
class NukiHubLock : public NukiHubComponent {
 public:
  explicit NukiHubLock(const std::string &device_name) : nuki_lock_(device_name, fnv1_hash(device_name)) {}

  NukiBle *ble() override { return &this->nuki_lock_; }
  NukiLock *get_nuki_lock() { return &this->nuki_lock_; }
  const char *kind() const override { return "lock"; }

  const KeyTurnerState &state() const { return this->state_; }
  bool state_known() const { return this->state_known_; }

  /// Hybrid mode: the lock's own MQTT (Nuki's official API over Wi-Fi or Thread) for state
  /// changes and, if asked for, lock actions. Everything else stays on Bluetooth.
  void set_hybrid(bool send_actions, bool ble_fallback, uint32_t fallback_timeout, uint32_t offline_update_interval) {
    this->hybrid_ = true;
    this->hybrid_send_actions_ = send_actions;
    this->hybrid_ble_fallback_ = ble_fallback;
    this->hybrid_fallback_timeout_ = fallback_timeout;
    this->hybrid_offline_interval_ = offline_update_interval;
  }
  void set_hybrid_topic_prefix(const std::string &prefix) { this->hybrid_prefix_ = prefix; }
  bool hybrid_connected() const { return this->hybrid_connected_; }

  void loop() override;

  lock::LockState entity_state() const override;
  const char *entity_action(bool lock, bool open) const override {
    return open ? "unlatch" : (lock ? "lock" : "unlock");
  }

 protected:
  void setup_device() override;
  Operation make_state_operation() override;
  Operation make_config_operation() override;
  Operation make_battery_operation() override;
  Operation make_time_control_operation() override;
  Operation make_log_operation() override;
  bool queue_action(const std::string &action, const std::string &request_id, uint8_t timeout = 0) override;
  void queue_log_range(uint32_t from, uint32_t to, const std::string &request_id) override;
  void handle_config_action(const std::string &payload) override;
  void handle_time_control_action(const std::string &payload) override;
  bool has_keypad() const override {
    return this->config_valid_ && ((this->config_.hasKeypad != 0 && this->config_.hasKeypad != 255) ||
                                   (this->config_.hasKeypadV2 != 0 && this->config_.hasKeypadV2 != 255));
  }
  bool config_valid() const override { return this->config_valid_; }
  void publish_unavailable() override;
  bool battery_critical() override { return this->nuki_lock_.is_battery_critical(); }
  bool keypad_battery_critical() override { return this->nuki_lock_.is_keypad_battery_critical(); }
  std::string state_text() const override;
  void describe_for_discovery(JsonObject lock) override;
  std::vector<DiscoverySetting> discovery_settings() const override;

  void publish_state_();
  void publish_config_();
  void publish_advanced_config_();
  void publish_time_control_();
  void process_log_();
  void log_entry_to_json_(const LogEntry &entry, JsonObject json);
  /// Waits until a list the lock keeps sending after its first answer has stopped growing
  CmdResult settle_list_(CmdResult result, size_t size);

  void queue_ble_action_(LockAction action, const std::string &text, const std::string &request_id,
                         uint32_t received_at);
  void hybrid_subscribe_();
  void hybrid_on_message_(const char *key, const std::string &payload);
  void hybrid_set_connected_(bool connected);
  bool hybrid_send_action_(LockAction action, const std::string &text, const std::string &request_id,
                           uint32_t received_at);
  void hybrid_action_confirmed_();
  void hybrid_action_failed_(const char *reason);

  NukiLock nuki_lock_;

  bool hybrid_{false};
  bool hybrid_send_actions_{false};
  bool hybrid_ble_fallback_{true};
  bool hybrid_subscribed_{false};
  bool hybrid_connected_{false};
  bool hybrid_action_pending_{false};
  uint32_t hybrid_fallback_timeout_{2000};
  uint32_t hybrid_offline_interval_{600000};
  uint32_t hybrid_last_offline_update_{0};
  uint32_t hybrid_action_sent_at_{0};
  uint32_t hybrid_action_received_at_{0};
  LockAction hybrid_pending_action_{};
  std::string hybrid_pending_text_;
  std::string hybrid_pending_request_id_;
  std::string hybrid_prefix_;

  KeyTurnerState state_;
  bool state_known_{false};
  Config config_;
  AdvancedConfig advanced_config_;
  bool config_valid_{false};
  bool advanced_config_valid_{false};
  uint8_t config_stage_{0};
  BatteryReport battery_report_;
  std::vector<TimeControlEntry> time_control_entries_;

  std::vector<LogEntry> log_batch_;
  uint8_t log_stage_{0};
  uint32_t log_fill_start_{0};
  uint16_t log_fill_count_{0};

  bool list_settling_{false};
  size_t list_last_size_{0};
  uint32_t list_last_change_{0};
};

}  // namespace esphome::nuki_hub
