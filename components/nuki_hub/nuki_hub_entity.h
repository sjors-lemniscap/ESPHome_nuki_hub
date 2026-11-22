#pragma once

#include "nuki_hub.h"
#include "esphome/components/lock/lock.h"

namespace esphome::nuki_hub {

/// The device of a nuki_hub entry as a lock entity. For an Opener, locked means ring to open
/// is off, unlocked that it is on (or continuous mode), and "open" actuates the strike.
class NukiHubLockEntity : public lock::Lock, public Component {
 public:
  void set_parent(NukiHubComponent *parent) { this->parent_ = parent; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void control(const lock::LockCall &call) override;
  void open_latch() override;

  NukiHubComponent *parent_{nullptr};
};

}  // namespace esphome::nuki_hub
