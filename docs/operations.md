# Running it

Many rooms, memory, and what has been tested. Setup is in [nuki_hub.md](nuki_hub.md), measurements in [benchmarks.md](benchmarks.md).

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
- A sync changes at most 50 entries at a time, see [sync](topics.md#additions-to-nuki-hubs-topics).

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

Not run on hardware: **hybrid mode**, a **Smart Lock** (its code shares the queue, keypad, authorization and
MQTT code with the Opener and compiles on both frameworks), pairing with the button (the
pairing was taken over from Nuki Hub), a ring of the bell, wrong codes on the keypad, a
and a real Nuki device losing power or range (only simulated with a device that never
existed).
