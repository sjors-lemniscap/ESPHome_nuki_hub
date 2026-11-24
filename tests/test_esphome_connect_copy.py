"""NukiBle::connect() repeats the body of ESPHome's BLEClientBase::connect() to open the
connection with its own scan parameters. When ESPHome changes that function, this test fails:
compare the two and bring NukiBle::connect() in nuki_ble.cpp in line, then record the new hash.
"""
import hashlib
import re
import unittest
from pathlib import Path

# BLEClientBase::connect() of ESPHome 2026.11.0
KNOWN = {"881bdd87bf8966fd"}


def upstream_connect():
    try:
        import esphome
    except ImportError:
        return None
    source = Path(esphome.__file__).parent / "components" / "esp32_ble_client" / "ble_client_base.cpp"
    text = source.read_text()
    match = re.search(r"void BLEClientBase::connect\(\) \{\n(.*?)\n\}\n", text, re.S)
    return match.group(1) if match else ""


class ConnectCopyTest(unittest.TestCase):
    def test_upstream_connect_is_the_one_copied(self):
        body = upstream_connect()
        if body is None:
            self.skipTest("esphome is not installed in this interpreter")
        digest = hashlib.sha256(re.sub(r"\s+", " ", body).encode()).hexdigest()[:16]
        self.assertIn(digest, KNOWN, "BLEClientBase::connect() changed in this ESPHome version: " + digest)


if __name__ == "__main__":
    unittest.main()
