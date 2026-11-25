#include "nuki_hub.h"

#include "esp_heap_caps.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::nuki_hub {


static const char *const TAG = "nuki_hub.keypad";

// --------------------------------------------------------------------------------------------
// Keypad
// --------------------------------------------------------------------------------------------

const KeypadEntry *NukiHubComponent::find_keypad_entry_(uint16_t code_id) const {
  for (const auto &entry : this->keypad_entries_) {
    if (entry.codeId == code_id) {
      return &entry;
    }
  }
  return nullptr;
}

Operation NukiHubComponent::make_keypad_operation_() {
  Operation op;
  op.name = "read keypad codes";
  op.step = [this] { return this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_); };
  op.done = [this](CmdResult result) {
    if (result != CmdResult::Success) {
      return;
    }
    this->ble()->get_keypad_entries(&this->keypad_entries_);
    std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
              [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
    this->keypad_cache_valid_ = true;
    this->keypad_cache_at_ = millis();
    ESP_LOGI(TAG, "%s keypad codes: %u", this->kind(), (unsigned) this->keypad_entries_.size());
    this->publish_keypad_();
    this->delete_expired_keypad_entries_();
  };
  return op;
}

void keypad_entry_to_json(const KeypadEntry &entry, JsonObject json, bool include_code) {
  char dt[20];
  char tod[6];
  json["codeId"] = entry.codeId;
  if (include_code) {
    json["code"] = entry.code;
  }
  json["enabled"] = entry.enabled;
  json["name"] = util::fixed_string(entry.name, sizeof(entry.name));
  util::date_time(dt, entry.dateCreatedYear, entry.dateCreatedMonth, entry.dateCreatedDay, entry.dateCreatedHour,
                  entry.dateCreatedMin, entry.dateCreatedSec);
  json["dateCreated"] = dt;
  json["lockCount"] = entry.lockCount;
  util::date_time(dt, entry.dateLastActiveYear, entry.dateLastActiveMonth, entry.dateLastActiveDay,
                  entry.dateLastActiveHour, entry.dateLastActiveMin, entry.dateLastActiveSec);
  json["dateLastActive"] = dt;
  json["timeLimited"] = entry.timeLimited;
  util::date_time(dt, entry.allowedFromYear, entry.allowedFromMonth, entry.allowedFromDay, entry.allowedFromHour,
                  entry.allowedFromMin, entry.allowedFromSec);
  json["allowedFrom"] = dt;
  util::date_time(dt, entry.allowedUntilYear, entry.allowedUntilMonth, entry.allowedUntilDay, entry.allowedUntilHour,
                  entry.allowedUntilMin, entry.allowedUntilSec);
  json["allowedUntil"] = dt;
  util::weekdays_to_json(entry.allowedWeekdays, json["allowedWeekdays"].to<JsonArray>());
  util::time_of_day(tod, entry.allowedFromTimeHour, entry.allowedFromTimeMin);
  json["allowedFromTime"] = tod;
  util::time_of_day(tod, entry.allowedUntilTimeHour, entry.allowedUntilTimeMin);
  json["allowedUntilTime"] = tod;
}

void NukiHubComponent::publish_keypad_() {
  JsonDocument doc(json_memory());
  JsonArray list = doc.to<JsonArray>();
  for (const auto &entry : this->keypad_entries_) {
    keypad_entry_to_json(entry, list.add<JsonObject>(), this->keypad_publish_code_);
  }
  this->publish_json_("/keypad/json", doc);
  // A list too large for the memory of this board goes out in pages of 10 entries,
  // keypad/page/<n>, with their number on keypad/pages
  uint8_t pages = 0;
  if (this->last_publish_too_large_) {
    char topic[24];
    const size_t count = this->keypad_entries_.size();
    for (size_t first = 0; first < count; first += 10) {
      // One after the other with time for the MQTT client in between: all at once is as
      // much in its queue as the whole list would have been
      const uint8_t number = pages++;
      // By number, not by a name in a buffer that the next round overwrites
      this->set_timeout(0x4B500000u + number, 50 + 250 * number, [this, first, number] {
        JsonDocument page(json_memory());
        JsonArray part = page.to<JsonArray>();
        for (size_t i = first; i < this->keypad_entries_.size() && i < first + 10; i++) {
          keypad_entry_to_json(this->keypad_entries_[i], part.add<JsonObject>(), this->keypad_publish_code_);
        }
        char page_topic[24];
        snprintf(page_topic, sizeof(page_topic), "/keypad/page/%u", number);
        this->publish_json_(page_topic, page);
      });
    }
  }
  if (pages > 0 || this->keypad_pages_published_ > 0) {
    char topic[24];
    for (uint8_t page = pages; page < this->keypad_pages_published_; page++) {
      snprintf(topic, sizeof(topic), "/keypad/page/%u", page);
      this->publish_(topic, "");
    }
    this->publish_int_("/keypad/pages", pages);
    this->keypad_pages_published_ = pages;
  }
  this->publish_int_("/keypad/count", this->keypad_entries_.size());
  this->update_entities_();
  if (this->keypad_topic_per_entry_) {
    this->publish_keypad_entries_();
  }
}

