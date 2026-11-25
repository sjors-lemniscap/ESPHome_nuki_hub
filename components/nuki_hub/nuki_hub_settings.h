#pragma once

#include "nuki_hub.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

namespace esphome::nuki_hub {

// The settings of a Nuki device as entities of the ESPHome API. Each follows one key of the
// configuration topics and writes through configuration/action, like a client on MQTT does;
// its range or choices come from the same table Home Assistant's MQTT discovery uses.

class NukiHubSettingBase {
 public:
  void set_parent(NukiHubComponent *parent) { this->parent_ = parent; }
  void set_key(const char *key) { this->key_ = key; }

 protected:
  /// Looks the setting up and listens to its topic. False if the device has no such setting.
  bool attach_(NukiHubComponent::DiscoverySetting &setting, std::function<void(JsonVariantConst)> &&on_value);
  void write_(const std::string &value);

  NukiHubComponent *parent_{nullptr};
  const char *key_{""};
};

#ifdef USE_SWITCH
class NukiHubSettingSwitch : public switch_::Switch, public Component, public NukiHubSettingBase {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void write_state(bool state) override;
};

class NukiHubPairingModeSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(NukiHubComponent *parent) { this->parent_ = parent; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void write_state(bool state) override { this->parent_->set_pairing_mode(state); }

  NukiHubComponent *parent_{nullptr};
};
#endif

#ifdef USE_NUMBER
class NukiHubSettingNumber : public number::Number, public Component, public NukiHubSettingBase {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void control(float value) override;
};
#endif

#ifdef USE_SELECT
class NukiHubSettingSelect : public select::Select, public Component, public NukiHubSettingBase {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void control(size_t index) override;
};
#endif

#ifdef USE_BUTTON
/// Sends one command, as a message on a command topic would, or removes the pairing
class NukiHubButton : public button::Button {
 public:
  void set_parent(NukiHubComponent *parent) { this->parent_ = parent; }
  void set_command(const char *topic, const char *payload) {
    this->topic_ = topic;
    this->payload_ = payload;
  }

 protected:
  void press_action() override;

  NukiHubComponent *parent_{nullptr};
  const char *topic_{nullptr};
  const char *payload_{nullptr};
};
#endif

}  // namespace esphome::nuki_hub
