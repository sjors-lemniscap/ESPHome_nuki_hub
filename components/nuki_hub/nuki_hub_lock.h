#pragma once

#include "nuki_hub.h"
#include "esphome/components/nuki_lock/nuki_lock_protocol.h"

namespace esphome::nuki_hub {

/// A Nuki Smart Lock, published below `<prefix>/lock`
class NukiHubLock : public NukiHubComponent {
 public:
  explicit NukiHubLock(const std::string &device_name) : nuki_lock_(device_name, fnv1_hash(device_name)) {}

  nuki_lock::NukiBle *ble() override { return &this->nuki_lock_; }
  nuki_lock::NukiLock *get_nuki_lock() { return &this->nuki_lock_; }
  const char *kind() const override { return "lock"; }

  const nuki_lock::KeyTurnerState &state() const { return this->state_; }
  bool state_known() const { return this->state_known_; }

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
  bool queue_action(const std::string &action, const std::string &request_id) override;
  void handle_config_action(const std::string &payload) override;
  void handle_time_control_action(const std::string &payload) override;
  bool has_keypad() const override {
    return this->config_valid_ && ((this->config_.hasKeypad != 0 && this->config_.hasKeypad != 255) ||
                                   (this->config_.hasKeypadV2 != 0 && this->config_.hasKeypadV2 != 255));
  }
  bool config_valid() const override { return this->config_valid_; }
  void publish_unavailable() override;

  void publish_state_();
  void publish_config_();
  void publish_advanced_config_();
  void publish_time_control_();
  void process_log_();
  void log_entry_to_json_(const nuki_lock::LogEntry &entry, JsonObject json);
  /// Waits until a list the lock keeps sending after its first answer has stopped growing
  CmdResult settle_list_(CmdResult result, size_t size);

  nuki_lock::NukiLock nuki_lock_;

  nuki_lock::KeyTurnerState state_;
  bool state_known_{false};
  nuki_lock::Config config_;
  nuki_lock::AdvancedConfig advanced_config_;
  bool config_valid_{false};
  bool advanced_config_valid_{false};
  uint8_t config_stage_{0};
  nuki_lock::BatteryReport battery_report_;
  std::vector<nuki_lock::TimeControlEntry> time_control_entries_;

  std::vector<nuki_lock::LogEntry> log_batch_;
  uint8_t log_stage_{0};
  uint32_t log_fill_start_{0};
  uint16_t log_fill_count_{0};

  bool list_settling_{false};
  size_t list_last_size_{0};
  uint32_t list_last_change_{0};
};

}  // namespace esphome::nuki_hub