// One retained topic per entry, keypad/codes/<code id>, and with Home Assistant discovery a
// switch that enables and disables the entry. Topics of entries that are gone are cleared.
void NukiHubComponent::publish_keypad_entries_() {
  std::vector<uint16_t> current;
  char topic[40];
  // A few at a time, and only with memory to spare: every message waits in the internal RAM
  // until the broker has it, a keypad with 80 entries is 160 of them. loop() comes back for
  // the rest; what was published is skipped by its hash.
  uint8_t budget = 6;
  this->keypad_entries_more_ = false;
  this->keypad_entries_at_ = millis();
  for (const auto &entry : this->keypad_entries_) {
    current.push_back(entry.codeId);
    if (this->keypad_entries_more_) {
      continue;
    }
    JsonDocument doc(json_memory());
    JsonObject json = doc.to<JsonObject>();
    keypad_entry_to_json(entry, json, this->keypad_publish_code_);
    std::string out;
    serializeJson(doc, out);
    // Only what changed: with every entry on every change, one command to a keypad with 25
    // entries would be 50 messages
    const uint32_t hash = fnv1_hash(out);
    auto known = this->keypad_published_hashes_.find(entry.codeId);
    if (known != this->keypad_published_hashes_.end() && known->second == hash) {
      continue;
    }
    if (budget == 0 || this->outbox_.size() >= 16 || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 40000) {
      this->keypad_entries_more_ = true;
      continue;
    }
    budget--;
    this->keypad_published_hashes_[entry.codeId] = hash;
    snprintf(topic, sizeof(topic), "/keypad/codes/%u", entry.codeId);
    this->publish_(topic, out);
    this->publish_keypad_entry_discovery_(entry.codeId, util::fixed_string(entry.name, sizeof(entry.name)), false);
  }
  for (uint16_t code_id : this->keypad_published_ids_) {
    if (std::find(current.begin(), current.end(), code_id) == current.end()) {
      snprintf(topic, sizeof(topic), "/keypad/codes/%u", code_id);
      this->publish_(topic, "");
      this->publish_keypad_entry_discovery_(code_id, "", true);
      this->keypad_published_hashes_.erase(code_id);
    }
  }
  this->keypad_published_ids_ = std::move(current);
  this->keypad_entries_published_ = true;
}

void NukiHubComponent::handle_keypad_action_(const std::string &payload) {
  const std::string request_id = util::request_id(payload);
  if (!this->is_paired()) {
    this->publish_("/keypad/commandResultJson", "notPaired");
    this->publish_result_("/keypad/result", request_id, "", "notPaired", false, millis());
    return;
  }
  if (!this->enqueue_keypad_command_(payload, request_id, nullptr, 0)) {
    this->publish_("/keypad/commandResultJson", "busy");
    this->publish_result_("/keypad/result", request_id, "", "busy", false, millis());
  }
}

bool NukiHubComponent::enqueue_keypad_command_(const std::string &payload, const std::string &request_id,
                                               std::shared_ptr<KeypadBatch> batch, size_t index) {
  if (this->queue_.size() >= MAX_QUEUE) {
    return false;
  }
  auto cmd = std::make_shared<KeypadCommand>();
  cmd->payload = payload;
  cmd->request_id = request_id;
  cmd->received_at = batch ? batch->received_at : millis();
  cmd->batch = std::move(batch);
  cmd->batch_index = index;

  Operation op;
  op.name = "keypad command";
  op.step = [this, cmd] { return this->keypad_command_step_(*cmd); };
  op.done = [this, cmd](CmdResult result) { this->keypad_command_done_(*cmd, result); };
  this->keypad_commands_pending_++;
  this->enqueue(std::move(op));
  return true;
}

