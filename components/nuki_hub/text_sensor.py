"""Text sensors of a nuki_hub device for the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_NUKI_HUB_ID, NukiHubComponent

DEPENDENCIES = ["nuki_hub"]

CONF_STATE = "state"
CONF_LAST_USER = "last_user"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        # The state as Nuki Hub names it: locked, RTOactive, open, ...
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_USER): text_sensor.text_sensor_schema(icon="mdi:account"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key in (CONF_STATE, CONF_LAST_USER):
        if conf := config.get(key):
            var = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_text_sensor")(var))
