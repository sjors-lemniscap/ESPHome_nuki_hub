# Measurements

All on one rig: ESP32-S3 (8 MB PSRAM) on Wi-Fi, a Nuki Opener (firmware 1.12.4, hardware
4.17) with a Keypad 2 about a metre away, MQTT broker on the local network. Times are what
a client sees over MQTT: from publishing a command to receiving its result. Measured on
2026-11-23 with `tests/rig/benchmark.js` and the harness of the booking system that uses
these topics.

## Single commands

| Operation | average | median | 95% | slowest |
| --- | --- | --- | --- | --- |
| Keypad add | 384 ms | 347 ms | 688 ms | 688 ms |
| Keypad update | 386 ms | 401 ms | 594 ms | 594 ms |
| Keypad delete | 407 ms | 374 ms | 649 ms | 649 ms |
| State read, connection open | 292 ms | 292 ms | 497 ms | 497 ms |
| Keypad list read, new connection | 1510 ms | 1110 ms | 3770 ms | 3770 ms |

20 commands each, 10 for the reads.

## Many commands

| Operation | total | per command |
| --- | --- | --- |
| 20 adds as one batch | 4.6 s | 232 ms |
| 20 deletes as one batch | 4.6 s | 229 ms |
| Sync that adds 20 entries (reads the list first) | 5.0 s | 251 ms |
| Sync with nothing to do (reads the list) | 1.2 s | |
| Sync that deletes 20 entries | 6.6 s | 330 ms |

A batch saves the trip over Wi-Fi and the broker for every command but the first. Wi-Fi
power saving, which the ESP32 needs while Bluetooth is on, delays incoming packets by up to a
beacon interval (about 100 ms), and that is most of the difference to a single command.

## What the tuning did

| | single keypad command | batch of 20 |
| --- | --- | --- |
| Stepping a command every 20 ms, default connection interval | 458-475 ms | 6.1 s |
| Stepping continuously, connection interval 7.5-15 ms (`fast_connection`, the default) | 384-407 ms | 4.6 s |

## Under load

| Run | Commands | Lost | Bluetooth retries | Restarts |
| --- | --- | --- | --- | --- |
| Harness with activity log checks and a 20 code burst | 72 | 0 | 0 | 0 |
| The same while another device held the Opener's connection for 20 s | 72 | 0 | 0 | 0 |
| One hour, a 70 command burst every five minutes | 732 | 0 | 0 | 0 |

In the one hour run a burst of 20 adds, 20 updates, 10 commands at once and 20 deletes took
35 to 45 seconds, waits for the keypad list included. Free internal RAM stayed at about
140 kB with 8 MB PSRAM present.

## Not measured

A Smart Lock, a board without PSRAM, Ethernet, a keypad with more than about 25 entries, and
more than one Nuki device on one ESP32.
