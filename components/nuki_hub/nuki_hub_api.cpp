#include "nuki_hub.h"

#ifdef USE_NUKI_HUB_API
#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/log.h"

#include <map>
#include <vector>

namespace esphome::nuki_hub {

static const char *const TAG = "nuki_hub.api";

// The topics of Nuki Hub over the ESPHome API instead of a broker. A client calls the action
// nuki_hub_command(device, topic, payload), with `device` being "<topic_prefix>/<opener|lock>"
// and `topic` what follows it on MQTT ("/keypad/actionJson"). Everything a device publishes
// reaches the client as the event esphome.nuki_hub with the same three values.
class NukiHubApi : public api::CustomAPIDevice {
 public:
  void attach(NukiHubComponent *component) {
    if (this->components_.empty()) {
      this->register_service(&NukiHubApi::on_command, "nuki_hub_command", {"device", "topic", "payload"});
    }
    this->components_.push_back(component);
    const std::string device = component->device_path();
    component->add_on_message_callback([this, device](const std::string &topic, const std::string &payload) {
      if (!this->is_connected()) {
        return;
      }
      this->fire_homeassistant_event("esphome.nuki_hub",
                                     {{"device", device}, {"topic", topic}, {"payload", payload}});
    });
  }

  void on_command(std::string device, std::string topic, std::string payload) {
    for (auto *component : this->components_) {
      if (component->device_path() == device) {
        component->command(topic, payload);
        return;
      }
    }
    ESP_LOGW(TAG, "No device '%s'", device.c_str());
  }

 protected:
  std::vector<NukiHubComponent *> components_;
};

void NukiHubComponent::attach_api_() {
  static NukiHubApi api;
  api.attach(this);
}

}  // namespace esphome::nuki_hub
#endif
