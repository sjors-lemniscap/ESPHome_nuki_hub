# Home Assistant and automations

The ways a `nuki_hub` device shows up in Home Assistant, and the automations on the ESP32 itself. Setup is in [nuki_hub.md](nuki_hub.md).

## Home Assistant

Three ways, which can be combined:

- **MQTT discovery** (`homeassistant_discovery: true`): a lock, battery and ring binary
  sensors, signal, state, last user and keypad entry count, announced below `homeassistant/`.
- **ESPHome API**: `lock`, `binary_sensor`, `sensor` and `text_sensor` platforms. Besides
  the entities they name, each takes `topics:`, a list of entities that follow anything the
  device publishes (see below).
- **Automations** on the ESP32 itself, see above.

```yaml
api:

lock:
  - platform: nuki_hub
    nuki_hub_id: entrance
    name: Entrance

binary_sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    available:
      name: Entrance available
    battery_critical:
      name: Entrance battery critical
    keypad_battery_critical:
      name: Entrance keypad battery critical
    ring:
      name: Entrance bell

sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    rssi:
      name: Entrance signal
    keypad_entries:
      name: Entrance keypad entries

text_sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    state:
      name: Entrance state
    last_user:
      name: Entrance last user
```

**Keypad codes from Home Assistant without MQTT.** `nuki_hub.command` and `on_message`
carry the same JSON over the ESPHome API: the command goes in through an API action, the
answer comes back as a Home Assistant event.

```yaml
api:
  homeassistant_services: true
  actions:
    - action: keypad_command
      variables:
        payload: string
      then:
        - nuki_hub.command:
            id: entrance
            topic: /keypad/actionJson
            payload: !lambda "return payload;"

nuki_hub:
  - id: entrance
    # ...
    on_message:
      - if:
          condition:
            lambda: 'return topic == "/keypad/result" || topic == "/keypad/json";'
          then:
            - homeassistant.event:
                event: esphome.nuki_hub
                data:
                  topic: !lambda "return topic;"
                  payload: !lambda "return payload;"
```

In Home Assistant, `esphome.<node>_keypad_command` with
`payload: '{"action":"add","name":"guest","code":345678,"requestId":"1"}'` adds the code, and
the event `esphome.nuki_hub` brings `{"requestId":"1","result":"success","codeId":105,...}`.
(ESPHome's API actions can only return data directly when they are written in YAML with
`api.respond`, which cannot wait for a Bluetooth command; hence the event.)

If Home Assistant is not always connected, set `api: reboot_timeout: 0s`: ESPHome otherwise
restarts the ESP32 after 15 minutes without an API client.

### Entities from topics

