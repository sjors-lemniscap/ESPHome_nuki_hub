"""Binary sensors of a nuki_hub device for the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_BATTERY, DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_NUKI_HUB_ID, NukiHubComponent

DEPENDENCIES = ["nuki_hub"]

CONF_AVAILABLE = "available"
CONF_BATTERY_CRITICAL = "battery_critical"
CONF_KEYPAD_BATTERY_CRITICAL = "keypad_battery_critical"
CONF_RING = "ring"
CONF_PAIRED = "paired"
CONF_KEY = "key"
CONF_TOPICS = "topics"
CONF_TOPIC = "topic"
CONF_PAYLOAD_ON = "payload_on"


def _topic(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or "#" in value or "+" in value:
        raise cv.Invalid("A topic below the device, as in the documentation, starting with '/': /trigger")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        cv.Optional(CONF_AVAILABLE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_BATTERY_CRITICAL): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_KEYPAD_BATTERY_CRITICAL): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_PAIRED): binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        # Opener only: on for two seconds when someone rings
        cv.Optional(CONF_RING): binary_sensor.binary_sensor_schema(icon="mdi:bell-ring"),
        # On while a topic has one payload: /doorSensorState with payload_on doorOpened, ...
        cv.Optional(CONF_TOPICS): cv.ensure_list(
            binary_sensor.binary_sensor_schema().extend(
                {
                    cv.Required(CONF_TOPIC): _topic,
                    # One value of a JSON topic: /battery/basicJson and charging
                    cv.Optional(CONF_KEY): cv.string_strict,
                    cv.Optional(CONF_PAYLOAD_ON, default="1"): cv.string_strict,
                }
            )
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key in (CONF_AVAILABLE, CONF_BATTERY_CRITICAL, CONF_KEYPAD_BATTERY_CRITICAL, CONF_RING, CONF_PAIRED):
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(var))
    for conf in config.get(CONF_TOPICS, []):
        var = await binary_sensor.new_binary_sensor(conf)
        if CONF_KEY in conf:
            cg.add(parent.add_json_binary_sensor(conf[CONF_TOPIC], conf[CONF_KEY], conf[CONF_PAYLOAD_ON], var))
        else:
            cg.add(parent.add_topic_binary_sensor(conf[CONF_TOPIC], conf[CONF_PAYLOAD_ON], var))
