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

/// Fires for everything the device publishes: `topic` below `<prefix>/<kind>`, and the payload
class NukiHubMessageTrigger : public Trigger<std::string, std::string> {
 public:
  explicit NukiHubMessageTrigger(NukiHubComponent *parent) {
    parent->add_on_message_callback([this](std::string topic, std::string payload) { this->trigger(topic, payload); });
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
