# Moving a room from Nuki Hub to ESPHome

The ESP32 that ran Nuki Hub keeps its place, its Nuki device and its MQTT topics. Nobody has
to press the pairing button: the pairing Nuki Hub made is taken over.

## Before you start

- ESPHome 2026.10 or newer on the computer that builds the firmware.
- A USB cable to the ESP32 for the first installation. Later updates go over the air.
- Do one room first and leave it running for a few days.

## 1. Back up Nuki Hub

```bash
esptool --port /dev/ttyACM0 read-flash 0 ALL nukihub-backup.bin
```

This file is the way back, see [Going back](#going-back). It contains the pairing key and
the Wi-Fi and broker passwords: store it like a password.

## 2. Extract the pairing

```bash
python3 tools/nuki_hub_pairing.py --dump nukihub-backup.bin --out secrets-room.yaml
```

(`--port /dev/ttyACM0` instead of `--dump` reads the settings straight from the ESP32, and
`--device opener` or `lock` chooses if Nuki Hub was paired with both.) The tool prints what
it found and writes the values to the file: Bluetooth address, pairing
key, authorization id, security PIN, and the Wi-Fi and broker settings Nuki Hub used. Copy
them into your `secrets.yaml`.

Note Nuki Hub's "MQTT path" (`mqtt_path` in the file). It becomes `topic_prefix`, so the
topics stay the same.

## 3. Write the configuration

Start from [`examples/nuki_hub_opener.yaml`](../examples/nuki_hub_opener.yaml), or for
several rooms from [`examples/fleet`](../examples/fleet). Settings that had a switch in
Nuki Hub's web interface:

| Nuki Hub | nuki_hub |
| --- | --- |
| MQTT path | `topic_prefix` |
| Nuki PIN | `security_pin` |
| Keypad control | `keypad_control` (on by default) |
| Publish keypad codes | `keypad_publish_code` |
| Publish authorization log | `publish_activity_log` (on by default) |
| Update time of the Nuki device | `time_id` and `update_time: true` |
| MQTT logging | `mqtt_log: INFO` |
| Query intervals | `query_interval_state`, `_config`, `_battery`, `_keypad` |
| Number of retries, retry delay | `command_retries`, `command_retry_delay` |
| Restart on missing beacons | no equivalent. `bluetooth_watchdog` restarts the ESP32 when commands keep failing although the Nuki device is advertising |

## 4. Install

```bash
esptool --port /dev/ttyACM0 erase-flash
esphome run room.yaml --device /dev/ttyACM0
```

Erasing first removes Nuki Hub's settings and partition layout. The log then shows

```
Took over the pairing given in the configuration
Security PIN is valid
Opener state: locked ...
```

## 5. Check

- `<prefix>/opener/state` (or `lock/state`) and `keypad/json` are published.
- A keypad command on `keypad/actionJson` is answered with `success` on
  `keypad/commandResultJson`.
- `<prefix>/opener/status` shows `"available":true,"pinValid":true`.

The `pairing:` block is only read while the ESP32 has no pairing of its own. It can stay in
the configuration or be removed after the first start.

## Going back

```bash
esptool --port /dev/ttyACM0 write-flash 0 nukihub-backup.bin
```

puts Nuki Hub back exactly as it was, settings and pairing included. The Nuki device does
not notice the difference: both firmwares use the same authorization.

Do not run both at the same time on two ESP32s with the same pairing.

## Home Assistant entities of Nuki Hub

If Nuki Hub announced itself to Home Assistant, its discovery topics are still on the broker
after the move and its entities stay next to the new ones. Publish `1` to
`<prefix>/maintenance/removeNukiHubDiscovery` to clear those of the Nuki device, see
[Home Assistant](home_assistant.md#coming-from-nuki-hub).
