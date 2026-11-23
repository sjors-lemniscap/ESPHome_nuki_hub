#pragma once
/**
 * @file nuki_opener_protocol.h
 * Communication with a Nuki Opener, the counterpart of nuki_lock_protocol.h
 *
 * Ported from NukiBleEsp32 (NukiOpener.h), GNU GENERAL PUBLIC LICENSE (see LICENSE)
 *
 * Every method that talks to the Opener performs one step per call and returns
 * CmdResult::Working until it is done, like the methods of NukiLock.
 */

#include "nuki_ble.h"
#include "nuki_opener_constants.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace esphome::nuki_lock::opener {

class NukiOpener : public NukiBle {
  public:
    NukiOpener(const std::string &device_name, const uint32_t device_id);

    /**
     * @brief Sends a lock action to the Opener
     *
     * @param nukiAppId 0 = App, 1 = Bridge, 2 = Fob, 3 = Keypad. Continuous mode is switched
     * with a Continuous Mode Action unless the id is 1.
     */
    CmdResult lock_action(const LockAction lock_action, const uint32_t nukiAppId = 1, const uint8_t flags = 0,
                          const char *nameSuffix = nullptr, const uint8_t nameSuffixLen = 0);

    /// Switches continuous mode. `timeoutMinutes` 0 leaves it on until it is switched off.
    CmdResult continuous_mode(bool enabled, uint8_t timeoutMinutes = 0);

    CmdResult request_opener_state(OpenerState *state);
    void retrieve_opener_state(OpenerState *state);
    /// The state the Opener pushed on its own since clear_pushed_opener_state(), once
    bool take_pushed_opener_state(OpenerState *state);
    void clear_pushed_opener_state() { this->opener_state_pushed_ = false; }

    CmdResult request_battery_report(BatteryReport *report);
    CmdResult request_config(Config *config);
    CmdResult request_advanced_config(AdvancedConfig *config);

    /// Reads the config, lets `mutate` change it and writes it back
    CmdResult write_config(const std::function<void(Config &)> &mutate);
    /// Reads the advanced config, lets `mutate` change it and writes it back
    CmdResult write_advanced_config(const std::function<void(AdvancedConfig &)> &mutate);

    CmdResult add_time_control_entry(NewTimeControlEntry entry);
    CmdResult update_time_control_entry(TimeControlEntry entry);
    CmdResult remove_time_control_entry(uint8_t entryId);
    /// Done once the Opener has sent all entries
    CmdResult retrieve_time_control_entries();
    void get_time_control_entries(std::vector<TimeControlEntry> *entries);

    /// Done once the Opener has sent all requested entries
    CmdResult retrieve_log_entries(const uint32_t startIndex, const uint16_t count, const uint8_t sortOrder,
                                   const bool totalCount);
    void get_log_entries(std::vector<LogEntry> *entries);

    bool is_battery_critical();
    bool is_keypad_battery_critical();

    uint8_t get_last_error() const { return this->error_code_; }

    void log_error_code(uint8_t error_code) override;

  protected:
    void handle_return_message(Command returnCode, unsigned char *data, uint16_t dataLen) override;

    CmdResult set_config(NewConfig newConfig);
    CmdResult set_advanced_config(NewAdvancedConfig newAdvancedConfig);
    static void create_new_config(const Config *oldConfig, NewConfig *newConfig);
    static void create_new_advanced_config(const AdvancedConfig *oldConfig, NewAdvancedConfig *newConfig);

    /// Waits for the end of a list the Opener sends after `request` was accepted
    CmdResult retrieve_list(const NukiAction &request, const std::function<void()> &clear,
                            const std::function<size_t()> &size, const char *what);

    OpenerState opener_state_;
    bool opener_state_pushed_{false};
    BatteryReport battery_report_;
    Config config_;
    AdvancedConfig advanced_config_;
    std::vector<TimeControlEntry> list_of_time_control_entries_;
    std::vector<LogEntry> list_of_log_entries_;

    bool config_write_pending_{false};
    Config config_write_buffer_;
    bool advanced_config_write_pending_{false};
    AdvancedConfig advanced_config_write_buffer_;

    // List retrieval progress, see retrieve_list()
    bool list_request_started_{false};
    bool list_waiting_{false};
    bool list_complete_{false};
    bool list_count_known_{false};
    uint16_t list_expected_count_{0};
    size_t list_last_size_{0};
    int64_t list_wait_since_{0};
    uint32_t list_timeout_ms_{5000};
};

}  // namespace esphome::nuki_lock::opener
