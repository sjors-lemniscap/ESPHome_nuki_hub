"""tools/fleet_generate.py: one file per door, keys made once and never printed."""
import contextlib
import importlib.util
import io
import json
import os
import stat
import tempfile
import unittest

_SPEC = importlib.util.spec_from_file_location(
    "fleet_generate", os.path.join(os.path.dirname(__file__), "..", "tools", "fleet_generate.py")
)
fleet_generate = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(fleet_generate)


class FleetGenerateTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.rooms = os.path.join(self.dir.name, "rooms.json")
        self.out = os.path.join(self.dir.name, "out")
        self.write_rooms(["room_u1", ""], ["entrance", "room_u1"])

    def write_rooms(self, rooms, global_doors):
        with open(self.rooms, "w", encoding="utf-8") as handle:
            json.dump({"rooms": [{"code": str(i), "mqtt_name": name} for i, name in enumerate(rooms)],
                       "global": global_doors}, handle)

    def run_tool(self, *extra):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            fleet_generate.main([self.rooms, self.out, *extra])
        return output.getvalue()

    def read(self, name):
        with open(os.path.join(self.out, name), encoding="utf-8") as handle:
            return handle.read()

    def test_one_file_per_door_without_duplicates(self):
        self.run_tool("--lock", "entrance")
        self.assertEqual(sorted(os.listdir(self.out)),
                         ["agent.env", "entrance.yaml", "room.yaml", "room_u1.yaml", "secrets.yaml"])
        room = self.read("room_u1.yaml")
        self.assertIn("  room: room_u1\n", room)
        self.assertIn("  host: nuki-room-u1\n", room)
        self.assertIn("  device: opener\n", room)
        self.assertIn("  api_key: !secret room_u1_api_key\n", room)
        self.assertIn("  device: lock\n", self.read("entrance.yaml"))

    def test_keys_are_private_valid_and_not_printed(self):
        printed = self.run_tool()
        known = fleet_generate.read_secrets(os.path.join(self.out, "secrets.yaml"))
        key = known["room_u1_api_key"]
        self.assertEqual(len(__import__("base64").b64decode(key)), 32)
        self.assertNotEqual(key, known["entrance_api_key"])
        self.assertNotIn(key, printed)
        for name in ("secrets.yaml", "agent.env"):
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.out, name)).st_mode), 0o600)
        env = self.read("agent.env")
        hosts = json.loads(env.split("NUKI_ESPHOME_HOSTS='")[1].split("'")[0])
        keys = json.loads(env.split("NUKI_ESPHOME_KEYS='")[1].split("'")[0])
        self.assertEqual(hosts, {"room_u1": "nuki-room-u1.local", "entrance": "nuki-entrance.local"})
        self.assertEqual(keys["room_u1"], key)

    def test_second_run_keeps_what_is_there_and_adds_new_doors(self):
        self.run_tool()
        key = fleet_generate.read_secrets(os.path.join(self.out, "secrets.yaml"))["room_u1_api_key"]
        with open(os.path.join(self.out, "room_u1.yaml"), "a", encoding="utf-8") as handle:
            handle.write("# mine\n")
        self.write_rooms(["room_u1", "room_u2"], ["entrance"])
        printed = self.run_tool()
        self.assertIn("1 new files", printed)
        self.assertIn("# mine", self.read("room_u1.yaml"))
        known = fleet_generate.read_secrets(os.path.join(self.out, "secrets.yaml"))
        self.assertEqual(known["room_u1_api_key"], key)
        self.assertIn("room_u2_api_key", known)
        self.assertEqual(self.read("secrets.yaml").count("room_u1_api_key"), 1)

    def test_bad_names_are_refused(self):
        self.write_rooms(["room/+"], [])
        with self.assertRaises(SystemExit):
            self.run_tool()
        self.write_rooms(["room_u1"], [])
        with self.assertRaises(SystemExit):
            self.run_tool("--lock", "nowhere")


if __name__ == "__main__":
    unittest.main()