void NukiHubComponent::keypad_command_done_(KeypadCommand &cmd, CmdResult result) {
  if (this->keypad_commands_pending_ > 0) {
    this->keypad_commands_pending_--;
  }
  const char *text = cmd.verdict != nullptr ? cmd.verdict : result_to_string(result);
  bool publish_list = false;
  if (cmd.verdict != nullptr) {
    ESP_LOGI(TAG, "Keypad command not executed: %s", cmd.verdict);
  }
  if (cmd.verdict == nullptr) {
    ESP_LOGI(TAG, "%s keypad code: %s", cmd.action.c_str(), text);
    if (result == CmdResult::Success) {
      if (cmd.action == "delete") {
        this->keypad_entries_.erase(std::remove_if(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                                                   [&cmd](const KeypadEntry &e) { return e.codeId == cmd.code_id; }),
                                    this->keypad_entries_.end());
      } else if (cmd.action == "add") {
        // The device answers an add with the id it gave the code. Without it the id is
        // only known after the next read.
        if (!cmd.existed) {
          cmd.code_id = this->ble()->get_last_keypad_code_id();
        }
        if (cmd.existed) {
          // Found in the list read just now
        } else if (cmd.code_id != 0 && this->find_keypad_entry_(cmd.code_id) == nullptr) {
          KeypadEntry e{};
          e.codeId = cmd.code_id;
          e.code = cmd.add.code;
          memcpy(e.name, cmd.add.name, sizeof(e.name));
          e.enabled = 1;
          e.timeLimited = cmd.add.timeLimited;
          e.allowedFromYear = cmd.add.allowedFromYear;
          e.allowedFromMonth = cmd.add.allowedFromMonth;
          e.allowedFromDay = cmd.add.allowedFromDay;
          e.allowedFromHour = cmd.add.allowedFromHour;
          e.allowedFromMin = cmd.add.allowedFromMin;
          e.allowedFromSec = cmd.add.allowedFromSec;
          e.allowedUntilYear = cmd.add.allowedUntilYear;
          e.allowedUntilMonth = cmd.add.allowedUntilMonth;
          e.allowedUntilDay = cmd.add.allowedUntilDay;
          e.allowedUntilHour = cmd.add.allowedUntilHour;
          e.allowedUntilMin = cmd.add.allowedUntilMin;
          e.allowedUntilSec = cmd.add.allowedUntilSec;
          e.allowedWeekdays = cmd.add.allowedWeekdays;
          e.allowedFromTimeHour = cmd.add.allowedFromTimeHour;
          e.allowedFromTimeMin = cmd.add.allowedFromTimeMin;
          e.allowedUntilTimeHour = cmd.add.allowedUntilTimeHour;
          e.allowedUntilTimeMin = cmd.add.allowedUntilTimeMin;
          this->keypad_entries_.push_back(e);
        } else {
          this->keypad_cache_valid_ = false;
        }
      } else {
        for (auto &e : this->keypad_entries_) {
          if (e.codeId != cmd.update.codeId) {
            continue;
          }
          e.code = cmd.update.code;
          memcpy(e.name, cmd.update.name, sizeof(e.name));
          e.enabled = cmd.update.enabled;
          e.timeLimited = cmd.update.timeLimited;
          e.allowedFromYear = cmd.update.allowedFromYear;
          e.allowedFromMonth = cmd.update.allowedFromMonth;
          e.allowedFromDay = cmd.update.allowedFromDay;
          e.allowedFromHour = cmd.update.allowedFromHour;
          e.allowedFromMin = cmd.update.allowedFromMin;
          e.allowedFromSec = cmd.update.allowedFromSec;
          e.allowedUntilYear = cmd.update.allowedUntilYear;
          e.allowedUntilMonth = cmd.update.allowedUntilMonth;
          e.allowedUntilDay = cmd.update.allowedUntilDay;
          e.allowedUntilHour = cmd.update.allowedUntilHour;
          e.allowedUntilMin = cmd.update.allowedUntilMin;
          e.allowedUntilSec = cmd.update.allowedUntilSec;
          e.allowedWeekdays = cmd.update.allowedWeekdays;
          e.allowedFromTimeHour = cmd.update.allowedFromTimeHour;
          e.allowedFromTimeMin = cmd.update.allowedFromTimeMin;
          e.allowedUntilTimeHour = cmd.update.allowedUntilTimeHour;
          e.allowedUntilTimeMin = cmd.update.allowedUntilTimeMin;
        }
      }
    }
    if (result == CmdResult::Success && this->keypad_cache_valid_) {
      this->keypad_cache_at_ = millis();
    }
    // Read the list back from the device once the commands are through
    if (result != CmdResult::Success && result != CmdResult::Failed) {
      // No answer: the command may or may not have reached the device. What is known of the
      // keypad is no longer certain until it has been read again.
      this->keypad_cache_valid_ = false;
    }
    uint32_t delay = 500;
    if (this->keypad_fast_list_ && this->keypad_cache_valid_ && this->keypad_commands_pending_ == 0 &&
        result == CmdResult::Success) {
      // Every change is known from the answers of the device: publish the list as it must be
      // now (after the answer to this command, below), and read it back a little later to
      // confirm it and to get the dates of new entries
      publish_list = true;
      delay = 15000;
    }
    this->keypad_refresh_at_ = millis() + delay;
    if (this->keypad_refresh_at_ == 0) {
      this->keypad_refresh_at_ = 1;
    }
  }
  const bool executed = cmd.verdict == nullptr;
  const bool refused = executed && result == CmdResult::Failed;
  const uint16_t code_id = cmd.code_id;
  if (cmd.batch) {
    // Part of a batch: the result is collected and published with the others
    JsonObject json = cmd.batch->results[cmd.batch_index].to<JsonObject>();
    json["index"] = cmd.batch_index;
    if (!cmd.action.empty()) {
      json["action"] = cmd.action;
    }
    if (!cmd.name.empty()) {
      json["name"] = cmd.name;
    }
    json["result"] = text;
    if (refused) {
      json["error"] = error_name(this->ble()->get_last_error_code());
    }
    if (code_id != 0) {
      json["codeId"] = code_id;
    }
    if (executed && result == CmdResult::Success) {
      cmd.batch->succeeded++;
    }
    if (--cmd.batch->remaining == 0) {
      this->finish_keypad_batch_(*cmd.batch);
    }
  } else {
    this->publish_("/keypad/commandResultJson", text);
    this->publish_result_("/keypad/result", cmd.request_id, cmd.action, text, refused, cmd.received_at,
                          [code_id, &cmd](JsonObject json) {
                            if (code_id != 0) {
                              json["codeId"] = code_id;
                            }
                            if (cmd.existed) {
                              json["existed"] = true;
                            }
                          });
  }
  if (executed && result == CmdResult::Success && code_id != 0) {
    // The one entry that changed, for clients that keep a copy of the list
    JsonDocument doc(json_memory());
    JsonObject json = doc.to<JsonObject>();
    json["change"] = cmd.action == "add" ? "added" : (cmd.action == "delete" ? "deleted" : "updated");
    json["codeId"] = code_id;
    const KeypadEntry *entry = this->find_keypad_entry_(code_id);
    if (entry != nullptr && cmd.action != "delete") {
      keypad_entry_to_json(*entry, json["entry"].to<JsonObject>(), this->keypad_publish_code_);
      if (cmd.action == "add") {
        // Not known before the next read of the list
        json["entry"].remove("dateCreated");
        json["entry"].remove("dateLastActive");
        json["entry"].remove("lockCount");
      }
    }
    this->publish_json_("/keypad/changed", doc, false);
  }
  if (publish_list) {
    this->publish_keypad_();
  }
}

// Fills the date and time fields `prefix`Year..Sec of a packed entry from six values
#define NUKI_SET_DATE(entry, prefix, v) \
  do { \
    (entry).prefix##Year = (v)[0]; \
    (entry).prefix##Month = (v)[1]; \
    (entry).prefix##Day = (v)[2]; \
    (entry).prefix##Hour = (v)[3]; \
    (entry).prefix##Min = (v)[4]; \
    (entry).prefix##Sec = (v)[5]; \
  } while (0)

