# Topics and commands

What a `nuki_hub` device publishes and accepts, over MQTT or the ESPHome API. Setup is in [nuki_hub.md](nuki_hub.md), advice for writing a client in [client_guide.md](client_guide.md).

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

Messages are published with QoS 0. With QoS 1 (`mqtt_qos: 1`) the broker acknowledges every
message with a small packet of its own, and the next command for the ESP32 waits behind
those in the broker's connection until the ESP32 has acknowledged them, which a small TCP
stack does up to a quarter of a second later. Measured, that is 115 ms on every command that
follows another. A broker that sends without delay (`set_tcp_nodelay true` in Mosquitto)
should not have this cost; that was not tried here. What QoS 1 adds is a repeat after a
lost connection, and after one everything is read and published again anyway.

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
