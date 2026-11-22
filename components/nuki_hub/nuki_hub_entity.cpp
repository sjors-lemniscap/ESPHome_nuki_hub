#include "nuki_hub_entity.h"

#include "esphome/core/log.h"

namespace esphome::nuki_hub {

static const char *const TAG = "nuki_hub.lock_entity";

void NukiHubLockEntity::setup() {
  this->traits.set_supports_open(true);
  this->traits.set_supported_states({lock::LOCK_STATE_NONE, lock::LOCK_STATE_LOCKED, lock::LOCK_STATE_UNLOCKED,
                                     lock::LOCK_STATE_JAMMED, lock::LOCK_STATE_LOCKING, lock::LOCK_STATE_UNLOCKING});
  this->publish_state(lock::LOCK_STATE_NONE);
  this->parent_->add_on_state_callback([this] {
    this->publish_state(this->parent_->is_available() ? this->parent_->entity_state() : lock::LOCK_STATE_NONE);
  });
}

void NukiHubLockEntity::dump_config() { LOG_LOCK("", "Nuki Hub Lock", this); }

void NukiHubLockEntity::control(const lock::LockCall &call) {
  auto state = call.get_state();
  if (!state.has_value()) {
    return;
  }
  const char *action = this->parent_->entity_action(*state == lock::LOCK_STATE_LOCKED, false);
  if (action == nullptr || !this->parent_->request_action(action)) {
    ESP_LOGW(TAG, "Action not available");
  }
}

void NukiHubLockEntity::open_latch() {
  const char *action = this->parent_->entity_action(false, true);
  if (action == nullptr || !this->parent_->request_action(action)) {
    ESP_LOGW(TAG, "Action not available");
  }
}

}  // namespace esphome::nuki_hub
