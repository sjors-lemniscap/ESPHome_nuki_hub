#pragma once

#include "esphome/core/automation.h"
#include "nuki_hub.h"

namespace esphome::nuki_hub {

template<typename... Ts> class NukiHubActionAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, action)
  void play(const Ts &...x) override { this->parent_->request_action(this->action_.value(x...)); }
};

template<typename... Ts> class NukiHubPairingModeAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  TEMPLATABLE_VALUE(bool, pairing_mode)
  void play(const Ts &...x) override { this->parent_->set_pairing_mode(this->pairing_mode_.value(x...)); }
};

template<typename... Ts> class NukiHubUnpairAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->unpair(); }
};

template<typename... Ts> class NukiHubCommandAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, topic)
  TEMPLATABLE_VALUE(std::string, payload)
  void play(const Ts &...x) override { this->parent_->command(this->topic_.value(x...), this->payload_.value(x...)); }
};

template<typename... Ts> class NukiHubActionOptionsAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  TEMPLATABLE_VALUE(bool, force)
  TEMPLATABLE_VALUE(bool, auto_unlock)
  TEMPLATABLE_VALUE(std::string, name_suffix)
  void play(const Ts &...x) override {
    this->parent_->set_action_options(this->force_.value(x...), this->auto_unlock_.value(x...),
                                      this->name_suffix_.value(x...));
  }
};

template<typename... Ts> class NukiHubSecurityPinAction : public Action<Ts...>, public Parented<NukiHubComponent> {
 public:
  TEMPLATABLE_VALUE(uint32_t, security_pin)
  void play(const Ts &...x) override { this->parent_->override_security_pin(this->security_pin_.value(x...)); }
};

/// Fires when a pairing was made (not at a start with a pairing that is already there)
class NukiHubPairedTrigger : public Trigger<> {
 public:
  explicit NukiHubPairedTrigger(NukiHubComponent *parent) {
    parent->add_on_paired_callback([this] { this->trigger(); });
  }
};

/// Fires when pairing mode goes on (or, with `on` false, off)
class NukiHubPairingModeTrigger : public Trigger<> {
 public:
  NukiHubPairingModeTrigger(NukiHubComponent *parent, bool on) {
    parent->add_on_pairing_mode_callback([this, on](bool enabled) {
      if (enabled == on) {
        this->trigger();
      }
    });
  }
};

/// Fires for everything the device publishes: `topic` below `<prefix>/<kind>`, and the payload
class NukiHubMessageTrigger : public Trigger<std::string, std::string> {
 public:
  explicit NukiHubMessageTrigger(NukiHubComponent *parent) {
    parent->add_on_message_callback([this](const std::string &topic, const std::string &payload) { this->trigger(topic, payload); });
  }
  void set_topic(const std::string &topic) { this->topic_ = topic; }

 protected:
  std::string topic_;
};

template<typename... Ts> class NukiHubPairedCondition : public Condition<Ts...>, public Parented<NukiHubComponent> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_paired(); }
};

template<typename... Ts> class NukiHubAvailableCondition : public Condition<Ts...>, public Parented<NukiHubComponent> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_available(); }
};

}  // namespace esphome::nuki_hub
