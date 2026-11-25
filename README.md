# ESPHome Nuki Hub (ESP32)

> **Credits.** This component stands on the work of three projects, and would not exist without them:
>
> - **[NukiBleEsp32](https://github.com/I-Connect/NukiBleEsp32)** by I-Connect and its maintainers: the implementation of Nuki's Bluetooth protocol that the code here derives from, by way of the ESP-IDF port by [AzonInc](https://github.com/AzonInc/NukiBleEsp32/tree/idf).
> - **[Nuki Hub](https://github.com/technyon/nuki_hub)** by technyon and its maintainers: the MQTT interface (topics, payloads, behaviour) that this component follows, and the reference it was tested against.
> - **[ESPHome Nuki Lock](https://github.com/uriyacovy/ESPHome_nuki_lock)** by uriyacovy, AzonInc and its contributors: the ESPHome component this repository is a fork of, and where the Bluetooth code on ESPHome's own stack comes from.

An ESPHome component that puts a Nuki **Opener** or **Smart Lock** on MQTT with the topics and payloads of [Nuki Hub](https://github.com/technyon/nuki_hub), over Bluetooth, without cloud and without a Nuki Bridge. Software written against Nuki Hub keeps working, while the ESP32 is configured, built and updated like any other ESPHome device.

- Lock actions, keypad codes with time limits, time control, authorizations, settings and the activity log, each command answered with a result
- Optional Home Assistant lock entity and sensors through the ESPHome API, and MQTT discovery
- The same topics over the ESPHome API for setups without a broker
- Hybrid mode for Smart Locks with Wi-Fi or Thread: state changes from the lock's own MQTT, the rest over Bluetooth
- The settings of the Nuki device as switches, numbers and selects in Home Assistant, and door sensor emulation for a Smart Lock
- An existing Nuki Hub pairing can be [taken over](docs/migrating_from_nuki_hub.md), and given back

## Supported Nuki devices

| Device | `type` | Status |
| --- | --- | --- |
| Opener | `opener` | Tested on hardware (firmware 1.12.4) |
| Smart Lock 1st to 4th Gen, including Pro | `lock` | Implemented, not yet tried on a device |
| Smart Lock Ultra, 5th Gen, Go | `lock` | Implemented, not yet tried on a device. Needs `pairing_as_app: true` and the 6-digit PIN without leading zeros |
| Keypad and Keypad 2 (codes and fingerprints) | through its Opener or Smart Lock | Keypad 2 codes tested on an Opener, fingerprints not |
| Door Sensor | through its Smart Lock | State and battery are published, not yet tried on a device |

These are the devices Nuki Hub talks to over Bluetooth. The Smart Lock code comes from ESPHome Nuki Lock, where it is used with these locks, and shares the queue, keypad and MQTT code with the Opener here. [Hybrid mode](docs/nuki_hub.md#hybrid-mode) (the lock's own MQTT for instant state changes, on Smart Locks with Wi-Fi or Thread) is implemented too, also not yet tried on a device. What Nuki Hub has and this does not: GPIO lock control and the HTTP bridge API, see [differences](docs/nuki_hub.md#differences-from-nuki-hub).

## Requirements

| | |
| --- | --- |
| ESP32 | Any ESP32 with Bluetooth LE, within reliable Bluetooth range of the Nuki device. Run on an ESP32-S3 |
| ESPHome | 2026.10.0 or newer |
| Framework | `esp-idf` (recommended) or `arduino` |
| MQTT broker | For the Nuki Hub topics. Without one the lock entity and the API transport remain |

## Quick start

```yaml
external_components:
  - source: github://sjors-lemniscap/ESPHome_nuki_hub
    components: [nuki_hub]

esp32_ble_tracker:

mqtt:
  broker: 192.168.1.10
  discovery: false
  log_topic: null
  idf_send_async: true

nuki_hub:
  - id: entrance
    type: opener          # or lock
    topic_prefix: entrance
    security_pin: 1234
    pairing_mode_at_boot: true
```

Flash it with `esphome run <file>.yaml`, put the Nuki device in pairing mode (hold its button for about five seconds) and it pairs by itself; see [Pairing](docs/nuki_hub.md#pairing). A complete configuration is in [`examples/nuki_hub_opener.yaml`](examples/nuki_hub_opener.yaml), one for many rooms in [`examples/fleet`](examples/fleet).

## Documentation

| | |
| --- | --- |
| [Setup](docs/nuki_hub.md) | what it does, every option, pairing, security, differences from Nuki Hub |
| [Topics and commands](docs/topics.md) | every topic, results and the queue, the ESPHome API instead of a broker |
| [Home Assistant and automations](docs/home_assistant.md) | discovery, entities, automations on the ESP32 |
| [Running it](docs/operations.md) | many rooms, memory, large keypads, what has been tested |
| [Writing a client](docs/client_guide.md) | how to use the topics well |
| [Coming from Nuki Hub](docs/migrating_from_nuki_hub.md) | taking over the pairing and going back |
| [Coming from ESPHome Nuki Lock](docs/coming_from_nuki_lock.md) | every option and entity of `platform: nuki_lock` and where it is here |
| [Measurements](docs/benchmarks.md) | times and memory on the test rig |

## Speed and test status

On the test rig (ESP32-S3, Opener with a Keypad 2) a keypad command takes about 110 to 150 ms from publish to result, see the [measurements](docs/benchmarks.md). Nuki Hub on the same hardware averaged about 1 s per command in the same test run.

Everything has been run on an Opener with a Keypad 2 only, see [supported devices](#supported-nuki-devices) and the [test status](docs/operations.md#test-status).

## Coming from ESPHome Nuki Lock

Upstream's `lock: - platform: nuki_lock` is not in this repository, there is one component. What it offers is here under `nuki_hub`: the Smart Lock as a native Home Assistant lock, its settings as switches, numbers and selects, hybrid mode and door sensor emulation, with or without MQTT. [Coming from ESPHome Nuki Lock](docs/coming_from_nuki_lock.md) lists every option and entity and its counterpart, and the few things that are missing. The YAML is not the same and the lock has to be paired again.

## License

MIT, see [LICENSE](LICENSE). The three projects this builds on are MIT licensed as well; NukiBleEsp32 changed from GPL 3 to MIT in October 2024.
