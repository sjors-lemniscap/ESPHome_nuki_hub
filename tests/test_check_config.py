"""tools/check_config.py: the mistakes it has to name."""
import contextlib
import importlib.util
import io
import os
import unittest

_SPEC = importlib.util.spec_from_file_location(
    "check_config", os.path.join(os.path.dirname(__file__), "..", "tools", "check_config.py")
)
check_config = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check_config)

GOOD = {
    "psram": {},
    "mqtt": {
        "broker": "b", "username": "u", "idf_send_async": True, "log_topic": None, "discovery": False,
        "birth_message": {"topic": "room/maintenance/mqttConnectionState"},
        "will_message": {"topic": "room/maintenance/mqttConnectionState"},
    },
    "api": {"encryption": {"key": "k"}, "custom_services": True, "homeassistant_services": True},
    "nuki_hub": [{"type": "opener", "topic_prefix": "room", "security_pin": 1, "api_transport": True}],
}


def changed(**sections):
    config = {key: (dict(value) if isinstance(value, dict) else value) for key, value in GOOD.items()}
    config.update(sections)
    return config


@unittest.skipIf(check_config.yaml is None, "PyYAML not installed")
class CheckConfigTest(unittest.TestCase):
    def errors(self, config):
        return " | ".join(check_config.check(config)[0])

    def test_a_complete_configuration_has_no_errors(self):
        self.assertEqual(check_config.check(GOOD), ([], []))

    def test_mqtt_mistakes(self):
        mqtt = dict(GOOD["mqtt"], idf_send_async=False)
        self.assertIn("idf_send_async", self.errors(changed(mqtt=mqtt)))
        mqtt = dict(GOOD["mqtt"], will_message={"topic": "other"})
        self.assertIn("same topic", self.errors(changed(mqtt=mqtt)))
        mqtt = {k: v for k, v in GOOD["mqtt"].items() if k != "log_topic"}
        self.assertTrue(any("log_topic" in note for note in check_config.check(changed(mqtt=mqtt))[1]))

    def test_api_transport_needs_its_flags_and_a_key(self):
        self.assertIn("custom_services", self.errors(changed(api={"encryption": {"key": "k"}})))
        api = dict(GOOD["api"])
        del api["encryption"]
        self.assertIn("in clear", self.errors(changed(api=api)))
        config = changed()
        del config["api"]
        self.assertIn("needs an api: section", self.errors(config))

    def test_device_options(self):
        hub = dict(GOOD["nuki_hub"][0], keypad_delete_expired={})
        self.assertIn("keypad_delete_expired", self.errors(changed(nuki_hub=[hub])))
        twice = [GOOD["nuki_hub"][0], GOOD["nuki_hub"][0]]
        self.assertIn("used twice", self.errors(changed(nuki_hub=twice)))
        self.assertEqual(check_config.check({})[0], ["no nuki_hub: section"])

    def test_the_examples_pass(self):
        root = os.path.join(os.path.dirname(__file__), "..", "examples")
        for name in ("nuki_hub_opener.yaml", os.path.join("fleet", "room.yaml")):
            with self.subTest(name=name), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check_config.main([os.path.join(root, name)]), 0)


if __name__ == "__main__":
    unittest.main()