CmdResult NukiHubComponent::keypad_command_step_(KeypadCommand &cmd) {
  auto refuse = [&cmd](const char *verdict) {
    cmd.verdict = verdict;
    return CmdResult::Failed;
  };

  if (cmd.phase == KeypadCommand::Phase::Parse) {
    if (cmd.timeout_ms == 0 && cmd.payload.find("timeoutMs") != std::string::npos) {
      JsonDocument probe(json_memory());
      if (!deserializeJson(probe, cmd.payload)) {
        cmd.timeout_ms = probe["timeoutMs"].as<unsigned int>();
      }
    }
    if (cmd.timeout_ms != 0 && millis() - cmd.received_at > cmd.timeout_ms) {
      // The client gave up on this command by now, don't change the keypad behind its back
      return refuse("expired");
    }
    if (!this->pin_valid_) {
      // Not checked yet (the device was not reached since the start) is not the same as wrong
      return refuse(this->security_pin_ == 0 ? "noPinSet" : (this->pin_checked_ ? "noValidPinSet" : "configNotReady"));
    }
    if (!this->keypad_control_) {
      return refuse("keypadControlDisabled");
    }
    if (!this->has_keypad()) {
      return refuse(this->config_valid() ? "keypadNotAvailable" : "configNotReady");
    }

    JsonDocument json(json_memory());
    if (deserializeJson(json, cmd.payload) || !json.is<JsonObjectConst>()) {
      return refuse("invalidJson");
    }
    if (!json["action"].is<const char *>()) {
      return refuse("noActionSet");
    }
    cmd.action = json["action"].as<const char *>();
    if (json["codeId"].as<unsigned int>() > 65535) {
      // Ids have 16 bits: a larger number must not end up as some other entry's id
      return refuse("noExistingCodeIdSet");
    }
    cmd.code_id = json["codeId"].as<unsigned int>();
    cmd.only_if_expired = json["ifExpired"].as<bool>();

    if (cmd.action == "check") {
      if (!this->keypad_check_code_) {
        return refuse("checkingKeypadCodesDisabled");
      }
      const KeypadEntry *entry = this->keypad_cache_valid_ ? this->find_keypad_entry_(cmd.code_id) : nullptr;
      if (entry == nullptr) {
        return refuse("noExistingCodeIdSet");
      }
      return refuse(json["code"].as<unsigned int>() == entry->code ? "codeValid" : "codeInvalid");
    }
    if (cmd.action != "add" && cmd.action != "update" && cmd.action != "delete") {
      return refuse("invalidAction");
    }

    if (cmd.action == "delete") {
      if (cmd.code_id == 0) {
        return refuse("noCodeIdSet");
      }
      cmd.phase = KeypadCommand::Phase::Fetch;
    } else {
      std::string name = json["name"].is<JsonVariantConst>() ? util::as_text(json["name"]) : "";
      cmd.has_name = !name.empty();
      cmd.name = name.substr(0, 20);
      if (!cmd.has_name && cmd.action == "add") {
        return refuse("noNameSet");
      }

      uint32_t code = 0;
      if (json["code"].is<JsonVariantConst>()) {
        const std::string code_text = util::as_text(json["code"]);
        code = json["code"].as<unsigned int>();
        if (!(code > 100000 && code < 1000000 && code_text.find('0') == std::string::npos)) {
          return refuse("noValidCodeSet");
        }
        cmd.has_code = true;
      } else if (cmd.action == "add") {
        return refuse("noCodeSet");
      }

      cmd.has_enabled = json["enabled"].is<JsonVariantConst>();
      const uint8_t enabled = cmd.has_enabled ? (json["enabled"].as<unsigned int>() ? 1 : 0) : 1;
      cmd.has_time_limited = json["timeLimited"].is<JsonVariantConst>();
      const uint8_t time_limited = cmd.has_time_limited ? (json["timeLimited"].as<unsigned int>() == 1 ? 1 : 0) : 0;

      uint16_t from[6] = {0}, until[6] = {0};
      uint8_t from_time[2] = {0}, until_time[2] = {0};
      uint8_t weekdays = 0;
      if (time_limited == 1) {
        const std::string allowed_from = util::as_text(json["allowedFrom"]);
        if (!allowed_from.empty()) {
          if (!util::parse_date_time(allowed_from, from)) {
            return refuse("invalidAllowedFrom");
          }
          cmd.has_from = true;
        }
        const std::string allowed_until = util::as_text(json["allowedUntil"]);
        if (!allowed_until.empty()) {
          if (!util::parse_date_time(allowed_until, until)) {
            return refuse("invalidAllowedUntil");
          }
          cmd.has_until = true;
        }
        const std::string allowed_from_time = util::as_text(json["allowedFromTime"]);
        if (!allowed_from_time.empty()) {
          if (!util::parse_time_of_day(allowed_from_time, from_time)) {
            return refuse("invalidAllowedFromTime");
          }
          cmd.has_from_time = true;
        }
        const std::string allowed_until_time = util::as_text(json["allowedUntilTime"]);
        if (!allowed_until_time.empty()) {
          if (!util::parse_time_of_day(allowed_until_time, until_time)) {
            return refuse("invalidAllowedUntilTime");
          }
          cmd.has_until_time = true;
        }
        if (json["allowedWeekdays"].is<JsonVariantConst>()) {
          weekdays = util::weekdays_from_json(json["allowedWeekdays"]);
          cmd.has_weekdays = true;
        }
      }

      if (cmd.action == "add") {
        NewKeypadEntry &e = cmd.add;
        memset(&e, 0, sizeof(e));
        e.code = code;
        memcpy(e.name, name.c_str(), std::min(name.size(), sizeof(e.name)));
        e.timeLimited = time_limited;
        NUKI_SET_DATE(e, allowedFrom, from);
        NUKI_SET_DATE(e, allowedUntil, until);
        e.allowedWeekdays = weekdays;
        e.allowedFromTimeHour = from_time[0];
        e.allowedFromTimeMin = from_time[1];
        e.allowedUntilTimeHour = until_time[0];
        e.allowedUntilTimeMin = until_time[1];
        cmd.phase = KeypadCommand::Phase::Execute;
      } else {
        if (cmd.code_id == 0) {
          return refuse("noCodeIdSet");
        }
        UpdatedKeypadEntry &e = cmd.update;
        memset(&e, 0, sizeof(e));
        e.codeId = cmd.code_id;
        e.code = code;
        memcpy(e.name, name.c_str(), std::min(name.size(), sizeof(e.name)));
        e.enabled = enabled;
        e.timeLimited = time_limited;
        NUKI_SET_DATE(e, allowedFrom, from);
        NUKI_SET_DATE(e, allowedUntil, until);
        e.allowedWeekdays = weekdays;
        e.allowedFromTimeHour = from_time[0];
        e.allowedFromTimeMin = from_time[1];
        e.allowedUntilTimeHour = until_time[0];
        e.allowedUntilTimeMin = until_time[1];
        cmd.phase = KeypadCommand::Phase::Fetch;
      }
    }
  }

  if (cmd.phase == KeypadCommand::Phase::Fetch) {
    // delete and update refer to an existing entry: look it up, in a fresh list if the
    // cached one doesn't have it
    const KeypadEntry *existing = this->keypad_cache_valid_ ? this->find_keypad_entry_(cmd.code_id) : nullptr;
    if (existing == nullptr && !cmd.fetched) {
      CmdResult result = this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_);
      if (result == CmdResult::Working) {
        return result;
      }
      if (result != CmdResult::Success) {
        if (result == CmdResult::Failed) {
          return refuse("failedToRetrieveExistingKeypadEntry");
        }
        return result;
      }
      cmd.fetched = true;
      this->ble()->get_keypad_entries(&this->keypad_entries_);
      std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
      this->keypad_cache_valid_ = true;
      this->keypad_cache_at_ = millis();
      existing = this->find_keypad_entry_(cmd.code_id);
    }
    if (existing == nullptr) {
      return refuse("noExistingCodeIdSet");
    }
    if (cmd.only_if_expired && !this->keypad_entry_expired_(*existing)) {
      // Extended by a command that came in between
      return refuse("notExpired");
    }

    if (cmd.action == "update") {
      // Everything the command left out keeps its current value
      UpdatedKeypadEntry &e = cmd.update;
      if (!cmd.has_name) {
        memcpy(e.name, existing->name, sizeof(e.name));
      }
      if (!cmd.has_code) {
        e.code = existing->code;
      }
      if (!cmd.has_enabled) {
        e.enabled = existing->enabled;
      }
      if (!cmd.has_time_limited) {
        e.timeLimited = existing->timeLimited;
      }
      if (e.timeLimited == 1) {
        if (!cmd.has_from) {
          e.allowedFromYear = existing->allowedFromYear;
          e.allowedFromMonth = existing->allowedFromMonth;
          e.allowedFromDay = existing->allowedFromDay;
          e.allowedFromHour = existing->allowedFromHour;
          e.allowedFromMin = existing->allowedFromMin;
          e.allowedFromSec = existing->allowedFromSec;
        }
        if (!cmd.has_until) {
          e.allowedUntilYear = existing->allowedUntilYear;
          e.allowedUntilMonth = existing->allowedUntilMonth;
          e.allowedUntilDay = existing->allowedUntilDay;
          e.allowedUntilHour = existing->allowedUntilHour;
          e.allowedUntilMin = existing->allowedUntilMin;
          e.allowedUntilSec = existing->allowedUntilSec;
        }
        if (!cmd.has_weekdays) {
          e.allowedWeekdays = existing->allowedWeekdays;
        }
        if (!cmd.has_from_time) {
          e.allowedFromTimeHour = existing->allowedFromTimeHour;
          e.allowedFromTimeMin = existing->allowedFromTimeMin;
        }
        if (!cmd.has_until_time) {
          e.allowedUntilTimeHour = existing->allowedUntilTimeHour;
          e.allowedUntilTimeMin = existing->allowedUntilTimeMin;
        }
      }
    }
    cmd.phase = KeypadCommand::Phase::Execute;
  }

  if (cmd.phase == KeypadCommand::Phase::Verify) {
    // The device said the code exists already. If it is the entry this command describes (the
    // answer to an earlier attempt got lost), the command has done its job.
    CmdResult result = this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_);
    if (result == CmdResult::Working) {
      return result;
    }
    if (result == CmdResult::Success) {
      this->ble()->get_keypad_entries(&this->keypad_entries_);
      std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
      this->keypad_cache_valid_ = true;
      this->keypad_cache_at_ = millis();
      for (const auto &entry : this->keypad_entries_) {
        const bool same_window =
            entry.timeLimited == cmd.add.timeLimited &&
            (entry.timeLimited == 0 ||
             (entry.allowedFromYear == cmd.add.allowedFromYear && entry.allowedFromMonth == cmd.add.allowedFromMonth &&
              entry.allowedFromDay == cmd.add.allowedFromDay && entry.allowedFromHour == cmd.add.allowedFromHour &&
              entry.allowedFromMin == cmd.add.allowedFromMin && entry.allowedFromSec == cmd.add.allowedFromSec &&
              entry.allowedUntilYear == cmd.add.allowedUntilYear &&
              entry.allowedUntilMonth == cmd.add.allowedUntilMonth && entry.allowedUntilDay == cmd.add.allowedUntilDay &&
              entry.allowedUntilHour == cmd.add.allowedUntilHour && entry.allowedUntilMin == cmd.add.allowedUntilMin &&
              entry.allowedUntilSec == cmd.add.allowedUntilSec && entry.allowedWeekdays == cmd.add.allowedWeekdays &&
              entry.allowedFromTimeHour == cmd.add.allowedFromTimeHour &&
              entry.allowedFromTimeMin == cmd.add.allowedFromTimeMin &&
              entry.allowedUntilTimeHour == cmd.add.allowedUntilTimeHour &&
              entry.allowedUntilTimeMin == cmd.add.allowedUntilTimeMin));
        if (entry.code == cmd.add.code && memcmp(entry.name, cmd.add.name, sizeof(entry.name)) == 0 && same_window) {
          cmd.code_id = entry.codeId;
          cmd.existed = true;
          return CmdResult::Success;
        }
      }
    }
    // Another entry has this code: the refusal stands
    this->ble()->set_last_error_code(0x2A);
    return CmdResult::Failed;
  }

  if (cmd.action == "add") {
    if (!cmd.fetched) {
      // First step of the add itself: the id the device answers with belongs to this command
      cmd.fetched = true;
      this->ble()->clear_last_keypad_code_id();
    }
    CmdResult result = this->ble()->add_keypad_entry(cmd.add);
    if (result == CmdResult::Failed && this->ble()->get_last_error_code() == 0x2A) {
      cmd.phase = KeypadCommand::Phase::Verify;
      return CmdResult::Working;
    }
    return result;
  }
  if (cmd.action == "update") {
    return this->ble()->update_keypad_entry(cmd.update);
  }
  return this->ble()->delete_keypad_entry(cmd.code_id);
}


