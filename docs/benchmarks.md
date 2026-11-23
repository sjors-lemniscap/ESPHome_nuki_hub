# Measurements

All on one rig: ESP32-S3 (8 MB PSRAM) on Wi-Fi, a Nuki Opener (firmware 1.12.4, hardware
4.17) with a Keypad 2 about a metre away, MQTT broker on the local network. Times are what
a client sees over MQTT: from publishing a command to receiving its result. Measured on
2026-11-24 with `tests/rig/benchmark.js` and the harness of the booking system that uses
these topics.

## Single commands

| Operation | average | median | 95% | slowest |
| --- | --- | --- | --- | --- |
| Keypad add | 148 ms | 148 ms | 205 ms | 205 ms |
| Keypad update | 144 ms | 113 ms | 508 ms | 508 ms |
| Keypad delete | 123 ms | 110 ms | 194 ms | 194 ms |
| State read, connection open | 236 ms | 239 ms | 292 ms | 292 ms |
| Keypad list read, new connection | 1046 ms | 1098 ms | 1453 ms | 1453 ms |

20 commands each, 10 for the reads, with `keypad_fast_list: true`.

## Over the ESPHome API instead of a broker

`api_transport: true`, a client connected to the ESP32 itself, 9 commands each
(`tests/rig/api_transport.py`):

| Operation | average | median | slowest |
| --- | --- | --- | --- |
| Keypad add (the first one opens the connection) | 223 ms | 149 ms | 902 ms |
| Keypad update | 129 ms | 110 ms | 166 ms |
| Keypad delete | 109 ms | 104 ms | 146 ms |

A little faster than over MQTT, which has the broker in between.

## Many commands

| Operation | total | per command |
| --- | --- | --- |
| 20 adds as one batch | 2.5 s | 126 ms |
| 20 deletes as one batch | 2.4 s | 118 ms |
| Sync that adds 20 entries (reads the list first) | 2.8 s | 138 ms |
| Sync with nothing to do (reads the list) | 1.2 s | |
| Sync that deletes 20 entries | 3.4 s | 172 ms |

A batch saves the trip over Wi-Fi and the broker for every command but the first. Wi-Fi
power saving, which the ESP32 needs while Bluetooth is on, delays incoming packets by up to a
beacon interval (about 100 ms). Times of single commands therefore vary from run to run with
the Wi-Fi link more than with anything in the firmware.

## What the tuning did

| | single keypad command | batch of 20 |
| --- | --- | --- |
| Stepping a command every 20 ms, default connection interval | 458-475 ms | 6.1 s |
| Stepping continuously, connection interval 7.5-15 ms (`fast_connection`, the default) | 384-407 ms | 4.6 s |
| The same with `keypad_fast_list: true` | 220-312 ms | 4.7 s |
| Commands started the moment they arrive, published with QoS 0 | 190-225 ms | 2.7 s |

## Where the time of a command goes

Keypad updates one right after the other on an open connection (`tests/rig/quick_bench.js`),
what the client waits and what the ESP32 itself needs from receiving a command to answering:

| | client | ESP32 |
| --- | --- | --- |
| Before: a look at the queue every 100 ms while idle, QoS 1 | 380-420 ms | 174 ms |
| Commands started the moment they arrive | 340-380 ms | 134 ms |
| And published with QoS 0 | 220-235 ms | 130-135 ms |

The 130 ms on the ESP32 are the exchange with the Opener: a challenge, the command, its
status, each waiting for the next connection event, 15 ms apart. The Opener chooses that
interval itself: a connection opened at 7.5 ms is moved to 15 ms by it shortly after.
Packets are not the limit (the Opener agrees to 247 bytes, a command fits in one), and
writing without waiting for an acknowledgement gained nothing. Keeping Wi-Fi out of power
save while commands arrive gained nothing either. What QoS 1 costs is explained with the
[results](topics.md#command-queue-and-results).

Since then (2026-11-24) the challenge of the next command is asked for when a command is
done (`challenge_prefetch`), and is there when the next one arrives:

| Keypad update on an open connection | client | ESP32 |
| --- | --- | --- |
| Challenge asked for when the command arrives | 220-235 ms | 130-135 ms |
| Challenge asked for ahead | 148 ms | 75 ms |

The Opener takes a challenge that is seconds old, and one with a status read in between. In
a batch the commands follow each other without a pause, so there is nothing to ask ahead in:
a batch of 20 stays at 2.4 to 2.5 s.

## The first command after a pause

Two seconds after a command the connection is closed. The next command then has to wait
until the Opener advertises (about every 0.4 s in its advertising mode Normal) and the
ESP32 hears it. Time on the ESP32 for a keypad update, with `tests/rig` probes:

| | median | range |
| --- | --- | --- |
| Before: the radio listened 30 ms of every 60 ms for the advertisement | 1180 ms | 720-3110 ms |
| Listening without gaps | 640 ms | 240-1130 ms |
| Connection held open (`keep_connected` or `connection/hold`) | 70 ms | 67-81 ms |

Finding the services again is not part of it: the stack remembers them, that step takes no
measurable time. The Opener closes a connection by itself after about ten seconds without
traffic, so a held connection asks for a fresh challenge every five seconds, which is also
the one the next command uses.

What remains for a client on Wi-Fi is the network: 70 ms on this rig while commands follow
each other, 170 to 230 ms for the first one after a pause, because Wi-Fi power saving
(needed while Bluetooth is on) lets a packet wait for the next beacon. Wired Ethernet does
not have that.

## Under load

| Run | Commands | Lost | Bluetooth retries | Restarts |
| --- | --- | --- | --- | --- |
| Harness with activity log checks and a 20 code burst | 72 | 0 | 0 | 0 |
| The same while another device held the Opener's connection for 20 s | 72 | 0 | 0 | 0 |
| Both again on the final build: average 207 ms per command, 95% below 295 ms | 144 | 0 | 0 | 0 |
| One hour, a 70 command burst every five minutes | 732 | 0 | 0 | 0 |

In the one hour run a burst of 20 adds, 20 updates, 10 commands at once and 20 deletes took
35 to 45 seconds, waits for the keypad list included. Free internal RAM stayed at about
140 kB with 8 MB PSRAM present.

With `keypad_topic_per_entry` only the entries that changed are published after a
command. Before, every entry and its Home Assistant switch were published again each time:
two messages per entry per command, about 50 for a keypad with 25 entries, more than the
29 the MQTT client of ESPHome queues with `idf_send_async`. Messages the client cannot
take at once now wait in the component and go out in order.

## A full keypad

80 entries added by a sync next to the one that was there (`tests/rig/large_keypad.js`):

| | with PSRAM | without |
| --- | --- | --- |
| Sync that adds 80 entries (two parts of 50 and 30) | 13.4 s, 168 ms each | 21.6 s, 270 ms each (before the last speed-up) |
| List read, 25.6 kB | 132 ms | 4.7 s, in 9 pages |
| Single add / update / delete with all there | 459 / 985 / 295 ms | 931 / 271 / 322 ms |
| Sync that removes them | 14.6 s | 23.1 s |
| Lowest free internal RAM | 88 kB | 32 kB |

## Not measured

A Smart Lock, a board with Ethernet (it compiles), and more than one Nuki device on one
ESP32.
