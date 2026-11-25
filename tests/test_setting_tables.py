"""The YAML keys of the switch, number and select platforms against the settings the
devices have: every key must name a setting of the right kind, with the same limits."""
import ast
import re
import unittest
from pathlib import Path

COMPONENT = Path(__file__).resolve().parent.parent / "components" / "nuki_hub"


def python_table(file, name):
    source = (COMPONENT / file).read_text()
    return ast.literal_eval(re.search(name + r" = (\{.*?\n\})", source, re.S).group(1))


def device_settings(file):
    """key -> ("switch" | "select" | (min, max)) from discovery_settings() of a device"""
    source = (COMPONENT / file).read_text()
    body = source[source.index("::discovery_settings() const {") :]
    body = body[: body.index("\n}\n")]
    settings = {}
    for key, low, high, options in re.findall(r'\{"(\w+)", "[^"]*", (?:true|false), (-?\d+), (-?\d+)(, \w+\(\w*\))?\}', body):
        if options:
            settings[key] = "select"
        elif low == high:
            settings[key] = "switch"
        else:
            settings[key] = (int(low), int(high))
    return settings


class SettingTables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lock = device_settings("nuki_hub_lock.cpp")
        cls.opener = device_settings("nuki_hub_opener.cpp")

    def kinds(self, key):
        return [table[key] for table in (self.lock, self.opener) if key in table]

    def test_tables_were_read(self):
        self.assertGreater(len(self.lock), 30)
        self.assertGreater(len(self.opener), 15)

    def test_switches(self):
        for name, key in python_table("switch.py", "SETTINGS").items():
            self.assertTrue(self.kinds(key), f"{name}: no device has {key}")
            self.assertEqual(set(self.kinds(key)), {"switch"}, name)

    def test_selects(self):
        for name, key in python_table("select.py", "SETTINGS").items():
            self.assertTrue(self.kinds(key), f"{name}: no device has {key}")
            self.assertEqual(set(self.kinds(key)), {"select"}, name)

    def test_numbers(self):
        for name, (key, low, high) in python_table("number.py", "SETTINGS").items():
            self.assertTrue(self.kinds(key), f"{name}: no device has {key}")
            self.assertEqual(set(self.kinds(key)), {(low, high)}, name)

    def test_every_lock_setting_has_an_entity(self):
        named = set(python_table("switch.py", "SETTINGS").values())
        named |= set(python_table("select.py", "SETTINGS").values())
        named |= {key for key, _, _ in python_table("number.py", "SETTINGS").values()}
        self.assertEqual(set(self.lock) - named, set())
        self.assertEqual(set(self.opener) - named, set())


if __name__ == "__main__":
    unittest.main()
