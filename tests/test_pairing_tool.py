"""The pairing tool against a settings partition built here, laid out like ESP-IDF's NVS."""

import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "nuki_hub_pairing.py"

spec = importlib.util.spec_from_file_location("nuki_hub_pairing", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def entry(ns, kind, span, key, payload, chunk=0xFF):
    return bytes([ns, kind, span, chunk]) + b"\xff" * 4 + key.encode().ljust(16, b"\0") + payload.ljust(8, b"\xff")


def variable(ns, kind, key, data, chunk=0xFF):
    """A string or blob: a header entry followed by the data in whole entries."""
    padded = data + b"\xff" * (-len(data) % 32)
    header = entry(ns, kind, 1 + len(padded) // 32, key, struct.pack("<HH", len(data), 0xFFFF) + b"\xff" * 4, chunk)
    return header + padded


def page(entries: bytes, sequence=0):
    count = len(entries) // 32
    bitmap = bytearray(b"\xff" * 32)
    for i in range(count):  # 0b10 = written
        bitmap[i // 4] &= ~(1 << ((i % 4) * 2)) & 0xFF
    header = struct.pack("<II", 0xFFFFFFFE, sequence) + b"\xfe" + b"\xff" * 19 + b"\xff" * 4
    return (header + bytes(bitmap) + entries).ljust(4096, b"\xff")


KEY = bytes(range(32))
ADDRESS = bytes.fromhex("54D272000001")
AUTH = bytes.fromhex("04030201")


def partition(namespace="NukiHubopener"):
    entries = b"".join([
        entry(0, 0x01, 1, "nukihub", b"\x01"),
        entry(0, 0x01, 1, namespace, b"\x02"),
        variable(1, 0x21, "wifiSSID", b"My Network\0"),
        variable(1, 0x21, "wifiPass", b"wifi \"secret\"\0"),
        variable(1, 0x21, "mqttbroker", b"192.168.1.10\0"),
        variable(1, 0x21, "mqttpath", b"entrance\0"),
        entry(1, 0x14, 1, "mqttport", struct.pack("<i", 1883)),
        variable(2, 0x42, "bleAddress", ADDRESS, chunk=0),
        variable(2, 0x42, "secretKeyK", KEY, chunk=0),
        variable(2, 0x42, "authorizationId", AUTH, chunk=0),
        variable(2, 0x42, "securityPinCode", struct.pack("<H", 4711), chunk=0),
    ])
    return page(entries) + b"\xff" * 4096


class PairingToolTests(unittest.TestCase):
    def test_parse(self):
        values = tool.parse_nvs(partition())
        self.assertEqual(values[("NukiHubopener", "secretKeyK")], KEY)
        self.assertEqual(values[("NukiHubopener", "bleAddress")], ADDRESS)
        self.assertEqual(values[("nukihub", "wifiSSID")], "My Network")
        self.assertEqual(values[("nukihub", "mqttport")], 1883)

    def test_newer_page_wins(self):
        old = page(entry(0, 0x01, 1, "nukihub", b"\x01") + entry(1, 0x14, 1, "mqttport", struct.pack("<i", 1)), 0)
        new = page(entry(1, 0x14, 1, "mqttport", struct.pack("<i", 8883)), 1)
        self.assertEqual(tool.parse_nvs(new + old)[("nukihub", "mqttport")], 8883)

    def run_tool(self, data, *extra):
        with tempfile.TemporaryDirectory() as directory:
            dump = os.path.join(directory, "nvs.bin")
            out = os.path.join(directory, "secrets.yaml")
            with open(dump, "wb") as handle:
                handle.write(data)
            result = subprocess.run([sys.executable, str(TOOL), "--dump", dump, "--out", out, *extra],
                                    capture_output=True, text=True)
            text = Path(out).read_text() if os.path.exists(out) else ""
            mode = os.stat(out).st_mode & 0o777 if os.path.exists(out) else None
            return result, text, mode

    def test_secrets_file(self):
        result, text, mode = self.run_tool(partition())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(mode, 0o600)
        self.assertIn('nuki_ble_address: "54:D2:72:00:00:01"', text)
        self.assertIn(f'nuki_secret_key: "{KEY.hex()}"', text)
        self.assertIn('nuki_auth_id: "04030201"', text)
        self.assertIn("nuki_pin: 4711", text)
        self.assertIn('wifi_password: "wifi \\"secret\\""', text)
        self.assertIn("mqtt_port: 1883", text)
        # What was found is named, the values are not shown
        self.assertNotIn(KEY.hex(), result.stdout)
        self.assertNotIn("secret", result.stdout.replace("secrets.yaml", ""))

    def test_full_flash_dump(self):
        table = (b"\xaa\x50\x01\x02" + struct.pack("<II", 0x9000, 0x2000) + b"nvs".ljust(16, b"\0") + b"\0" * 4)
        flash = (b"\xff" * 0x8000 + table.ljust(0x1000, b"\xff") + partition()).ljust(0x10000, b"\xff")
        result, text, _ = self.run_tool(flash)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nuki_device: opener", text)

    def test_lock_namespace(self):
        result, text, _ = self.run_tool(partition("NukiHub"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nuki_device: lock", text)

    def test_no_pairing(self):
        result, text, _ = self.run_tool(page(entry(0, 0x01, 1, "nukihub", b"\x01")))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(text, "")


if __name__ == "__main__":
    unittest.main()
