"""Sensors of a nuki_hub device for the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
)

from . import CONF_NUKI_HUB_ID, NukiHubComponent

DEPENDENCIES = ["nuki_hub"]

CONF_RSSI = "rssi"
CONF_KEYPAD_ENTRIES = "keypad_entries"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        cv.Optional(CONF_RSSI): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ),
        cv.Optional(CONF_KEYPAD_ENTRIES): sensor.sensor_schema(
            icon="mdi:dialpad", entity_category=ENTITY_CATEGORY_DIAGNOSTIC, accuracy_decimals=0
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key in (CONF_RSSI, CONF_KEYPAD_ENTRIES):
        if conf := config.get(key):
            var = await sensor.new_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_sensor")(var))
