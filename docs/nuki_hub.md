# nuki_hub: Nuki Hub's MQTT interface on ESPHome

`nuki_hub` connects a **Nuki Opener** or a **Nuki Smart Lock** over Bluetooth and makes it available
over MQTT with the topics and payloads of [Nuki Hub](https://github.com/technyon/nuki_hub).
Software written against Nuki Hub (a booking system that manages keypad codes, for example)
keeps working, while the ESP32 itself is configured, built and updated like any other
ESPHome device.


## Where to find what

| | |
| --- | --- |
| This page | what it does, configuration, pairing, security, differences from Nuki Hub |
| [Topics and commands](topics.md) | every topic, the additions, results and the queue, the ESPHome API transport |
| [Home Assistant and automations](home_assistant.md) | discovery, entities, settings, availability, automations on the ESP32 |
| [Running it](operations.md) | many rooms, memory and large keypads, what has been tested |
| [Writing a client](client_guide.md) | how to use the topics well |
| [Coming from Nuki Hub](migrating_from_nuki_hub.md) | taking over the pairing and going back |
| [Measurements](benchmarks.md) | times and memory on the test rig |

## What it does

| | |
| --- | --- |
| Devices | Nuki Opener, Nuki Smart Lock (1st to 5th generation, Ultra, Go, Pro) |
| State | lock state, trigger, last action, battery, signal strength, availability |
| Actions | every lock action of the device, with a result per command |
| Keypad | list, add, update, delete, with time limits, weekdays and enable/disable |
| Authorizations | list, update, delete |
| Time control | list, add, update, delete |
| Configuration | read as JSON, write single settings, with a result per setting |
| Activity log | newest entries, and a rolling log that publishes every entry once and in order |
| Maintenance | uptime, free heap, Wi-Fi signal, restart reasons, log lines, restart over MQTT |
| Home Assistant | optional lock entity through the ESPHome API (`lock: - platform: nuki_hub`) |

Without an `mqtt:` component the Nuki Hub topics are not available, and what remains is the
lock entity and the automations: an Opener in Home Assistant through the ESPHome API.

Several devices can be configured on one ESP32, each with its own `topic_prefix`. Every
device uses one Bluetooth connection slot (`esp32_ble: max_connections`, default 3).

## Configuration

A complete example is in [`examples/nuki_hub_opener.yaml`](../examples/nuki_hub_opener.yaml).

```yaml
external_components:
  - source: github://sjors-lemniscap/ESPHome_nuki_hub
    components: [nuki_hub]

esp32_ble_tracker:

mqtt:
  broker: 192.168.1.10
  discovery: false
  log_topic: null        # ESPHome's own debug log over MQTT is not needed
  idf_send_async: true   # a publish must not wait for the network, see below

nuki_hub:
  - id: entrance
    type: opener
    topic_prefix: entrance
    security_pin: 1234
```

| Option | Default | Description |
| --- | --- | --- |
| `type` | required | `opener` or `lock` |
| `topic_prefix` | required | Nuki Hub's "MQTT path". Topics are `<prefix>/opener/...` or `<prefix>/lock/...` and `<prefix>/maintenance/...` |
| `id` | generated | Also the name the Nuki device stores for this pairing (at most 32 characters). Changing it means pairing again |
| `security_pin` | `0` | The PIN set in the Nuki app. Needed for the keypad, authorizations, time control, the activity log and settings |
| `pairing_as_app` | `false` | Pair as an app instead of as a bridge |
| `pairing_mode_at_boot` | `false` | Look for a Nuki device in pairing mode while not paired |
| `pairing` | | Take over an existing pairing, see [Pairing](#pairing) |
| `keypad_control` | `true` | Accept keypad commands |
| `keypad_publish_code` | `false` | Include the codes themselves in `keypad/json` |
| `keypad_fast_list` | `false` | Publish `keypad/json` right after keypad commands from what their answers say, and read the list back from the device 15 seconds later instead of at once. New entries have no creation date until then |
| `keypad_topic_per_entry` | `false` | Also publish every entry on its own retained topic `keypad/codes/<code id>`, and with `homeassistant_discovery` a switch per entry that enables and disables it. Topics and switches of deleted entries are removed |
| `keypad_check_code` | `false` | Allow the keypad `check` action, which tells whether a code belongs to a code id |
| `keypad_delete_expired` | off | `after` (default `1h`) and `name_prefix`: delete time limited entries whose end lies longer ago than `after`, optionally only those whose name starts with the prefix. Needs `time_id` and `update_time`. The end of an entry is read as local time of the `time` component, the clock `update_time` gives the Nuki device: write the dates of the entries in that time zone |
| `invalid_code_alert` | off | `count` (default 3) and `window` (default `5min`): publish `event/invalidCodes` when that many wrong codes were entered within the window |
| `log_marker_in_flash` | `false` | Keep the index of the last published log entry in flash instead of reading it back from the retained `lastRollingLog` |
| `homeassistant_discovery` | `false` | `true` or a discovery prefix: announce the device to Home Assistant over MQTT |
| `api_transport` | `false` | The topics over the ESPHome API as well: an action for commands and an event for everything published, see [Without a broker](topics.md#without-a-broker) |
| `mqtt_qos` | `0` | QoS of what is published. `1` costs about 115 ms per command with most brokers, see [results](topics.md#command-queue-and-results) |
| `authorization_add` | `false` | Let a client create authorizations with `authorization/action`. An authorization is permanent access to the Nuki device for whoever holds its key |
| `bluetooth_watchdog` | `30min` | Restart the ESP32 if no command got through for this long although the Nuki device is advertising. `0s` turns it off |
| `fast_connection` | `true` | Ask for a short Bluetooth connection interval (7.5 to 15 ms) while connected. Commands get faster, see [measurements](benchmarks.md) |
| `ble_disconnect_timeout` | `2s` | How long the Bluetooth connection is kept open after a command, for the next one |
| `connection_linger` | `15s` | After a command from a client the connection stays open this long, because another one often follows; that one then does not wait for the device to advertise. `0s` closes it after `ble_disconnect_timeout` as before. What the component reads by itself (state, the list read back) does not prolong it |
| `keep_connected` | `false` | Never close the Bluetooth connection. Every command is then as fast as one that follows another (see [measurements](benchmarks.md#the-first-command-after-a-pause)), at the cost of the Nuki device's batteries: it answers about 65 times a second instead of sending an advertisement about twice a second. For a while only, a client writes the seconds to `connection/hold` |
| `challenge_prefetch` | on for an Opener, off for a Smart Lock | Ask for the challenge of the next command when one is done, so the next one goes out at once. Not tried on a Smart Lock. Switches itself off until the restart when the device refuses two commands sent this way |
| `mqtt_commands` | `true` | `false`: the broker only gets what the device publishes, nothing sent to it is taken as a command. For commands over the API only (`api_transport`), with MQTT kept for watching. The entities of Home Assistant's MQTT discovery can then not switch anything |
| `hybrid_mode` | off | `type: lock` only: use the lock's own MQTT for state changes and, with `send_actions`, for lock actions, see [Hybrid mode](#hybrid-mode) |
| `publish_activity_log` | `true` | Publish `log`, `shortLog`, `rollingLog` |
| `max_keypad_entries` | `100` | Entries read from the keypad |
| `max_authorization_entries` | `20` | Authorizations read |
| `max_log_entries` | `5` | Newest log entries read per update |
| `query_interval_state` | `30min` | Besides this, the state is read whenever the device announces a change |
| `query_interval_config` | `60min` | |
| `query_interval_battery` | `30min` | |
| `query_interval_keypad` | `30min` | Besides this, the keypad is read after every keypad command |
| `query_interval_authorizations` | `60min` | |
| `query_interval_time_control` | `60min` | |
| `command_retries` | `3` | Retries of a command that got no answer |
| `command_retry_delay` | `1s` | |
| `ble_general_timeout` | `10s` | |
| `ble_command_timeout` | `3s` | How long the device may take to answer a command |
| `ble_action_timeout` | `15s` | How long an accepted action (lock, unlock, open) may take to complete |
| `time_id` | | A `time` component, for `update_time` |
| `update_time` | `false` | Set the clock of the Nuki device at boot and with every config query |
| `publish_maintenance` | `true` | Publish the `<prefix>/maintenance/...` and `<prefix>/info/...` topics |
| `mqtt_log` | off | A log level (`INFO`, `DEBUG`, ...): publish the log lines up to that level on `<prefix>/maintenance/log`, one line per message, like Nuki Hub's "MQTT logging" |
| `debug` | | `connect`, `communication`, `readable_data`, `hex_data`, `command`: extra logging of the Bluetooth protocol |

Set **`idf_send_async: true`** in `mqtt:`. Without it every publish waits until the message
is on the network. If the connection to the broker stalls (seen on the test rig with a broker
whose replies took another network path), the main loop of the ESP32 stands still with it,
and after five seconds the watchdog restarts the ESP32, again and again for as long as the
connection stays bad. With the option, messages are queued and the loop goes on. The
component warns at build time if the option is missing.

Nuki Hub's connection state is the last will of the MQTT connection and is set in the `mqtt:`
block, see the example: `birth_message`/`will_message` on
`<prefix>/maintenance/mqttConnectionState`.

## Pairing

**With the button.** Set `pairing_mode_at_boot: true`, flash, and put the Nuki device into
pairing mode (hold its button for five seconds). The log shows `Paired with the Nuki opener`.
Remove the option afterwards, or use the `nuki_hub.set_pairing_mode` and `nuki_hub.unpair`
actions instead.

**Taking over a pairing from Nuki Hub.** An ESP32 that ran Nuki Hub is already authorized
at the Nuki device. Its pairing can be reused, so nobody has to press the button:

```yaml
nuki_hub:
  - id: entrance
    type: opener
    topic_prefix: entrance
    security_pin: !secret nuki_pin
    pairing:
      ble_address: "54:D2:72:00:00:00"
      secret_key: !secret nuki_secret_key          # 32 bytes as 64 hexadecimal digits
      authorization_id: !secret nuki_auth_id       # 4 bytes as 8 hexadecimal digits
```

The three values are stored by Nuki Hub in the ESP32's NVS under the keys `bleAddress`,
`secretKeyK` and `authorizationId`, for an Opener in the namespace `NukiHubopener`. Read the flash before installing ESPHome
(`esptool read-flash 0x9000 0x5000 nvs.bin`) and decode it with ESP-IDF's `nvs_tool.py`.
The pairing is taken over once, while the device has none; after that the option can be
removed. Treat the key like a password: it opens the door.

## Hybrid mode

A Smart Lock with Wi-Fi or Thread can publish its state to the broker itself (Nuki's official
MQTT API, topics `nuki/<Nuki ID>/...`). In hybrid mode the component listens there, like
[Nuki Hub](https://github.com/technyon/nuki_hub/blob/master/HYBRID.md) does:

- A state change is published on `<prefix>/lock/state` the moment the lock reports it, without
  a Bluetooth connection.
- What the lock's topics do not carry (battery, trigger, keypad, log, settings) is read over
  Bluetooth once the lock stands still.
- With `send_actions: true` a lock action goes out over MQTT as well and is answered on
  `actionResult` when the lock confirms it. If it does not within `fallback_timeout`, the
  action is sent over Bluetooth.
- While the lock's own MQTT connection is offline, the state is read over Bluetooth every
  `offline_update_interval`.

This matters most when paired as app, which a Smart Lock Ultra, 5th Gen and Go always are:
such a pairing gets no announcement of state changes over Bluetooth.

```yaml
nuki_hub:
  - id: front_door
    type: lock
    topic_prefix: frontdoor
    pairing_as_app: true
    hybrid_mode:
      send_actions: true
```

| Option | Default | Description |
| --- | --- | --- |
| `topic_prefix` | `nuki/<Nuki ID in hex>` | The path the lock publishes under, if it is not the usual one |
| `send_actions` | `false` | Send lock, unlock, unlatch, lock 'n' go and full lock over MQTT. Needs "Allow locking" in the Nuki app's MQTT settings. Fob actions always use Bluetooth |
| `ble_fallback` | `true` | Send an action over Bluetooth if the lock did not confirm it. With `false` it is answered with `timeOut` or `failed` |
| `fallback_timeout` | `2s` | How long to wait for the confirmation |
| `offline_update_interval` | `10min` | State reads over Bluetooth while the lock is offline on MQTT, `0s` for none |

Set up MQTT in the Nuki app with the broker the ESP32 uses, without "Auto discovery". Whether
the lock is online there is published on `<prefix>/lock/hybridConnected` (`1` or `0`), as
Nuki Hub does. With "Allow locking" anyone who may publish to `nuki/<Nuki ID>/lockAction`
can operate the lock: restrict that topic on the broker to the lock and the ESP32.

Hybrid mode has not been run on a lock: it compiles and follows the code ESPHome Nuki Lock
uses for it.

## Security

- **Broker access.** Whoever may publish to `<prefix>/opener/action` can open the door, and
  whoever may read `keypad/json` with `keypad_publish_code` sees the codes. Give every room
  its own broker user and restrict the topics with the broker's access control list.
- **Encryption.** ESPHome's `mqtt:` supports TLS (`certificate_authority`, with the ESP-IDF
  framework). On an open network use it.
- **The pairing key** in `secrets.yaml` and in the ESP32's flash opens the door like a
  paired phone does. Keep the file private; flash encryption protects the copy on the chip.
- **The security PIN** is never logged by this component. The `debug: connect` option of the
  Bluetooth code does log it, leave that off outside the workbench.

## Differences from Nuki Hub

- No web configurator: settings are in the YAML. Firmware updates are ESPHome's OTA.
- Home Assistant discovery over MQTT covers the main entities, not every setting.
- Per-entry keypad topics are keyed by code id (`keypad/codes/<code id>`), not by position.
  No per-entry topics for authorizations and no single value topics next to the JSON ones.
- `authorization/action` adds an authorization only with `authorization_add: true`.
- Messages are published with QoS 0 unless `mqtt_qos: 1` is set.
- No GPIO lock control and no HTTP bridge API. [Hybrid mode](#hybrid-mode) has fewer settings.
- An accepted lock action is given 15 seconds (an Opener: strike delay plus strike duration
  plus 5 seconds, if that is more) to report its completion instead of the command timeout, so an action is not repeated
  because the strike was still held.
- Log gaps are requested from one entry earlier than Nuki Hub does: an Opener with firmware
  1.12.4 answers with the entries after the requested index.
