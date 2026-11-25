"""Buttons of a nuki_hub device for the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import DEVICE_CLASS_RESTART, ENTITY_CATEGORY_CONFIG

from . import CONF_NUKI_HUB_ID, NukiHubComponent, nuki_hub_ns

DEPENDENCIES = ["nuki_hub"]

NukiHubButton = nuki_hub_ns.class_("NukiHubButton", button.Button)

CONF_UNPAIR = "unpair"

# YAML key: (command topic, payload)
COMMANDS = {
    # Smart Lock: finds the end positions again, the lock turns while it does
    "request_calibration": ("/configuration/action", '{"recalibrateNuki":"1"}'),
    # Restarts the Nuki device, not the ESP32
    "reboot": ("/configuration/action", '{"rebootNuki":"1"}'),
    "query_state": ("/query/lockstate", "1"),
    "query_config": ("/query/config", "1"),
    "query_battery": ("/query/battery", "1"),
    "query_keypad": ("/query/keypad", "1"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        # Removes the pairing from the ESP32
        cv.Optional(CONF_UNPAIR): button.button_schema(
            NukiHubButton, entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:link-off"
        ),
        **{
            cv.Optional(key): button.button_schema(
                NukiHubButton,
                entity_category=ENTITY_CATEGORY_CONFIG,
                **({"device_class": DEVICE_CLASS_RESTART} if key == "reboot" else {}),
            )
            for key in COMMANDS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    if conf := config.get(CONF_UNPAIR):
        var = await button.new_button(conf)
        cg.add(var.set_parent(parent))
    for key, (topic, payload) in COMMANDS.items():
        if conf := config.get(key):
            var = await button.new_button(conf)
            cg.add(var.set_parent(parent))
            cg.add(var.set_command(topic, payload))