// --------------------------------------------------------------------------------------------
// Keypad: several commands in one message
// --------------------------------------------------------------------------------------------

void NukiHubComponent::finish_keypad_batch_(KeypadBatch &batch) {
  JsonDocument doc(json_memory());
  JsonObject json = doc.to<JsonObject>();
  if (!batch.request_id.empty()) {
    json["requestId"] = batch.request_id;
  }
  json["result"] = batch.succeeded == batch.total ? "success" : (batch.succeeded == 0 ? "failed" : "partial");
  json["succeeded"] = batch.succeeded;
  json["failed"] = batch.total - batch.succeeded;
  if (batch.succeeded == batch.total && strcmp(batch.result_topic, "/keypad/expiredResult") == 0) {
    // Only a delete that failed is held back, the next entry that runs out is deleted at once
    this->keypad_expiry_not_before_ = 0;
  }
  json["durationMs"] = millis() - batch.received_at;
  for (JsonPairConst extra : batch.summary.as<JsonObjectConst>()) {
    json[extra.key()] = extra.value();
  }
  json["results"] = batch.results;
  if (!batch.silent) {
    this->publish_json_(batch.result_topic, doc, false);
  }
  ESP_LOGI(TAG, "Keypad batch: %u of %u commands succeeded", (unsigned) batch.succeeded, (unsigned) batch.total);
}

