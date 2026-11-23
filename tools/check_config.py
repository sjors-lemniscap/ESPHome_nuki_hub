#!/usr/bin/env python3
"""Looks over an ESPHome file that uses nuki_hub for what is easy to get wrong.

    tools/check_config.py room.yaml [more.yaml ...]

It reads the YAML as text with its includes and substitutions left as they are, so it needs
no ESPHome and no secrets, and says what it could not see. Exit code 1 if anything is an
error, 0 with only notes.
"""
import re
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None


class _Loader(yaml.SafeLoader if yaml else object):
    pass


def _tagged(loader, tag_suffix, node):
    if isinstance(node, yaml.ScalarNode):
        return f"!{tag_suffix} {loader.construct_scalar(node)}"
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node)
    return loader.construct_mapping(node)


if yaml:
    _Loader.add_multi_constructor("!", _tagged)


def check(config):
    """Returns (errors, notes) for one parsed configuration."""
    errors, notes = [], []
    hubs = config.get("nuki_hub")
    if hubs is None:
        return ["no nuki_hub: section"], notes
    if isinstance(hubs, dict):
        hubs = [hubs]
    mqtt = config.get("mqtt")
    api = config.get("api")
    if isinstance(api, str) or api is None and "api" in config:
        api = {}

    if mqtt is None and not any(hub.get("api_transport") for hub in hubs):
        notes.append("neither mqtt: nor api_transport: only the entities and automations of the ESP32 itself are there")
    if isinstance(mqtt, dict):
        if not mqtt.get("idf_send_async"):
            errors.append("mqtt: set idf_send_async: true, a stalled broker connection can otherwise hold the main loop "
                          "until the watchdog restarts the ESP32")
        if "log_topic" not in mqtt:
            notes.append("mqtt: without log_topic: null ESPHome publishes its whole log below <topic_prefix>/debug")
        if mqtt.get("discovery", True) is not False and api is not None:
            notes.append("mqtt: discovery: false avoids announcing every entity over MQTT as well as over the API")
        birth = (mqtt.get("birth_message") or {}).get("topic") if isinstance(mqtt.get("birth_message"), dict) else None
        will = (mqtt.get("will_message") or {}).get("topic") if isinstance(mqtt.get("will_message"), dict) else None
        if birth != will:
            errors.append("mqtt: birth_message and will_message need the same topic, Home Assistant entities stay "
                          "available while the ESP32 is gone otherwise")
        if not mqtt.get("username"):
            notes.append("mqtt: no username, the keypad topics carry door codes")
    prefixes = []
    for index, hub in enumerate(hubs):
        where = f"nuki_hub[{index}]"
        prefix = hub.get("topic_prefix")
        if prefix in prefixes:
            errors.append(f"{where}: topic_prefix '{prefix}' is used twice")
        prefixes.append(prefix)
        if hub.get("api_transport"):
            if not isinstance(api, dict):
                errors.append(f"{where}: api_transport needs an api: section")
            else:
                for flag in ("custom_services", "homeassistant_services"):
                    if not api.get(flag):
                        errors.append(f"{where}: api_transport needs {flag}: true in api:")
                if not (isinstance(api.get("encryption"), dict) and api["encryption"].get("key")):
                    errors.append(f"{where}: api_transport without api: encryption: key: sends door codes in clear")
        if "security_pin" not in hub:
            notes.append(f"{where}: no security_pin, keypad, authorizations, time control and the log stay unavailable")
        if hub.get("keypad_delete_expired") is not None and not (hub.get("time_id") and hub.get("update_time")):
            errors.append(f"{where}: keypad_delete_expired needs time_id and update_time: true")
        if hub.get("update_time") and not hub.get("time_id"):
            errors.append(f"{where}: update_time needs time_id")
        if hub.get("keypad_publish_code") and isinstance(mqtt, dict) and not mqtt.get("username"):
            errors.append(f"{where}: keypad_publish_code on a broker without a login publishes the codes to anyone")
        if hub.get("authorization_add"):
            notes.append(f"{where}: authorization_add lets any client of the topics create permanent access")
        if hub.get("homeassistant_discovery") and mqtt is None:
            errors.append(f"{where}: homeassistant_discovery needs mqtt:")
        if hub.get("pairing_mode_at_boot") and hub.get("pairing"):
            notes.append(f"{where}: pairing_mode_at_boot next to a stored pairing is not needed")
    if "psram" not in config:
        notes.append("no psram: with more than about 40 keypad entries the list is published in pages (keypad/page/<n>)")
    if "packages" in config or any(isinstance(v, str) and v.startswith("!include") for v in config.values()):
        notes.append("includes and packages are not followed: check those files as well")
    return errors, notes


def main(argv=None):
    paths = (argv if argv is not None else sys.argv[1:])
    if not paths:
        print(__doc__.strip().split("\n\n")[0])
        return 2
    if yaml is None:
        print("needs PyYAML (pip install pyyaml)")
        return 2
    failed = False
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        # "<<: !include" merges a file this tool does not read
        text = re.sub(r"^<<:\s*!include.*$", "", text, flags=re.MULTILINE)
        config = yaml.load(text, Loader=_Loader) or {}
        errors, notes = check(config)
        print(f"{path}: {len(errors)} errors, {len(notes)} notes")
        for line in errors:
            print(f"  error: {line}")
        for line in notes:
            print(f"  note:  {line}")
        failed = failed or bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
