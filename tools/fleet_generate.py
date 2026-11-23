#!/usr/bin/env python3
"""Writes one ESPHome file per door from the rooms file of the booking agent.

    tools/fleet_generate.py rooms.json out/ [--board esp32-s3-devkitc-1] [--timezone Europe/Berlin]
                                             [--domain .local] [--lock door ...]

For every door in the rooms file (the `mqtt_name` of each room and the `global` doors) it
writes `<out>/<door>.yaml`, which includes `room.yaml` (copied next to it from
examples/fleet), and adds what is missing to `<out>/secrets.yaml`: an API encryption key
per door, made here, and placeholders for the pairing values of tools/nuki_hub_pairing.py.
`<out>/agent.env` gets the two lines the agent needs to reach the doors over the ESPHome
API. Files that exist are left as they are, so a second run only adds new doors.
Keys are written to files (mode 0600) and never printed.
"""
import argparse
import base64
import json
import os
import re
import secrets
import shutil
import sys

DOOR = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def doors_of(rooms_file):
    with open(rooms_file, encoding="utf-8") as handle:
        data = json.load(handle)
    names = [room.get("mqtt_name", "") for room in data.get("rooms", [])] + list(data.get("global", []))
    doors = []
    for name in names:
        if not name:
            continue
        if not DOOR.match(name):
            raise SystemExit(f"'{name}' is not a usable door name (letters, digits, _ . -)")
        if name not in doors:
            doors.append(name)
    return doors


def host_name(door):
    """ESPHome device names take letters, digits and hyphens."""
    return "nuki-" + re.sub(r"[^a-z0-9]+", "-", door.lower()).strip("-")


def secret_name(door):
    return re.sub(r"[^a-z0-9]+", "_", door.lower()).strip("_")


def read_secrets(path):
    if not os.path.exists(path):
        return {}
    found = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^([A-Za-z0-9_]+):\s*(.*)$", line)
            if match:
                found[match.group(1)] = match.group(2).strip().strip('"')
    return found


def write_private(path, text, mode="w"):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | (os.O_APPEND if mode == "a" else os.O_TRUNC), 0o600)
    with os.fdopen(descriptor, mode, encoding="utf-8") as handle:
        handle.write(text)
    os.chmod(path, 0o600)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("rooms")
    parser.add_argument("out")
    parser.add_argument("--board", default="esp32-s3-devkitc-1")
    parser.add_argument("--timezone", default="Europe/Berlin")
    parser.add_argument("--domain", default=".local", help="what follows the device name in its host name")
    parser.add_argument("--lock", nargs="*", default=[], help="doors with a Smart Lock instead of an Opener")
    args = parser.parse_args(argv)

    doors = doors_of(args.rooms)
    if not doors:
        raise SystemExit("no doors in the rooms file")
    unknown = [door for door in args.lock if door not in doors]
    if unknown:
        raise SystemExit(f"--lock names doors that are not in the rooms file: {', '.join(unknown)}")
    os.makedirs(args.out, exist_ok=True)

    package = os.path.join(args.out, "room.yaml")
    if not os.path.exists(package):
        shutil.copyfile(os.path.join(os.path.dirname(__file__), "..", "examples", "fleet", "room.yaml"), package)

    secrets_path = os.path.join(args.out, "secrets.yaml")
    known = read_secrets(secrets_path)
    added = []
    # What all rooms share: every secret room.yaml asks for
    with open(package, encoding="utf-8") as handle:
        shared = sorted(set(re.findall(r"!secret\s+([A-Za-z0-9_]+)", handle.read())))
    for name in shared:
        if name not in known:
            known[name] = "..."
            added.append(f'{name}: "..."\n')
    written = []
    for door in doors:
        secret = secret_name(door)
        if f"{secret}_api_key" not in known:
            key = base64.b64encode(secrets.token_bytes(32)).decode()
            known[f"{secret}_api_key"] = key
            added.append(f'{secret}_api_key: "{key}"\n')
        for name, placeholder in ((f"{secret}_nuki_ble_address", '"00:00:00:00:00:00"'), (f"{secret}_nuki_auth_id", "0"),
                                  (f"{secret}_nuki_pin", "0")):
            if name not in known:
                added.append(f"{name}: {placeholder}  # from tools/nuki_hub_pairing.py\n")
        path = os.path.join(args.out, f"{door}.yaml")
        if os.path.exists(path):
            continue
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(
                f"# {door}: made by tools/fleet_generate.py, yours to change\n"
                "substitutions:\n"
                f"  room: {door}\n"
                f"  host: {host_name(door)}\n"
                f"  device: {'lock' if door in args.lock else 'opener'}\n"
                f"  board: {args.board}\n"
                f"  timezone: {args.timezone}\n"
                f"  pin: !secret {secret}_nuki_pin\n"
                f"  ble_address: !secret {secret}_nuki_ble_address\n"
                f"  auth_id: !secret {secret}_nuki_auth_id\n"
                f"  api_key: !secret {secret}_api_key\n"
                "\n"
                "packages:\n"
                "  room: !include room.yaml\n"
            )
        written.append(door)
    if added:
        write_private(secrets_path, "".join(added), "a")

    hosts = {door: host_name(door) + args.domain for door in doors}
    keys = {door: known[f"{secret_name(door)}_api_key"] for door in doors}
    write_private(
        os.path.join(args.out, "agent.env"),
        "# For the .env of the booking agent, to reach the doors over the ESPHome API\n"
        'NUKI_TRANSPORT="esphome"\n'
        f"NUKI_ESPHOME_HOSTS='{json.dumps(hosts)}'\n"
        f"NUKI_ESPHOME_KEYS='{json.dumps(keys)}'\n",
    )
    print(f"{len(doors)} doors, {len(written)} new files in {args.out}: {', '.join(written) if written else 'none'}")
    print(f"secrets.yaml: {len(added)} entries added; agent.env written (both mode 0600, keys not shown)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