std::shared_ptr<KeypadBatch> NukiHubComponent::start_keypad_batch_(const std::string &request_id,
                                                                   const char *result_topic,
                                                                   const std::vector<std::string> &commands,
                                                                   uint32_t received_at) {
  if (commands.empty() || this->queue_.size() + commands.size() > MAX_QUEUE) {
    return nullptr;
  }
  auto batch = std::make_shared<KeypadBatch>();
  batch->request_id = request_id;
  batch->result_topic = result_topic;
  batch->received_at = received_at;
  batch->total = batch->remaining = commands.size();
  batch->results.to<JsonArray>();
  batch->summary.to<JsonObject>();
  for (size_t i = 0; i < commands.size(); i++) {
    batch->results.add<JsonObject>();
    this->enqueue_keypad_command_(commands[i], "", batch, i);
  }
  return batch;
}

// {"requestId": "...", "commands": [{"action": "add", ...}, {"action": "delete", "codeId": 5}]}
void NukiHubComponent::handle_keypad_batch_(const std::string &payload) {
  const uint32_t received_at = millis();
  const std::string request_id = util::request_id(payload);
  auto refuse = [this, &request_id, received_at](const char *text) {
    this->publish_result_("/keypad/batchResult", request_id, "", text, false, received_at);
  };
  if (!this->is_paired()) {
    refuse("notPaired");
    return;
  }
  JsonDocument json(json_memory());
  if (deserializeJson(json, payload) || !json["commands"].is<JsonArrayConst>()) {
    refuse("invalidJson");
    return;
  }
  std::vector<std::string> commands;
  for (JsonVariantConst command : json["commands"].as<JsonArrayConst>()) {
    std::string text;
    serializeJson(command, text);
    commands.push_back(std::move(text));
  }
  if (commands.empty()) {
    refuse("noCommands");
    return;
  }
  if (commands.size() > MAX_BATCH) {
    refuse("tooManyCommands");
    return;
  }
  if (!this->start_keypad_batch_(request_id, "/keypad/batchResult", commands, received_at)) {
    refuse("busy");
  }
}

// --------------------------------------------------------------------------------------------
// Keypad: bring the entries with a name prefix to a wanted state
// --------------------------------------------------------------------------------------------

