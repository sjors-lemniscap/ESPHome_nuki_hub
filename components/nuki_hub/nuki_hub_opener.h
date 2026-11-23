#pragma once

#include "nuki_hub.h"
#include "esphome/components/nuki_lock/nuki_opener_protocol.h"

namespace esphome::nuki_hub {

namespace opener = nuki_lock::opener;

/// A Nuki Opener, published below `<prefix>/opener`
class NukiHubOpener : public NukiHubComponent {
 public:
  explicit NukiHubOpener(const std::string &device_name)
      : nuki_opener_(device_name, fnv1_hash(device_name)) {}

  nuki_lock::NukiBle *ble() override { return &this->nuki_opener_; }
  opener::NukiOpener *get_nuki_opener() { return &this->nuki_opener_; }
  const char *kind() const override { return "opener"; }

  const opener::OpenerState &state() const { return this->state_; }
  bool state_known() const { return this->state_known_; }

  lock::LockState entity_state() const override;
  const char *entity_action(bool lock, bool open) const override {
    return open ? "electricStrikeActuation" : (lock ? "deactivateRTO" : "activateRTO");
  }

 protected:
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
    return this->config_valid_ && (this->config_.hasKeypad != 0 ||
                                   (this->config_.hasKeypadV2 != 0 && this->config_.hasKeypadV2 != 255));
  }
  bool config_valid() const override { return this->config_valid_; }
  void publish_unavailable() override;
  bool battery_critical() override { return this->nuki_opener_.is_battery_critical(); }
  bool keypad_battery_critical() override { return this->nuki_opener_.is_keypad_battery_critical(); }
  std::string state_text() const override;
  void describe_for_discovery(JsonObject lock) override;
  bool has_ring() const override { return true; }
  std::vector<DiscoverySetting> discovery_settings() const override {
    return {
        {"ledFlashEnabled", "LED enabled", false, 0, 0},
        {"buttonEnabled", "Button enabled", false, 0, 0},
        {"soundConfirmation", "Sound confirmation", true, 0, 0},
        {"disableRtoAfterRing", "Disable RTO after ring", true, 0, 0},
        {"randomElectricStrikeDelay", "Random electric strike delay", true, 0, 0},
        {"soundLevel", "Sound level", true, 0, 255},
        {"rtoTimeout", "RTO timeout", true, 5, 60},
        {"electricStrikeDelay", "Electric strike delay", true, 0, 30000},
        {"electricStrikeDuration", "Electric strike duration", true, 1000, 30000},
        {"doorbellSuppressionDuration", "Doorbell suppression duration", true, 500, 10000},
    };
  }

  void publish_state_();
  void publish_config_();
  void publish_advanced_config_();
  void publish_time_control_();
  void process_log_();
  void log_entry_to_json_(const opener::LogEntry &entry, JsonObject json);
  void publish_ring_(bool locked);

  opener::NukiOpener nuki_opener_;

  opener::OpenerState state_;
  bool state_known_{false};
  opener::Config config_;
  opener::AdvancedConfig advanced_config_;
  bool config_valid_{false};
  bool advanced_config_valid_{false};
  uint8_t config_stage_{0};
  opener::BatteryReport battery_report_;
  std::vector<opener::TimeControlEntry> time_control_entries_;

  // Activity log read in progress
  std::vector<opener::LogEntry> log_batch_;
  uint8_t log_stage_{0};
  uint32_t log_fill_start_{0};
  uint16_t log_fill_count_{0};
};

}  // namespace esphome::nuki_hub
