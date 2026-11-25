"""The settings of a nuki_hub device that are a choice, as selects of the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_NUKI_HUB_ID, NukiHubComponent, nuki_hub_ns

DEPENDENCIES = ["nuki_hub"]

NukiHubSettingSelect = nuki_hub_ns.class_("NukiHubSettingSelect", select.Select, cg.Component)

# YAML key: key of the configuration topics. The choices are those of the device type, set
# when the ESP32 starts. "buton" is how ESPHome Nuki Lock spells these two, both are accepted.
SETTINGS = {
    "single_button_press_action": "singleButtonPressAction",
    "double_button_press_action": "doubleButtonPressAction",
    "single_buton_press_action": "singleButtonPressAction",
    "double_buton_press_action": "doubleButtonPressAction",
    "fob_action_1": "fobAction1",
    "fob_action_2": "fobAction2",
    "fob_action_3": "fobAction3",
    # Smart Lock
    "timezone": "timeZone",
    "advertising_mode": "advertisingMode",
    "battery_type": "batteryType",
    "motor_speed": "motorSpeed",
    # Opener
    "sound_ring": "soundRing",
    "sound_open": "soundOpen",
    "sound_rto": "soundRto",
    "sound_cm": "soundCm",
    "doorbell_suppression": "doorbellSuppression",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        **{
            cv.Optional(key): select.select_schema(
                NukiHubSettingSelect, entity_category=ENTITY_CATEGORY_CONFIG
            ).extend(cv.COMPONENT_SCHEMA)
            for key in SETTINGS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key, setting in SETTINGS.items():
        if conf := config.get(key):
            var = await select.new_select(conf, options=[])
            await cg.register_component(var, conf)
            cg.add(var.set_parent(parent))
            cg.add(var.set_key(setting))