Anything in the [topics](topics.md#topics) table can be an entity of the ESPHome API, without a
broker:

```yaml
text_sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    topics:
      - topic: /trigger
        name: Trigger
      - topic: /doorSensorState      # Smart Lock
        name: Door
sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    topics:
      - topic: /keypad/count
        name: Keypad codes
binary_sensor:
  - platform: nuki_hub
    nuki_hub_id: entrance
    topics:
      - topic: /continuousMode
        name: Continuous mode
        payload_on: "on"
```

A sensor takes payloads that are a number, a binary sensor is on while the payload equals
`payload_on` (default `1`).

With `key:` an entity follows one value of a JSON topic:

```yaml
sensor:
  - platform: nuki_hub
    nuki_hub_id: front_door
    topics:
      - topic: /battery/basicJson    # Smart Lock
        key: level
        name: Battery
        unit_of_measurement: "%"
binary_sensor:
  - platform: nuki_hub
    nuki_hub_id: front_door
    topics:
      - topic: /battery/basicJson
        key: charging
        name: Battery charging
```

### Settings

With MQTT discovery the main settings are announced as switches, numbers and selects of
the device (LED, button, sounds, timeouts, button and fob actions; for a Smart Lock auto
lock, night mode, durations, fob and button actions, battery type and motor speed). They read `configuration/basicJson` and
`configuration/advancedJson` and write through `configuration/action`.

Without a broker the same settings are entities of the ESPHome API. Add the ones you want
with a name; a key the device type does not have is reported in the log and stays unavailable.

```yaml
switch:
  - platform: nuki_hub
    nuki_hub_id: front_door
    pairing_mode:
      name: Pairing mode
    auto_lock_enabled:
      name: Auto lock
    night_mode_enabled:
      name: Night mode

number:
  - platform: nuki_hub
    nuki_hub_id: front_door
    auto_lock_timeout:
      name: Auto lock timeout
    led_brightness:
      name: LED brightness

select:
  - platform: nuki_hub
    nuki_hub_id: front_door
    single_button_press_action:
      name: Button, single press
    motor_speed:
      name: Motor speed

button:
  - platform: nuki_hub
    nuki_hub_id: front_door
    unpair:
      name: Unpair
    request_calibration:
      name: Calibrate
```

| Platform | Smart Lock | Opener | Both |
| --- | --- | --- | --- |
| `switch` | `bluetooth_pairing_enabled`, `auto_unlatch`, `led_enabled`, `night_mode_enabled`, `night_mode_auto_lock_enabled`, `night_mode_auto_unlock_disabled`, `night_mode_immediate_lock_on_start_enabled`, `auto_lock_enabled`, `auto_unlock_disabled`, `immediate_auto_lock_enabled`, `auto_update_enabled`, `logging_enabled`, `single_lock_enabled`, `dst_mode_enabled`, `auto_battery_type_detection_enabled`, `slow_speed_during_night_mode_enabled`, `detached_cylinder_enabled` | `led_flash_enabled`, `sound_confirmation`, `disable_rto_after_ring`, `random_electric_strike_delay` | `pairing_mode`, `button_enabled` |
| `number` | `led_brightness`, `timezone_offset`, `lock_n_go_timeout`, `auto_lock_timeout`, `unlatch_duration`, `unlocked_position_offset`, `locked_position_offset`, `single_locked_position_offset`, `unlocked_to_locked_transition_offset` | `sound_level`, `rto_timeout`, `electric_strike_delay`, `electric_strike_duration`, `doorbell_suppression_duration` | |
| `select` | `timezone`, `advertising_mode`, `battery_type`, `motor_speed` | `sound_ring`, `sound_open`, `sound_rto`, `sound_cm`, `doorbell_suppression` | `single_button_press_action`, `double_button_press_action`, `fob_action_1` to `3` |
| `button` | `request_calibration` | | `unpair`, `reboot` (the Nuki device), `query_state`, `query_config`, `query_battery`, `query_keypad` |

Changing a setting needs the security PIN. The configuration is read again after every
write, so an entity shows what the device really has.

Every authorization and every time control entry is a switch that enables and disables it,
like the keypad entries with `keypad_topic_per_entry`. Switches of entries that are gone
are withdrawn, also those left from before a restart. Mind that the keypad and the apps are
authorizations themselves: switching one off locks it out.

### After a start

The entities are announced a few at a time, with a look at the free memory in between: a
device with its settings is about forty messages, complete after about three seconds.

### Availability

Entities announced over MQTT are available when the Nuki device answers (`availability`) and
the ESP32 is connected to the broker. The second is the last will of the `mqtt:` component:
give `birth_message` and `will_message` the same topic, as in the example, or leave both at
ESPHome's default.

### Coming from Nuki Hub

Nuki Hub's own discovery topics stay on the broker when the ESP32 is flashed, and with them
its entities in Home Assistant, next to the new ones. Publish `1` to
`<prefix>/maintenance/removeNukiHubDiscovery` once to clear those of the Nuki device. The
topics of the Nuki Hub device itself (restart, Wi-Fi signal, firmware) are below
`homeassistant/+/<hub id>/` and have to be cleared by hand or with the device removed in
Home Assistant.

## ESPHome automations

```yaml
button:
  - platform: template
    name: Open entrance
    on_press:
      - if:
          condition:
            nuki_hub.available: entrance
          then:
            - nuki_hub.action:
                id: entrance
                action: electricStrikeActuation
```

| | |
| --- | --- |
| `nuki_hub.action` | Queues a lock action by its name, see `action` under [Topics](topics.md#topics). `action` can be a lambda |
| `nuki_hub.command` | Does what a message on a command topic does: `topic` is the part after `<prefix>/<kind>` (`/keypad/actionJson`, `/keypad/syncJson`, `/timecontrol/action`, ...), `payload` the message. Works without MQTT |
| `on_message` (trigger, in the `nuki_hub:` entry) | Fires for everything published below `<prefix>/<kind>` (not the `maintenance` and `info` topics of the ESP32), with `topic` (same form) and `payload`. Works without MQTT |
| `nuki_hub.set_pairing_mode` | `pairing_mode: true` looks for a Nuki device in pairing mode until one is paired |
| `nuki_hub.unpair` | Forgets the pairing |
| `nuki_hub.set_action_options` | `force`, `auto_unlock` and `name_suffix` for the next lock action only, also one from the lock entity. See [topics](topics.md) |
| `nuki_hub.set_security_pin` | Replaces the `security_pin` of the configuration (templatable). Kept in flash and stronger than the YAML, also after a new firmware; `0` goes back to the YAML |
| `on_paired` (trigger, in the `nuki_hub:` entry) | A pairing was made |
| `on_pairing_mode_on`, `on_pairing_mode_off` (triggers) | Pairing mode was switched |
| `nuki_hub.paired` | Condition: a pairing is stored |
| `nuki_hub.available` | Condition: the Nuki device answered the last commands and is advertising |