// {"requestId": "...", "prefix": "booking_", "entries": [{"name": "booking_1", "code": 345678, ...}],
//  "dryRun": false}
// Entries on the keypad whose name starts with the prefix and that are not wanted are deleted,
// wanted ones that are missing are added, differing ones updated. Entries with other names
// are never touched.
void NukiHubComponent::handle_keypad_sync_(const std::string &payload) {
  const uint32_t received_at = millis();
  const std::string request_id = util::request_id(payload);
  auto refuse = [this, &request_id, received_at](const char *text) {
    this->publish_result_("/keypad/syncResult", request_id, "", text, false, received_at);
  };
  if (!this->is_paired()) {
    refuse("notPaired");
    return;
  }
  if (!this->pin_valid_) {
    refuse(this->security_pin_ == 0 ? "noPinSet" : "noValidPinSet");
    return;
  }
  if (!this->keypad_control_) {
    refuse("keypadControlDisabled");
    return;
  }
  if (!this->has_keypad()) {
    refuse(this->config_valid() ? "keypadNotAvailable" : "configNotReady");
    return;
  }
  auto wanted = std::make_shared<JsonDocument>(json_memory());
  if (deserializeJson(*wanted, payload) || !(*wanted)["entries"].is<JsonArrayConst>()) {
    refuse("invalidJson");
    return;
  }
  const std::string prefix = util::as_text((*wanted)["prefix"]);
  if (prefix.size() < 3) {
    // A short or missing prefix would put nearly every entry under this command's control
    refuse("invalidPrefix");
    return;
  }
  for (JsonVariantConst entry : (*wanted)["entries"].as<JsonArrayConst>()) {
    const std::string name = util::as_text(entry["name"]);
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() > 20) {
      refuse("invalidEntryName");
      return;
    }
  }
  if (this->queue_.size() >= MAX_QUEUE) {
    refuse("busy");
    return;
  }

  Operation op;
  op.name = "read keypad codes for sync";
  // A list read (or brought up to date by the commands since) seconds ago is not read again:
  // a sync in several parts would read it for every part
  auto fresh = std::make_shared<bool>(false);
  op.step = [this, fresh] {
    if (*fresh || (this->keypad_cache_valid_ && this->keypad_commands_pending_ == 0 &&
                   millis() - this->keypad_cache_at_ < 5000)) {
      *fresh = true;
      return CmdResult::Success;
    }
    return this->ble()->retrieve_keypad_entries(0, this->max_keypad_entries_);
  };
  op.done = [this, wanted, prefix, request_id, received_at, fresh](CmdResult result) {
    if (result != CmdResult::Success) {
      this->publish_result_("/keypad/syncResult", request_id, "", result_to_string(result),
                            result == CmdResult::Failed, received_at);
      return;
    }
    if (!*fresh) {
      this->ble()->get_keypad_entries(&this->keypad_entries_);
      std::sort(this->keypad_entries_.begin(), this->keypad_entries_.end(),
                [](const KeypadEntry &a, const KeypadEntry &b) { return a.codeId < b.codeId; });
      this->keypad_cache_valid_ = true;
      this->keypad_cache_at_ = millis();
    }

    std::vector<std::string> commands;
    size_t adds = 0, updates = 0, deletes = 0, unchanged = 0;
    JsonArrayConst entries = (*wanted)["entries"].as<JsonArrayConst>();

    for (const auto &existing : this->keypad_entries_) {
      const std::string name = util::fixed_string(existing.name, sizeof(existing.name));
      if (name.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      bool keep = false;
      for (JsonVariantConst entry : entries) {
        keep = keep || util::as_text(entry["name"]) == name;
      }
      if (!keep) {
        JsonDocument command(json_memory());
        command["action"] = "delete";
        command["codeId"] = existing.codeId;
        std::string text;
        serializeJson(command, text);
        commands.push_back(std::move(text));
        deletes++;
      }
    }
    for (JsonVariantConst entry : entries) {
      const std::string name = util::as_text(entry["name"]);
      const KeypadEntry *existing = nullptr;
      for (const auto &candidate : this->keypad_entries_) {
        if (util::fixed_string(candidate.name, sizeof(candidate.name)) == name) {
          existing = &candidate;
          break;
        }
      }
      JsonDocument command(json_memory());
      for (JsonPairConst field : entry.as<JsonObjectConst>()) {
        command[field.key()] = field.value();
      }
      if (existing == nullptr) {
        if (!entry["timeLimited"].is<JsonVariantConst>() &&
            (!util::as_text(entry["allowedFrom"]).empty() || !util::as_text(entry["allowedUntil"]).empty())) {
          command["timeLimited"] = 1;
        }
        command["action"] = "add";
        adds++;
      } else {
        // Compare what the wanted entry states with what the keypad has, as values: the same
        // flag or date can be written in more than one way
        const bool wants_limit = entry["timeLimited"].is<JsonVariantConst>()
                                     ? entry["timeLimited"].as<unsigned int>() == 1
                                     : (!util::as_text(entry["allowedFrom"]).empty() ||
                                        !util::as_text(entry["allowedUntil"]).empty() || existing->timeLimited == 1);
        // A date without "timeLimited" means a time limit, the command says so explicitly
        command["timeLimited"] = wants_limit ? 1 : 0;
        bool differs = (existing->timeLimited == 1) != wants_limit;
        if (entry["code"].is<JsonVariantConst>()) {
          differs = differs || entry["code"].as<unsigned int>() != existing->code;
        }
        if (entry["enabled"].is<JsonVariantConst>()) {
          differs = differs || (entry["enabled"].as<unsigned int>() ? 1 : 0) != existing->enabled;
        }
        if (wants_limit) {
          uint16_t dt[6];
          uint8_t tod[2];
          const std::string from = util::as_text(entry["allowedFrom"]);
          if (!from.empty() && util::parse_date_time(from, dt)) {
            differs = differs || dt[0] != existing->allowedFromYear || dt[1] != existing->allowedFromMonth ||
                      dt[2] != existing->allowedFromDay || dt[3] != existing->allowedFromHour ||
                      dt[4] != existing->allowedFromMin || dt[5] != existing->allowedFromSec;
          }
          const std::string until = util::as_text(entry["allowedUntil"]);
          if (!until.empty() && util::parse_date_time(until, dt)) {
            differs = differs || dt[0] != existing->allowedUntilYear || dt[1] != existing->allowedUntilMonth ||
                      dt[2] != existing->allowedUntilDay || dt[3] != existing->allowedUntilHour ||
                      dt[4] != existing->allowedUntilMin || dt[5] != existing->allowedUntilSec;
          }
          const std::string from_time = util::as_text(entry["allowedFromTime"]);
          if (!from_time.empty() && util::parse_time_of_day(from_time, tod)) {
            differs = differs || tod[0] != existing->allowedFromTimeHour || tod[1] != existing->allowedFromTimeMin;
          }
          const std::string until_time = util::as_text(entry["allowedUntilTime"]);
          if (!until_time.empty() && util::parse_time_of_day(until_time, tod)) {
            differs = differs || tod[0] != existing->allowedUntilTimeHour || tod[1] != existing->allowedUntilTimeMin;
          }
          if (entry["allowedWeekdays"].is<JsonVariantConst>()) {
            differs = differs || util::weekdays_from_json(entry["allowedWeekdays"]) != existing->allowedWeekdays;
          }
        }
        if (!differs) {
          unchanged++;
          continue;
        }
        command["action"] = "update";
        command["codeId"] = existing->codeId;
        updates++;
      }
      std::string text;
      serializeJson(command, text);
      commands.push_back(std::move(text));
    }

    // More than one batch holds: this sync does the first part and says how much is left,
    // the same sync sent again goes on from there
    const size_t remaining = commands.size() > MAX_BATCH ? commands.size() - MAX_BATCH : 0;
    auto summary = [&](JsonObject json) {
      json["prefix"] = prefix;
      json["adds"] = adds;
      json["updates"] = updates;
      json["deletes"] = deletes;
      json["unchanged"] = unchanged;
      if (remaining > 0) {
        json["remaining"] = remaining;
      }
    };
    const bool dry_run = (*wanted)["dryRun"].as<bool>();
    if (commands.empty() || dry_run) {
      this->publish_result_("/keypad/syncResult", request_id, "", dry_run ? "dryRun" : "success", false, received_at,
                            summary);
      return;
    }
    if (remaining > 0 && !dry_run) {
      commands.resize(MAX_BATCH);
    }
    auto batch = this->start_keypad_batch_(request_id, "/keypad/syncResult", commands, received_at);
    if (!batch) {
      this->publish_result_("/keypad/syncResult", request_id, "", "busy", false, received_at, summary);
      return;
    }
    summary(batch->summary.as<JsonObject>());
  };
  this->enqueue(std::move(op));
}

