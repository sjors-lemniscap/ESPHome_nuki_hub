#include "nuki_hub.h"

#ifdef USE_NUKI_HUB_API
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/log.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include "esp_heap_caps.h"

#include <cstring>
#include <deque>
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
    component->set_raw_message_hook([this, device](const char *topic, const char *payload, size_t length) {
      if (!this->is_connected()) {
        return;
      }
      // The API drops an event a client's connection has no room for at that moment, and a
      // dropped result leaves the client waiting. So an event that cannot go out now waits
      // here, and so does every one after it: they arrive in the order they were made.
      if (this->waiting_.empty() && this->room_()) {
        this->send_(device.c_str(), topic, payload, length);
      } else {
        this->keep_(device, topic, payload, length);
      }
    });
  }

  /// Called from the loop: sends what waited, as far as there is room
  void flush() {
    while (!this->waiting_.empty()) {
      Waiting &event = this->waiting_.front();
      const bool expired = millis() - event.since > 30000;
      if (!this->is_connected() || expired) {
        // No client, or one that takes nothing any more: not worth the memory
        if (expired && this->is_connected()) {
          this->dropped_++;
        }
      } else if (!this->room_()) {
        return;
      } else {
        const char *topic = event.data + event.device_length + 1;
        this->send_(event.data, topic, topic + event.topic_length + 1, event.length);
      }
      this->release_(event);
      this->waiting_.pop_front();
    }
    if (this->dropped_ > 0) {
      ESP_LOGW(TAG, "%u events not sent: a client did not take them for 30 seconds", (unsigned) this->dropped_);
      this->dropped_ = 0;
    }
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
  struct Waiting {
    char *data;  // device, 0, topic, 0, payload, 0
    uint16_t device_length;
    uint16_t topic_length;
    uint32_t length;
    uint32_t since;
  };
  static size_t size_(const Waiting &event) {
    return event.device_length + 1 + event.topic_length + 1 + event.length + 1;
  }

  /// Whether every client can take a message now. A client that takes nothing for three
  /// seconds in a row (gone without closing, most of the time) no longer holds up the others:
  /// what is sent then is lost for that one only.
  bool room_() {
    for (auto &client : api::global_api_server->active_clients()) {
      if (!client->try_to_clear_buffer(false)) {
        const uint32_t now = millis();
        if (this->blocked_since_ == 0) {
          this->blocked_since_ = now | 1;
        }
        return now - this->blocked_since_ > 3000;
      }
    }
    this->blocked_since_ = 0;
    return true;
  }

  void send_(const char *device, const char *topic, const char *payload, size_t length) {
    // As fire_homeassistant_event() does, without the copies a map of strings would make
    api::HomeassistantActionRequest event;
    event.service = ProgmemStringRef(StringRef("esphome.nuki_hub"));
    event.is_event = true;
    event.data.init(3);
    auto add = [&event](const char *key, StringRef value) {
      auto &pair = event.data.emplace_back();
      pair.key = ProgmemStringRef(StringRef(key));
      pair.value = ProgmemStringRef(value);
    };
    add("device", StringRef(device));
    add("topic", StringRef(topic));
    add("payload", StringRef(payload, length));
    api::global_api_server->send_homeassistant_action(event);
  }

  void keep_(const std::string &device, const char *topic, const char *payload, size_t length) {
    static const size_t limit = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? 512 * 1024 : 16 * 1024;
    Waiting event{nullptr, (uint16_t) device.size(), (uint16_t) strlen(topic), (uint32_t) length, millis()};
    const size_t size = size_(event);
    RAMAllocator<char> allocator;
    event.data = this->waiting_.size() < 300 && this->waiting_bytes_ + size <= limit ? allocator.allocate(size) : nullptr;
    if (event.data == nullptr) {
      this->dropped_++;
      return;
    }
    memcpy(event.data, device.c_str(), device.size() + 1);
    memcpy(event.data + device.size() + 1, topic, event.topic_length + 1);
    if (length > 0) {
      memcpy(event.data + device.size() + 1 + event.topic_length + 1, payload, length);
    }
    event.data[size - 1] = 0;
    this->waiting_.push_back(event);
    this->waiting_bytes_ += size;
  }

  void release_(const Waiting &event) {
    RAMAllocator<char> allocator;
    allocator.deallocate(event.data, size_(event));
    this->waiting_bytes_ -= size_(event);
  }

  std::vector<NukiHubComponent *> components_;
  std::deque<Waiting> waiting_;
  size_t waiting_bytes_{0};
  uint32_t dropped_{0};
  uint32_t blocked_since_{0};
};

static NukiHubApi *api_device() {
  static NukiHubApi api;
  return &api;
}

void NukiHubComponent::attach_api_() { api_device()->attach(this); }

void NukiHubComponent::flush_api_() { api_device()->flush(); }

}  // namespace esphome::nuki_hub
#endif
