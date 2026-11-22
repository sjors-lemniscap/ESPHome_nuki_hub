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
- [Command queue and results](#command-queue-and-results)
- [ESPHome automations](#esphome-automations)
- [Differences from Nuki Hub](#differences-from-nuki-hub)
- [Test status](#test-status)

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
| `debug` | | `connect`, `communication`, `readable_data`, `hex_data`, `command`: extra logging of the Bluetooth protocol |

Nuki Hub's connection state and log topics are plain MQTT features and are set in the `mqtt:`
block, see the example: `birth_message`/`will_message` on
`<prefix>/maintenance/mqttConnectionState` and `log_topic` on `<prefix>/maintenance/log`.

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
| `action` | write | `activateRTO`, `deactivateRTO`, `electricStrikeActuation`, `activateCM`, `deactivateCM` (Opener); `unlock`, `lock`, `unlatch`, `lockNgo`, `lockNgoUnlatch`, `fullLock` (Smart Lock); `fobAction1` to `3`. Answered with `ack` or `unknown_action` on the same topic |
| `commandResult` | read | result of the action: `success`, `failed`, `timeOut`, `busy`, `notPaired`, `error` |
| `query/lockstate`, `query/config`, `query/battery`, `query/keypad` | write | `1` reads again |
| `query/lockstateCommandResult` | read | |
| `keypad/json` | read | all keypad entries |
| `keypad/actionJson` | write | `{"action":"add","name":"...","code":345678,"enabled":1,"timeLimited":1,"allowedFrom":"2026-09-17 08:00:00","allowedUntil":"2026-09-17 10:00:00","allowedWeekdays":["mon","tue"],"allowedFromTime":"08:00","allowedUntilTime":"10:00"}`, `update` and `delete` with `codeId` |
| `keypad/commandResultJson` | read | `success` or the reason it was refused |
| `authorization/json` | read | |
| `authorization/action` | write | `update` and `delete` with `authId` |
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
| `maintenance/restartReasonNukiHub` | read | `RequestedViaMqtt` or `NotApplicable` |
| `maintenance/restartReasonNukiEsp` | read | the ESP32's reset reason |
| `maintenance/mqttConnectionState` | read | `online` / `offline` (set in `mqtt:`) |
| `maintenance/log` | read | log lines (set in `mqtt:`) |
| `maintenance/reset` | write | `1` restarts the ESP32 |
| `info/nukiHubVersion`, `info/nukiHubBuild`, `info/nukiHubIp` | read | this component's version, the ESPHome version, the IP address |

## Command queue and results

Only one command at a time can travel over the Bluetooth link. Commands that arrive while
another one is running wait in a queue (24 deep) and are answered one after the other, in the
order they arrived; lock actions go first. A command that gets no answer is retried
(`command_retries`). A command the device refuses is answered with `failed` right away.
If the Nuki device cannot be reached within 10 seconds (out of range, without power, or
another device holds its only Bluetooth connection), the attempt ends with `timeOut` and is
retried like any other. `availability` goes `offline` after five commands in a row without an
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
| `nuki_hub.set_pairing_mode` | `pairing_mode: true` looks for a Nuki device in pairing mode until one is paired |
| `nuki_hub.unpair` | Forgets the pairing |
| `nuki_hub.paired` | Condition: a pairing is stored |
| `nuki_hub.available` | Condition: the Nuki device answered the last commands and is advertising |

## Differences from Nuki Hub

- No web configurator: settings are in the YAML. Firmware updates are ESPHome's OTA.
- No Home Assistant discovery over MQTT. Use the lock entity over the ESPHome API, or
  ESPHome's own MQTT discovery for it.
- No per-entry topics (`keypad/codes/N`, `authorization/entries/N`) and no single value
  topics next to the JSON ones.
- `authorization/action` cannot add an authorization, `keypad/actionJson` has no `check`.
- No hybrid mode (official MQTT of Smart Lock 4/5), GPIO lock control, HTTP bridge API.
- An accepted lock action is given 15 seconds (an Opener: the strike duration plus 5 seconds)
  to report its completion instead of the command timeout, so an action is not repeated
  because the strike was still held.
- Log gaps are requested from one entry earlier than Nuki Hub does: an Opener with firmware
  1.12.4 answers with the entries after the requested index.

## Test status

Tested on an ESP32-S3 with a **Nuki Opener** (firmware 1.12.4, hardware 4.17) and a keypad.
The Smart Lock side shares the queue, keypad, authorization and MQTT code with the Opener
and compiles, but **has not been run against a Smart Lock**.
