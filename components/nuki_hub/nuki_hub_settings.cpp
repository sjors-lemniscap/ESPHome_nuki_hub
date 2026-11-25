#include "nuki_hub_settings.h"

#include "esphome/core/log.h"

#include <cmath>

namespace esphome::nuki_hub {

static const char *const TAG = "nuki_hub.setting";

bool NukiHubSettingBase::attach_(NukiHubComponent::DiscoverySetting &setting,
                                 std::function<void(JsonVariantConst)> &&on_value) {
  if (!this->parent_->setting_info(this->key_, setting)) {
    ESP_LOGE(TAG, "A Nuki %s has no setting '%s'", this->parent_->kind(), this->key_);
    return false;
  }
  const char *key = this->key_;
  this->parent_->add_json_listener(setting.advanced ? "/configuration/advancedJson" : "/configuration/basicJson",
                                   [key, on_value = std::move(on_value)](JsonObjectConst json) {
                                     JsonVariantConst value = json[key];
                                     if (!value.isNull()) {
                                       on_value(value);
                                     }
                                   });
  return true;
}

// The configuration is read again after every write, so an entity that showed a value the
// device then refused goes back to the real one within the same second.
void NukiHubSettingBase::write_(const std::string &value) {
  std::string payload = "{\"";
  payload += this->key_;
  payload += "\":\"";
  payload += value;
  payload += "\"}";
  this->parent_->command("/configuration/action", payload);
}

#ifdef USE_SWITCH
void NukiHubSettingSwitch::setup() {
  NukiHubComponent::DiscoverySetting setting;
  if (!this->attach_(setting, [this](JsonVariantConst value) { this->publish_state(value.as<int>() != 0); })) {
    this->mark_failed();
  }
}

void NukiHubSettingSwitch::write_state(bool state) {
  this->write_(state ? "1" : "0");
  this->publish_state(state);
}

void NukiHubPairingModeSwitch::setup() {
  this->publish_state(this->parent_->pairing_mode());
  this->parent_->add_on_state_callback([this] {
    if (this->state != this->parent_->pairing_mode()) {
      this->publish_state(this->parent_->pairing_mode());
    }
  });
}
#endif

#ifdef USE_NUMBER
void NukiHubSettingNumber::setup() {
  NukiHubComponent::DiscoverySetting setting;
  if (!this->attach_(setting, [this](JsonVariantConst value) { this->publish_state(value.as<float>()); })) {
    this->mark_failed();
    return;
  }
  this->traits.set_min_value(setting.min);
  this->traits.set_max_value(setting.max);
  this->traits.set_step(1);
}

void NukiHubSettingNumber::control(float value) {
  this->write_(to_string((long) lroundf(value)));
  this->publish_state(value);
}
#endif

#ifdef USE_SELECT
void NukiHubSettingSelect::setup() {
  NukiHubComponent::DiscoverySetting setting;
  if (!this->attach_(setting, [this](JsonVariantConst value) {
        const char *text = value.as<const char *>();
        if (text != nullptr && this->has_option(text)) {
          this->publish_state(text);
        }
      })) {
    this->mark_failed();
    return;
  }
  // The names are constants of the program, the table that points at them has to stay as well
  const size_t count = setting.options.size();
  const char **table = new const char *[count];
  for (size_t i = 0; i < count; i++) {
    table[i] = setting.options[i];
  }
  this->traits.set_options_static(table, count);
}

void NukiHubSettingSelect::control(size_t index) {
  this->write_(this->option_at(index));
  this->publish_state(index);
}
#endif

#ifdef USE_BUTTON
void NukiHubButton::press_action() {
  if (this->topic_ == nullptr) {
    this->parent_->unpair();
  } else {
    this->parent_->command(this->topic_, this->payload_);
  }
}
#endif

}  // namespace esphome::nuki_hub
