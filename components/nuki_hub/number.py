"""The numeric settings of a nuki_hub device as numbers of the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_NUKI_HUB_ID, NukiHubComponent, nuki_hub_ns

DEPENDENCIES = ["nuki_hub"]

NukiHubSettingNumber = nuki_hub_ns.class_("NukiHubSettingNumber", number.Number, cg.Component)

# YAML key: (key of the configuration topics, smallest value, largest value). The limits are
# those the device type accepts, see discovery_settings() of NukiHubLock and NukiHubOpener.
SETTINGS = {
    # Smart Lock
    "led_brightness": ("ledBrightness", 0, 5),
    "timezone_offset": ("timeZoneOffset", 0, 60),
    "lock_n_go_timeout": ("lockNgoTimeout", 5, 60),
    "auto_lock_timeout": ("autoLockTimeOut", 30, 1800),
    "unlatch_duration": ("unlatchDuration", 1, 30),
    "unlocked_position_offset": ("unlockedPositionOffsetDegrees", -90, 180),
    "locked_position_offset": ("lockedPositionOffsetDegrees", -180, 90),
    "single_locked_position_offset": ("singleLockedPositionOffsetDegrees", -180, 180),
    "unlocked_to_locked_transition_offset": ("unlockedToLockedTransitionOffsetDegrees", -180, 180),
    # Opener
    "sound_level": ("soundLevel", 0, 255),
    "rto_timeout": ("rtoTimeout", 5, 60),
    "electric_strike_delay": ("electricStrikeDelay", 0, 30000),
    "electric_strike_duration": ("electricStrikeDuration", 1000, 30000),
    "doorbell_suppression_duration": ("doorbellSuppressionDuration", 500, 10000),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        **{
            cv.Optional(key): number.number_schema(
                NukiHubSettingNumber, entity_category=ENTITY_CATEGORY_CONFIG
            ).extend(cv.COMPONENT_SCHEMA)
            for key in SETTINGS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    for key, (setting, min_value, max_value) in SETTINGS.items():
        if conf := config.get(key):
            var = await number.new_number(conf, min_value=min_value, max_value=max_value, step=1)
            await cg.register_component(var, conf)
            cg.add(var.set_parent(parent))
            cg.add(var.set_key(setting))
