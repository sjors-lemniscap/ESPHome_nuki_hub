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
        # Opener only: on for two seconds when someone rings
        cv.Optional(CONF_RING): binary_sensor.binary_sensor_schema(icon="mdi:bell-ring"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key in (CONF_AVAILABLE, CONF_BATTERY_CRITICAL, CONF_KEYPAD_BATTERY_CRITICAL, CONF_RING):
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(var))
