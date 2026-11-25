"""The on/off settings of a nuki_hub device, and its pairing mode, as switches of the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_NUKI_HUB_ID, NukiHubComponent, nuki_hub_ns

DEPENDENCIES = ["nuki_hub"]

NukiHubSettingSwitch = nuki_hub_ns.class_("NukiHubSettingSwitch", switch.Switch, cg.Component)
NukiHubPairingModeSwitch = nuki_hub_ns.class_("NukiHubPairingModeSwitch", switch.Switch, cg.Component)

CONF_PAIRING_MODE = "pairing_mode"

# YAML key: key of the configuration topics. The Smart Lock keys are those of ESPHome Nuki Lock.
SETTINGS = {
    # Smart Lock
    "bluetooth_pairing_enabled": "pairingEnabled",
    "auto_unlatch": "autoUnlatch",
    "led_enabled": "ledEnabled",
    "night_mode_enabled": "nightModeEnabled",
    "night_mode_auto_lock_enabled": "nightModeAutoLockEnabled",
    "night_mode_auto_unlock_disabled": "nightModeAutoUnlockDisabled",
    "night_mode_immediate_lock_on_start_enabled": "nightModeImmediateLockOnStart",
    "auto_lock_enabled": "autoLockEnabled",
    "auto_unlock_disabled": "autoUnLockDisabled",
    "immediate_auto_lock_enabled": "immediateAutoLockEnabled",
    "auto_update_enabled": "autoUpdateEnabled",
    "single_lock_enabled": "singleLock",
    "dst_mode_enabled": "dstMode",
    "auto_battery_type_detection_enabled": "automaticBatteryTypeDetection",
    "slow_speed_during_night_mode_enabled": "enableSlowSpeedDuringNightMode",
    "detached_cylinder_enabled": "detachedCylinder",
    # Opener
    "led_flash_enabled": "ledFlashEnabled",
    "sound_confirmation": "soundConfirmation",
    "disable_rto_after_ring": "disableRtoAfterRing",
    "random_electric_strike_delay": "randomElectricStrikeDelay",
    # Both
    "button_enabled": "buttonEnabled",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent),
        cv.Optional(CONF_PAIRING_MODE): switch.switch_schema(
            NukiHubPairingModeSwitch, entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:bluetooth-connect"
        ).extend(cv.COMPONENT_SCHEMA),
        **{
            cv.Optional(key): switch.switch_schema(
                NukiHubSettingSwitch, entity_category=ENTITY_CATEGORY_CONFIG
            ).extend(cv.COMPONENT_SCHEMA)
            for key in SETTINGS
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    if conf := config.get(CONF_PAIRING_MODE):
        var = await switch.new_switch(conf)
        await cg.register_component(var, conf)
        cg.add(var.set_parent(parent))
    for key, setting in SETTINGS.items():
        if conf := config.get(key):
            var = await switch.new_switch(conf)
            await cg.register_component(var, conf)
            cg.add(var.set_parent(parent))
            cg.add(var.set_key(setting))
