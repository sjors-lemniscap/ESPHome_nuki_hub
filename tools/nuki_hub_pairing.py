#!/usr/bin/env python3
"""Reads the pairing and the connection settings Nuki Hub stored on an ESP32 and writes them
as an ESPHome secrets file, so nuki_hub can take the pairing over without pairing again.

    # from the ESP32 itself (needs esptool, reads the settings partition only)
    python3 tools/nuki_hub_pairing.py --port /dev/ttyACM0 --out secrets.yaml

    # from a flash dump made earlier (a full dump or just the NVS partition)
    python3 tools/nuki_hub_pairing.py --dump nukihub-backup.bin --out secrets.yaml

Nothing is printed but the names of what was found. The output file holds the key that opens
the door: it is created readable by the owner only, keep it that way.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

PAGE = 4096
ENTRY = 32
NVS_OFFSET_DEFAULT = 0x9000
NVS_SIZE_DEFAULT = 0x5000


def find_nvs(dump: bytes):
    """Offset and size of the NVS partition in a full flash dump, from the partition table."""
    table = dump[0x8000:0x8000 + 0xC00]
    for i in range(0, len(table), 32):
        entry = table[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            break
        kind, subtype, offset, size = entry[2], entry[3], *struct.unpack("<II", entry[4:12])
        if kind == 1 and subtype == 2:
            return offset, size
    return None


def parse_nvs(data: bytes):
    """Returns {(namespace, key): value} of an NVS partition, the newest value of each key."""
    namespaces = {}
    values = {}
    pages = []
    for start in range(0, len(data) - PAGE + 1, PAGE):
        page = data[start:start + PAGE]
        state, sequence = struct.unpack("<II", page[:8])
        if state in (0xFFFFFFFF, 0xFFFFFFF0) or state not in (0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8):
            continue
        pages.append((sequence, page))
    for _, page in sorted(pages, key=lambda p: p[0]):
        bitmap = page[32:64]
        index = 0
        blobs = {}
        while index < 126:
            state = (bitmap[index // 4] >> ((index % 4) * 2)) & 3
            entry = page[64 + index * ENTRY:64 + (index + 1) * ENTRY]
            if state != 2:  # 2 = written
                index += 1
                continue
            ns, kind, span = entry[0], entry[1], max(1, entry[2])
            chunk = entry[3]
            key = entry[8:24].split(b"\0")[0].decode(errors="replace")
            payload = entry[24:32]
            if ns == 0:
                namespaces[payload[0]] = key
            elif kind in (0x01, 0x11):
                values[(ns, key)] = struct.unpack("<b" if kind == 0x11 else "<B", payload[:1])[0]
            elif kind in (0x02, 0x12):
                values[(ns, key)] = struct.unpack("<h" if kind == 0x12 else "<H", payload[:2])[0]
            elif kind in (0x04, 0x14):
                values[(ns, key)] = struct.unpack("<i" if kind == 0x14 else "<I", payload[:4])[0]
            elif kind in (0x08, 0x18):
                values[(ns, key)] = struct.unpack("<q" if kind == 0x18 else "<Q", payload[:8])[0]
            elif kind in (0x21, 0x41, 0x42):
                size = struct.unpack("<H", payload[:2])[0]
                raw = page[64 + (index + 1) * ENTRY:64 + (index + 1) * ENTRY + size]
                if kind == 0x21:
                    values[(ns, key)] = raw.split(b"\0")[0].decode(errors="replace")
                elif kind == 0x41:
                    values[(ns, key)] = raw
                else:
                    blobs.setdefault((ns, key), {})[chunk] = raw
            index += span
        for (ns, key), chunks in blobs.items():
            values[(ns, key)] = b"".join(chunks[c] for c in sorted(chunks))
    return {(namespaces.get(ns, str(ns)), key): value for (ns, key), value in values.items()}


def read_from_device(port: str, offset: int, size: int) -> bytes:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "nvs.bin")
        for tool in ("esptool", "esptool.py"):
            try:
                subprocess.run([tool, "--port", port, "read-flash", hex(offset), hex(size), path],
                               check=True, stdout=subprocess.DEVNULL)
                break
            except FileNotFoundError:
                continue
        else:
            sys.exit("esptool not found, install it or read the flash yourself and use --dump")
        with open(path, "rb") as handle:
            return handle.read()


def yaml_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port of the ESP32 running Nuki Hub")
    source.add_argument("--dump", help="flash dump (whole flash or the NVS partition)")
    parser.add_argument("--out", required=True, help="secrets file to write")
    parser.add_argument("--device", choices=("opener", "lock"), help="which pairing to take if both exist")
    parser.add_argument("--nvs-offset", type=lambda v: int(v, 0), default=NVS_OFFSET_DEFAULT)
    parser.add_argument("--nvs-size", type=lambda v: int(v, 0), default=NVS_SIZE_DEFAULT)
    args = parser.parse_args()

    if args.port:
        nvs = read_from_device(args.port, args.nvs_offset, args.nvs_size)
    else:
        with open(args.dump, "rb") as handle:
            dump = handle.read()
        located = find_nvs(dump) if len(dump) > 0x9000 else None
        nvs = dump[located[0]:located[0] + located[1]] if located else dump

    values = parse_nvs(nvs)
    pairings = {}
    for (namespace, key), value in values.items():
        if key == "secretKeyK" and isinstance(value, bytes) and len(value) == 32 and any(value):
            pairings["opener" if namespace.endswith("opener") else "lock"] = namespace
    if not pairings:
        sys.exit("No Nuki pairing found in the settings")
    device = args.device or ("opener" if "opener" in pairings else "lock")
    if device not in pairings:
        sys.exit(f"No pairing with a Nuki {device} found, there is one for: {', '.join(pairings)}")
    namespace = pairings[device]

    def setting(key, default=None):
        return values.get(("nukihub", key), default)

    address = values.get((namespace, "bleAddress"), b"")
    auth_id = values.get((namespace, "authorizationId"), b"")
    pin = values.get((namespace, "securityPinCode"), b"")
    if len(address) != 6 or len(auth_id) != 4:
        sys.exit("The pairing is incomplete")

    lines = [
        f"# Taken from Nuki Hub ({device}). Keep this file private.",
        f"nuki_device: {device}",
        "nuki_ble_address: " + yaml_string(":".join(f"{b:02X}" for b in address)),
        "nuki_secret_key: " + yaml_string(values[(namespace, "secretKeyK")].hex()),
        "nuki_auth_id: " + yaml_string(auth_id.hex()),
        f"nuki_pin: {int.from_bytes(pin[:2], 'little') if pin else 0}",
    ]
    found = ["pairing"]
    for name, key in (("wifi_ssid", "wifiSSID"), ("wifi_password", "wifiPass"), ("mqtt_broker", "mqttbroker"),
                      ("mqtt_username", "mqttuser"), ("mqtt_password", "mqttpass"), ("mqtt_path", "mqttpath")):
        value = setting(key)
        if isinstance(value, str) and value:
            lines.append(f"{name}: {yaml_string(value)}")
            found.append(name)
    port = setting("mqttport")
    if isinstance(port, int) and port:
        lines.append(f"mqtt_port: {port}")

    descriptor = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"Nuki {device} pairing written to {args.out} ({', '.join(found)})")


if __name__ == "__main__":
    main()
