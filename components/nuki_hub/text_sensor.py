"""Text sensors of a nuki_hub device for the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_NUKI_HUB_ID, NukiHubComponent

DEPENDENCIES = ["nuki_hub"]

CONF_STATE = "state"
CONF_LAST_USER = "last_user"
CONF_TOPICS = "topics"
CONF_TOPIC = "topic"


def _topic(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or "#" in value or "+" in value:
        raise cv.Invalid("A topic below the device, as in the documentation, starting with '/': /trigger")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        # The state as Nuki Hub names it: locked, RTOactive, open, ...
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_USER): text_sensor.text_sensor_schema(icon="mdi:account"),
        # Anything the device publishes as a text sensor: /trigger, /doorSensorState, ...
        cv.Optional(CONF_TOPICS): cv.ensure_list(
            text_sensor.text_sensor_schema().extend({cv.Required(CONF_TOPIC): _topic})
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key in (CONF_STATE, CONF_LAST_USER):
        if conf := config.get(key):
            var = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_text_sensor")(var))
    for conf in config.get(CONF_TOPICS, []):
        var = await text_sensor.new_text_sensor(conf)
        cg.add(parent.add_topic_text_sensor(conf[CONF_TOPIC], var))
