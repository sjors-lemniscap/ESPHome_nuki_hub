# nuki_hub: Nuki Hub's MQTT interface on ESPHome

`nuki_hub` is a second component in this repository, next to the `nuki_lock` lock platform.
It connects a **Nuki Opener** or a **Nuki Smart Lock** over Bluetooth and makes it available
over MQTT with the topics and payloads of [Nuki Hub](https://github.com/technyon/nuki_hub).
Software written against Nuki Hub (a booking system that manages keypad codes, for example)
keeps working, while the ESP32 itself is configured, built and updated like any other
ESPHome device.

- [What it does](#what-it-does)
- [Configuration](#configuration)
- [Pairing](#pairing)
- [Topics](#topics)
- [Additions to Nuki Hub's topics](#additions-to-nuki-hubs-topics)
- [Command queue and results](#command-queue-and-results)
- [ESPHome automations](#esphome-automations)
- [Home Assistant](#home-assistant)
- [Security](#security)
- [Differences from Nuki Hub](#differences-from-nuki-hub)
- [Test status](#test-status)
- [Measurements](benchmarks.md)

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
    components: [nuki_lock, nuki_hub]

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
| `api_transport` | `false` | The topics over the ESPHome API as well: an action for commands and an event for everything published, see [Without a broker](#without-a-broker) |
| `authorization_add` | `false` | Let a client create authorizations with `authorization/action`. An authorization is permanent access to the Nuki device for whoever holds its key |
| `bluetooth_watchdog` | `30min` | Restart the ESP32 if no command got through for this long although the Nuki device is advertising. `0s` turns it off |
| `fast_connection` | `true` | Ask for a short Bluetooth connection interval (7.5 to 15 ms) while connected. Commands get faster, see [measurements](benchmarks.md) |
| `ble_disconnect_timeout` | `2s` | How long the Bluetooth connection is kept open after a command, for the next one |
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
| `ble_command_timeout` | `3s` | |
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

## Topics

All below `<prefix>/opener` or `<prefix>/lock`. Topics in the "write" rows are subscribed.

| Topic | | Payload |
| --- | --- | --- |
| `state`, `hastate`, `binaryState`, `trigger`, `lastLockAction`, `completionStatus` | read | text |
| `continuousMode`, `ring`, `binaryRing` | read | Opener only |
| `json` | read | the state as one JSON object |
| `availability` | read | `online` / `offline`: whether the Nuki device answers |
| `rssi`, `address`, `statusUpdated`, `retry` | read | |
| `battery/basicJson`, `battery/advancedJson` | read | JSON |
| `info/firmwareVersion`, `info/hardwareVersion` | read | of the Nuki device |
| `action` | write | `activateRTO`, `deactivateRTO`, `electricStrikeActuation`, `activateCM`, `deactivateCM` (Opener); `unlock`, `lock`, `unlatch`, `lockNgo`, `lockNgoUnlatch`, `fullLock` (Smart Lock); `fobAction1` to `3`. Answered on the same topic with `ack`, `unknown_action`, or `denied` while not paired |
| `commandResult` | read | result of the action: `success`, `failed`, `timeOut`, `busy`, `notPaired`, `error` |
| `query/lockstate`, `query/config`, `query/battery`, `query/keypad` | write | `1` reads again, and is answered with `0` on the same topic |
| `query/lockstateCommandResult` | read | |
| `doorSensorState` | read | Smart Lock only |
| `keypad/json` | read | all keypad entries |
| `keypad/count` | read | their number |
| `keypad/actionJson` | write | Names longer than 20 characters are cut off.  `{"action":"add","name":"...","code":345678,"enabled":1,"timeLimited":1,"allowedFrom":"2026-09-17 08:00:00","allowedUntil":"2026-09-17 10:00:00","allowedWeekdays":["mon","tue"],"allowedFromTime":"08:00","allowedUntilTime":"10:00"}`, `update` and `delete` with `codeId` |
| `keypad/commandResultJson` | read | `success` or the reason it was refused |
| `authorization/json` | read | |
| `authorization/action` | write | `update` and `delete` with `authId`. `add` (`name`, optional `type` app/bridge/fob/keypad, `sharedKey` as 64 hex characters, `remoteAllowed`) only with `authorization_add: true`, otherwise `addNotSupported`. A key made by the ESP32 is told once on `authorization/added`, not retained |
| `authorization/commandResult` | read | |
| `timecontrol/json` | read | |
| `timecontrol/action` | write | `{"action":"add","weekdays":["mon"],"time":"08:00","lockAction":"activateRTO"}`, `update` and `delete` with `entryId` |
| `timecontrol/commandResult` | read | |
| `configuration/basicJson`, `configuration/advancedJson` | read | |
| `configuration/action` | write | `{"soundLevel":128,"doubleButtonPressAction":"Toggle CM"}` |
| `configuration/commandResult` | read | one result per setting and `general` |
| `log`, `shortLog` | read | the newest entries as a JSON array |
| `rollingLog` | read | every new entry once, oldest first, not retained |
| `lastRollingLog` | read | index of the last entry sent on `rollingLog`, retained. Read back after a restart so nothing is sent twice or left out |
| `authorizationId`, `authorizationName` | read | of the last action |

Below `<prefix>`:

| Topic | | Payload |
| --- | --- | --- |
| `maintenance/uptime` | read | minutes |
| `maintenance/freeHeap`, `maintenance/wifiRssi` | read | |
| `maintenance/restartReasonNukiHub` | read | `RequestedViaMqtt`, `BluetoothWatchdog` or `NotApplicable` |
| `maintenance/networkDevice` | read | `Built-in Wi-Fi` or `Ethernet` |
| `maintenance/restartReasonNukiEsp` | read | the ESP32's reset reason |
| `maintenance/mqttConnectionState` | read | `online` / `offline` (set in `mqtt:`) |
| `maintenance/log` | read | log lines, with `mqtt_log` |
| `maintenance/reset` | write | `1` restarts the ESP32 |
| `maintenance/removeNukiHubDiscovery` | write | `1` clears the Home Assistant discovery topics Nuki Hub left on the broker for this Nuki device, answered with their number after ten seconds |
| `info/nukiHubVersion`, `info/nukiHubBuild`, `info/nukiHubIp` | read | this component's version, the ESPHome version, the IP address |

## Additions to Nuki Hub's topics

Everything above is Nuki Hub's contract. The topics below exist only here. A client that
ignores them works as before, so the same client can talk to rooms on either firmware.

**Request ids.** Every JSON command (`keypad/actionJson`, `timecontrol/action`,
`authorization/action`, `configuration/action`) may carry a `requestId`, a string or number
of the client's choice. A lock action can be sent as JSON too:
`{"action":"activateRTO","requestId":"a1"}` on `action`. The id comes back in the JSON
result, so several clients can tell their own commands apart.

**JSON results**, not retained, next to the plain word Nuki Hub publishes:

| Topic | For |
| --- | --- |
| `keypad/result` | `keypad/actionJson` |
| `timecontrol/result` | `timecontrol/action` |
| `authorization/result` | `authorization/action` |
| `actionResult` | `action` |
| `configuration/commandResult` | already JSON, now with `requestId` |

```json
{"requestId":"a1","action":"add","result":"success","durationMs":412,"codeId":105}
{"requestId":"a2","action":"add","result":"failed","error":"K_ERROR_CODE_ALREADY_EXISTS","errorCode":42,"durationMs":377}
```

- `result` is the same word as on the plain topic.
- `error` and `errorCode` are there when the Nuki device refused the command.
- `durationMs` counts from the arrival of the command, time in the queue included.
- `codeId` of an `add` is the id the device gave the new code, so it is known without
  reading the list.

**`keypad/changed`**, not retained: the one entry a successful keypad command changed.

```json
{"change":"added","codeId":105,"entry":{"codeId":105,"enabled":1,"name":"booking_4711","timeLimited":1,"allowedFrom":"2026-09-17 08:00:00","allowedUntil":"2026-09-17 10:00:00","allowedWeekdays":[],"allowedFromTime":"00:00","allowedUntilTime":"00:00"}}
{"change":"deleted","codeId":105}
```

`keypad/json` is still published after the commands are through and stays the reference.

**Batch.** Several keypad commands in one message on `keypad/batchJson`, one combined answer
on `keypad/batchResult` (at most 50 commands):

```json
{"requestId":"b1","commands":[{"action":"add","name":"booking_1","code":345678},{"action":"delete","codeId":7}]}
{"requestId":"b1","result":"partial","succeeded":1,"failed":1,"durationMs":911,"results":[{"index":0,"action":"add","name":"booking_1","result":"success","codeId":105},{"index":1,"action":"delete","result":"noExistingCodeIdSet","codeId":7}]}
```

**Sync.** The wanted state of all entries whose name starts with a prefix, on `keypad/syncJson`:

```json
{"requestId":"s1","prefix":"booking_","entries":[{"name":"booking_1","code":345678,"enabled":1,"timeLimited":1,"allowedFrom":"2026-09-17 08:00:00","allowedUntil":"2026-09-17 10:00:00"}]}
```

The keypad is read, missing entries are added, differing ones updated, and entries with the
prefix that are not in the list deleted. Entries with other names are never touched, and the
prefix must have at least three characters. `"dryRun":true` only reports what would happen.
The answer on `keypad/syncResult` has `prefix`, `adds`, `updates`, `deletes` and `unchanged`,
and, if commands were needed, their results like a batch. A dry run is answered with the
result `dryRun`. Refusals: `invalidPrefix`, `invalidEntryName` (a name without the prefix or
longer than 20 characters), `busy`. A sync does at most 50 changes: with more to do the result carries `remaining`, and
the same sync sent again goes on from there until it is gone.

A batch is refused as a whole with `noCommands`, `tooManyCommands` or `busy`.

**Deadline.** `"timeoutMs"` in a keypad command: if the command waited in the queue longer
than that, it is answered with `expired` and not executed.

**Repeated add.** An `add` the device refuses because the code exists is answered with
`success`, `"existed":true` and the id of the entry, if that entry has the same name, code
and time limits: the answer to an earlier attempt was lost, the entry is there.

**Events**, not retained, one per new activity log entry: `event/keypad` (a code or
fingerprint was used, or refused), `event/action` (a lock action), `event/ring` (Opener: the
bell). The payload is the log entry as on `rollingLog`, with `time` added as one string.
`event/invalidCodes` with `invalid_code_alert`: `{"count":3,"windowSeconds":300}`.

**`query/logRange`**: `{"from":100,"to":120,"requestId":"l1"}` reads log entries by index (at
most 100). The answer on `logRange` has `from`, `to` and `entries`, or one of the refusals
`invalidRange`, `rangeTooLarge`, `notPaired`, `noValidPinSet`.

**Keypad `check`** (with `keypad_check_code`): `{"action":"check","codeId":105,"code":345678}`
is answered with `codeValid` or `codeInvalid`.

**`"ifExpired":true`** in a keypad `delete`: the entry is only deleted if its time limit has
run out when the command is executed, otherwise the answer is `notExpired`. Needs
`keypad_delete_expired`, whose `after` it uses. The automatic removal publishes its combined
result on `keypad/expiredResult`.

**`query/authorizations`, `query/timecontrol`, `query/fingerprints`**: `1` reads the list
again. Fingerprints (Keypad 2) are published on `fingerprint/json` on request only.

**Continuous mode for a time** (Opener): `{"action":"activateCM","timeout":30}` on `action`
switches it on for 30 minutes.

**`audit`**, not retained: every answered lock action, single keypad, authorization and time
control command and log range request, with `command`, `action`, `result` and `requestId`.
Batches, syncs and configuration writes are answered on their own topics only.

**`bluetoothJson`**, retained, once a minute: commands since boot, failures, retries, average
and longest duration.

**`status`**, retained, once a minute and when the availability changes:

```json
{"paired":true,"available":true,"pinValid":true,"rssi":-52,"lastSeenSecondsAgo":0,"queued":0,"consecutiveFailures":0}
```

**`<prefix>/maintenance/freeHeapInternal`** and **`minFreeHeapInternal`**: free internal RAM
now and at its lowest since boot. `freeHeap` includes external RAM (PSRAM) on boards that
have it and so cannot show internal memory running out.

## Command queue and results

Results are published retained, like Nuki Hub does. When the ESP32 connects to the broker,
the command topics and `keypad/commandResultJson` are reset to `--`; the other results keep
their last value.

Only one command at a time can travel over the Bluetooth link. Commands that arrive while
another one is running wait in a queue (64 deep) and are answered one after the other, in the
order they arrived; lock actions go first. A command that gets no answer is retried
(`command_retries`). A command the device refuses is answered with `failed` right away.
Commands that a restart cuts off (the reset topic, a firmware update) are answered with
`interrupted`; in a batch or sync the result is then `partial`. A crash or a power cut
answers nothing.
If the Nuki device cannot be reached within 10 seconds (another device holds its Bluetooth
connection, or it just went out of range), the attempt ends with `timeOut` and is retried,
but only once the device has been heard advertising again. A device that stays silent for 20
seconds while the ESP32 listens is taken as out of range or without power: its commands are
then answered with `timeOut` at once, and no connection is attempted until it advertises
again. This keeps the radio free for the other Nuki devices on the same ESP32; a connection
attempt occupies it for up to ten seconds. `availability` goes `offline` after five commands in a row without an
answer, or when the device has not advertised for three minutes.

After keypad commands the keypad is read again and `keypad/json` is published, once the
queue holds no more keypad commands.

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
| `nuki_hub.action` | Queues a lock action by its name, see `action` under [Topics](#topics). `action` can be a lambda |
| `nuki_hub.command` | Does what a message on a command topic does: `topic` is the part after `<prefix>/<kind>` (`/keypad/actionJson`, `/keypad/syncJson`, `/timecontrol/action`, ...), `payload` the message. Works without MQTT |
| `on_message` (trigger, in the `nuki_hub:` entry) | Fires for everything published below `<prefix>/<kind>` (not the `maintenance` and `info` topics of the ESP32), with `topic` (same form) and `payload`. Works without MQTT |
| `nuki_hub.set_pairing_mode` | `pairing_mode: true` looks for a Nuki device in pairing mode until one is paired |
| `nuki_hub.unpair` | Forgets the pairing |
| `nuki_hub.paired` | Condition: a pairing is stored |
| `nuki_hub.available` | Condition: the Nuki device answered the last commands and is advertising |

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

Anything in the [topics](#topics) table can be an entity of the ESPHome API, without a
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

### Settings

With MQTT discovery the main settings are announced as switches, numbers and selects of
the device (LED, button, sounds, timeouts, button and fob actions; for a Smart Lock auto
lock, night mode and durations, no selects yet). They read `configuration/basicJson` and
`configuration/advancedJson` and write through `configuration/action`.

Every authorization and every time control entry is a switch that enables and disables it,
like the keypad entries with `keypad_topic_per_entry`. Switches of entries that are gone
are withdrawn, also those left from before a restart. Mind that the keypad and the apps are
authorizations themselves: switching one off locks it out.

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

## Without a broker

With `api_transport: true` the same topics are there over the ESPHome API, for a client
that connects to each ESP32 itself:

```yaml
api:
  encryption:
    key: !secret api_key
  custom_services: true
  homeassistant_services: true

nuki_hub:
  - type: opener
    topic_prefix: room_u1
    api_transport: true
```

- **Commands**: the action `nuki_hub_command(device, topic, payload)`. `device` is what the
  topics start with on MQTT, `room_u1/opener`, and `topic` the rest with its slash,
  `/keypad/actionJson`.
- **Everything published**: the event `esphome.nuki_hub` with `device`, `topic` and
  `payload`.

Nothing is retained: a client asks for what it needs after connecting (`/query/keypad`,
`/query/lockstate`, `/query/config`). `mqtt:` can be left out or kept next to it. In Home
Assistant the events can be used in automations as they are. Measured on the rig, a keypad
command takes as long as over MQTT (about 210 ms), see [benchmarks](benchmarks.md).

## Many rooms

`examples/fleet/` has one file with what all rooms share and one per room.
`tools/fleet_generate.py rooms.json out/` writes the per-room files from a list of doors,
makes an API key for each and the lines a client needs to reach them;
`tools/fleet_update.sh` installs a build room by room, the first one as a canary.
`tools/check_config.py room.yaml` names what is easy to get wrong before flashing (the
last will, asynchronous sending, the API flags and key, options that need each other).

## Memory and large keypads

The component's memory is the internal RAM of the ESP32, about 130 kB free with Bluetooth
and Wi-Fi running. ESPHome does not hand PSRAM to ordinary allocations, so what can be
large is placed there on purpose where a board has it: the JSON documents, the text of a
message while it is built, and messages waiting for the MQTT client.

- **With PSRAM**: tested with 82 keypad entries (a 25 kB list), lowest free internal RAM
  94 kB.
- **Without PSRAM**: tested with the same 82 entries, lowest free RAM 32 kB. A list too
  large to publish in one message (from about 40 entries) goes out in pages of 10 instead:
  `keypad/page/0` and following, with their number on `keypad/pages` (`0` while the list
  fits `keypad/json`). At most 24 kB of messages wait for the MQTT client.
- Per-entry topics and their switches are published a few at a time, with a look at the free
  memory in between: after a start a keypad with 80 entries takes a few seconds to appear.
- A sync changes at most 50 entries at a time, see [sync](#additions-to-nuki-hubs-topics).

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
- No hybrid mode (official MQTT of Smart Lock 4/5), GPIO lock control, HTTP bridge API.
- An accepted lock action is given 15 seconds (an Opener: strike delay plus strike duration
  plus 5 seconds, if that is more) to report its completion instead of the command timeout, so an action is not repeated
  because the strike was still held.
- Log gaps are requested from one entry earlier than Nuki Hub does: an Opener with firmware
  1.12.4 answers with the entries after the requested index.

## Test status

Tested on an ESP32-S3 with a **Nuki Opener** (firmware 1.12.4, hardware 4.17) and a Keypad 2:

- `tests/rig/additions.js`, 41 checks of everything this component adds to Nuki Hub's
  contract: all pass.
- The harness of the booking system these topics were built for (keypad lifecycle, time
  windows, queueing of simultaneous commands, activity log without gaps across a restart):
  all pass, also with another device holding the Opener's Bluetooth connection, and over an
  hour of bursts.

The entities of the ESPHome API (lock, binary sensors, sensors, text sensors) were used with
an API client: they show the right values, and locking and unlocking through the lock entity
switches ring to open off and on. They were not used from a Home Assistant.

Failure cases tried on the rig:

- **A second, unreachable device** on the same ESP32 (a pairing with an address that does not
  exist): its commands are answered with `timeOut` in under 100 ms, `availability` is
  `offline`, and the real Opener beside it keeps its normal speed.
- **Broker away for a minute**: reconnected two seconds after it was back, state, settings and
  keypad list published again, the next command answered, no restart.
- **Reset in the middle of a batch** of keypad commands: comes back, reads the keypad, works.
  The answers to the interrupted commands are lost; a sync puts the keypad right.
- **Another device holding the Opener's Bluetooth connection** for 20 seconds during a burst:
  no command lost.

- **Commands from Home Assistant** through its own API: lock and unlock, a setting as a
  switch and one as a number changed and changed back, a keypad code disabled and enabled.
- **Wi-Fi away for a minute**: the last will sets the ESP32 offline at the broker, four
  seconds after Wi-Fi is back everything is published again, no restart, commands work.
- **Home Assistant 2026.10 with its MQTT integration** on the same broker: all announced
  entities appear with their states, go unavailable while the ESP32 is off Wi-Fi, and
  switches of deleted keypad entries do not stay behind.
- **The ESPHome API transport**: 27 keypad commands from a Python client and the booking
  system's agent (Node) adding, updating and deleting a code, without a broker.

- **80 keypad entries**: sync in two parts, list read, single commands and removal, with
  and without PSRAM, no restart.
- **Firmware update over the air during a sync of 60 entries**: installed, the device came
  back paired, the interrupted sync got no answer and the same sync sent again finished it.
- **Creating an authorization** and deleting it again.
- **Builds**: Ethernet without Wi-Fi (Olimex ESP32-POE) and without PSRAM compile; the
  one without PSRAM also ran all checks on the rig.

- **Selects, authorization and time control switches** from a real Home Assistant: a sound
  changed and changed back, an entry of each list added, switched off and deleted, its
  switch withdrawn.
- **A firmware update during a sync** is answered: `partial`, with what was done.

Not run on hardware: a **Smart Lock** (its code shares the queue, keypad, authorization and
MQTT code with the Opener and compiles on both frameworks), pairing with the button (the
pairing was taken over from Nuki Hub), a ring of the bell, wrong codes on the keypad, a
and a real Nuki device losing power or range (only simulated with a device that never
existed).