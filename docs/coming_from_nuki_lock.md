# Coming from ESPHome Nuki Lock

What `lock: - platform: nuki_lock` of [ESPHome Nuki Lock](https://github.com/uriyacovy/ESPHome_nuki_lock)
offers and where it is here. The lock itself moves to a `nuki_hub:` entry of `type: lock`, its
entities to the platforms of `nuki_hub`. The pairing is stored differently, so the lock has to
be paired again.

```yaml
nuki_hub:
  - id: front_door            # also the name shown in the Nuki app's user list
    type: lock
    topic_prefix: home        # needed also without mqtt:
    security_pin: 1234

lock:
  - platform: nuki_hub
    nuki_hub_id: front_door
    name: Nuki Lock
```

`mqtt:` is optional: without it the lock entity, the entities below and the automations work
over the ESPHome API alone.

## Options

| ESPHome Nuki Lock | Here |
| --- | --- |
| `security_pin`, `pairing_as_app`, `ble_general_timeout`, `ble_command_timeout`, `command_retries`, `command_retry_delay`, `query_interval_config` | the same in `nuki_hub:` (`security_pin` is not templatable) |
| `query_interval_auth_data` | `query_interval_authorizations` |
| `query_interval_battery_report` | `query_interval_battery` |
| `pairing_mode_timeout` | the same, but off (`0s`) unless set; there it is `300s` |
| `hybrid_mode` | `hybrid_mode`, see [nuki_hub.md](nuki_hub.md#hybrid-mode) |
| `door_sensor_input` | `door_sensor_input`, see [nuki_hub.md](nuki_hub.md#door-sensor-emulation) |
| `event` | `api_transport: true` sends everything published as the event `esphome.nuki_hub`; the activity log entries are the topics `/event/action` and `/event/keypad` |

## Entities

The switches (`logging_enabled` included), numbers, selects and buttons keep their keys, in `switch:`, `number:`,
`select:` and `button:` with `platform: nuki_hub`, see
[Settings](home_assistant.md#settings). The rest:

| ESPHome Nuki Lock | Here |
| --- | --- |
| `pairing_mode` | `switch:` `pairing_mode` |
| `unpair`, `request_calibration` | `button:` |
| `paired` | `binary_sensor:` `paired` |
| `connected` | `binary_sensor:` `available` |
| `battery_critical`, `keypad_battery_critical` | `binary_sensor:` the same |
| `battery_charging` | `binary_sensor:` topic `/battery/basicJson`, key `charging` |
| `door_sensor_battery_critical` | `binary_sensor:` topic `/battery/basicJson`, key `doorSensorCritical` |
| `door_sensor` | `binary_sensor:` topic `/doorSensorState`, `payload_on: doorOpened` |
| `remote_access_connected` | `binary_sensor:` topic `/json`, key `sseConnectionEstablished` |
| `battery_level` | `sensor:` topic `/battery/basicJson`, key `level` |
| `battery_voltage`, `battery_drain`, `motor_current` | `sensor:` topic `/battery/advancedJson`, keys `batteryVoltage`, `batteryDrain`, `maxTurnCurrent` |
| `bt_signal_strength` | `sensor:` `rssi` |
| `wifi_connection_strength` | `sensor:` topic `/json`, key `wifiConnectionStrength` |
| `door_sensor_state` | `text_sensor:` topic `/doorSensorState` |
| `last_unlock_user` | `text_sensor:` `last_user` |
| `last_lock_action` | `text_sensor:` topic `/lastLockAction` |
| `last_lock_action_trigger` | `text_sensor:` topic `/json`, key `last_lock_action_trigger` |
| `pin_status` | `binary_sensor:` topic `/status`, key `pinValid` |
| `wifi_connection_status`, `mqtt_connection_status`, `thread_connection_status` | the remote access values of `/json` (`sseConnectedViaWifi`, `isSseConnectedViaThread`, `bridgePaired`) |

How topics become entities is in [Entities from topics](home_assistant.md#entities-from-topics).

## Actions and automations

| ESPHome Nuki Lock | Here |
| --- | --- |
| `lock.open` (unlatch) | the same on the lock entity |
| Home Assistant action `lock_n_go` | `nuki_hub.action` with `lockNgo`, or the command `/action` |
| Home Assistant actions for keypad entries | the command `/keypad/actionJson`, see [topics](topics.md) and the example in [home_assistant.md](home_assistant.md) |
| `nuki_lock.set_pairing_mode`, `nuki_lock.unpair` | `nuki_hub.set_pairing_mode`, `nuki_hub.unpair` |
| `nuki_lock.request_calibration` | `nuki_hub.command` with topic `/configuration/action` and payload `{"recalibrateNuki":"1"}` |
| conditions `nuki_lock.paired`, `nuki_lock.connected` | `nuki_hub.paired`, `nuki_hub.available` |
| `on_event_log_action` | `on_message`, for the topics `/event/action` and `/event/keypad` |
| `nuki_lock.set_security_pin` | `nuki_hub.set_security_pin` |
| Home Assistant action `set_lock_action_options` | `nuki_hub.set_action_options`, or `force`, `autoUnlock` and `nameSuffix` in a JSON command on `/action` |
| `on_paired_action`, `on_pairing_mode_on_action`, `on_pairing_mode_off_action` | `on_paired`, `on_pairing_mode_on`, `on_pairing_mode_off` in `nuki_hub:` |

## Not here

- The Home Assistant event in ESPHome Nuki Lock's format: the entries arrive with the names
  Nuki Hub uses.

None of this has been run on a Smart Lock here, see the [test status](operations.md#test-status).