// --------------------------------------------------------------------------------------------
// Keypad: remove time limited entries that have run out
// --------------------------------------------------------------------------------------------

bool NukiHubComponent::keypad_entry_expired_(const KeypadEntry &entry) {
#ifdef USE_TIME
  if (entry.timeLimited != 1 || entry.allowedUntilYear == 0 || this->clock_ == nullptr) {
    return false;
  }
  // Compared in the Nuki device's own (local) time, which update_time keeps equal to this clock
  ESPTime now = this->clock_->now();
  if (!now.is_valid()) {
    return false;
  }
  ESPTime limit = ESPTime::from_epoch_local(now.timestamp - this->keypad_delete_expired_after_);
  auto stamp = [](int year, int month, int day, int hour, int minute, int second) {
    return (((((int64_t) year * 100 + month) * 100 + day) * 100 + hour) * 100 + minute) * 100 + second;
  };
  return stamp(entry.allowedUntilYear, entry.allowedUntilMonth, entry.allowedUntilDay, entry.allowedUntilHour,
               entry.allowedUntilMin, entry.allowedUntilSec) <
         stamp(limit.year, limit.month, limit.day_of_month, limit.hour, limit.minute, limit.second);
#else
  return false;
#endif
}

void NukiHubComponent::delete_expired_keypad_entries_() {
#ifdef USE_TIME
  // Not while anything else is waiting: a command in the queue may be about to extend an entry
  if (this->keypad_delete_expired_after_ == 0 || this->keypad_commands_pending_ > 0 || !this->queue_.empty()) {
    return;
  }
  if (this->keypad_expiry_not_before_ != 0) {
    if ((int32_t) (millis() - this->keypad_expiry_not_before_) < 0) {
      return;
    }
    this->keypad_expiry_not_before_ = 0;
  }
  std::vector<std::string> commands;
  for (const auto &entry : this->keypad_entries_) {
    const std::string name = util::fixed_string(entry.name, sizeof(entry.name));
    if (!this->keypad_delete_expired_prefix_.empty() &&
        name.compare(0, this->keypad_delete_expired_prefix_.size(), this->keypad_delete_expired_prefix_) != 0) {
      continue;
    }
    if (!this->keypad_entry_expired_(entry) || commands.size() >= MAX_BATCH) {
      continue;
    }
    ESP_LOGI(TAG, "Keypad entry %u '%s' ran out, deleting it", entry.codeId, name.c_str());
    JsonDocument command(json_memory());
    command["action"] = "delete";
    command["codeId"] = entry.codeId;
    // Looked at again when the delete is about to happen
    command["ifExpired"] = true;
    std::string text;
    serializeJson(command, text);
    commands.push_back(std::move(text));
  }
  if (commands.empty()) {
    return;
  }
  // A delete that keeps failing must not be tried again with every read of the list
  this->keypad_expiry_not_before_ = millis() + 600000;
  if (this->keypad_expiry_not_before_ == 0) {
    this->keypad_expiry_not_before_ = 1;
  }
  auto batch = this->start_keypad_batch_("", "/keypad/expiredResult", commands, millis());
  if (batch) {
    batch->summary["reason"] = "expired";
  }
#endif
}

}  // namespace esphome::nuki_hub
