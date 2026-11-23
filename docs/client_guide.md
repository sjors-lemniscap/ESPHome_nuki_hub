# Writing a client

For software that manages door codes through these topics, such as a booking system. The
[topic reference](nuki_hub.md#topics) lists everything; this is about how to use it well.
A client written for Nuki Hub works unchanged. What is described here makes it simpler and
faster, and can be adopted step by step: every room that runs Nuki Hub simply does not
answer on the additional topics.

## Telling the firmware apart

`<prefix>/info/nukiHubVersion` starts with `esphome-` on this firmware. A client that talks to
both kinds of rooms can decide per room whether to use the additions.

## One command, one answer

Give every command a `requestId` and wait for the JSON result that carries it:

```
publish   <prefix>/opener/keypad/actionJson   {"action":"add","name":"booking_4711","code":345678,"requestId":"k1"}
receive   <prefix>/opener/keypad/result       {"requestId":"k1","action":"add","result":"success","codeId":105,"durationMs":371}
```

- No guessing whose command an answer belongs to when several clients publish.
- `codeId` is there at once. With Nuki Hub the client learns it from the next list.
- `result` other than `success`:
  - `failed` with `error`: the Nuki device refused. `K_ERROR_CODE_ALREADY_EXISTS` means
    another entry has this code.
  - `timeOut`, `error`, `busy`: the device did not answer after all retries, or the queue was
    full. Nothing was changed, as far as the firmware knows. Send the command again; an `add`
    that did arrive is recognised and answered with `"existed":true`.
  - `expired`: the command waited longer than its `timeoutMs`. Nothing was changed.
  - anything else (`noValidCodeSet`, `noExistingCodeIdSet`, ...): the command itself was wrong.
- A result can arrive twice. Results are sent so that they arrive at least once, and after a
  short Wi-Fi stall the broker may deliver the same message again. Ignore a `requestId` that
  was answered already.
- Set `timeoutMs` to what the client is prepared to wait. A command that the client has
  given up on is then not executed late.

## Many changes at once

A day's worth of codes is one message:

```
publish   <prefix>/opener/keypad/batchJson    {"requestId":"b1","commands":[{"action":"add",...},{"action":"delete","codeId":7}]}
receive   <prefix>/opener/keypad/batchResult  {"requestId":"b1","result":"success","succeeded":2,"failed":0,"results":[...]}
```

About 230 ms per command instead of 400, and one answer to wait for. At most 50 commands.

## Or: say what should be there

If the client owns all entries with a name prefix, it can leave the comparing to the device:

```
publish   <prefix>/opener/keypad/syncJson     {"requestId":"s1","prefix":"booking_","entries":[...all wanted entries...]}
receive   <prefix>/opener/keypad/syncResult   {"requestId":"s1","result":"success","adds":1,"updates":0,"deletes":2,"unchanged":14,...}
```

Entries with other names (hand-made ones, other systems) are never touched. Send
`"dryRun":true` first to see what would change. A sync is safe to repeat: the second one
reports everything `unchanged`. A sync does at most 50 changes; if its result has
`remaining`, send the same sync again until it has not.

Entries are matched by name, so names must be unique within the prefix, and at most 20
characters long.

## Keeping a copy of the keypad

- `keypad/json` (retained) is the list as last read from the device.
- `keypad/changed` announces every single change as it happens.
- `keypad/count` is the number of entries.

A client that applies `keypad/changed` to its copy and compares with `keypad/json` now and
then does not have to request the list after its own commands.

## Who came in

`event/keypad` arrives once per use of the keypad, in order, also for entries that happened
while the ESP32 or the broker was away (they are caught up from the device's log). Shortened:

```json
{"index":812,"authorizationName":"Keypad","type":"KeypadAction","action":"ActivateRTO","trigger":"code","completionStatus":"success","codeId":105,"time":"2026-11-23 09:14:02"}
```

`completionStatus` `invalidCode` is a wrong code. `event/ring` is the bell, `event/action`
a lock action. Events are not retained: a client that was offline can ask for what it missed
with `query/logRange` and the `index` of the last entry it saw.

## Is the room healthy

- `<prefix>/maintenance/mqttConnectionState`: `offline` means the ESP32 is gone.
- `<prefix>/opener/status`: `available:false` means the ESP32 is there but the Nuki device
  does not answer; `pinValid:false` that the security PIN is wrong; `lastSeenSecondsAgo`
  grows when the Nuki device stops advertising (battery, range).
- `<prefix>/maintenance/uptime` falling back to 0 and `restartReasonNukiEsp` tell about restarts.
- `<prefix>/opener/bluetoothJson`: `retries` and `failed` growing fast point at a poor
  Bluetooth link.
