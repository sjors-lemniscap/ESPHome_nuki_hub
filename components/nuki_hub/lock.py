"""A lock entity for a nuki_hub device, for Home Assistant through the ESPHome API."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import lock

from . import NukiHubComponent, nuki_hub_ns

DEPENDENCIES = ["nuki_hub"]

CONF_NUKI_HUB_ID = "nuki_hub_id"

NukiHubLockEntity = nuki_hub_ns.class_("NukiHubLockEntity", lock.Lock, cg.Component)

CONFIG_SCHEMA = (
    lock.lock_schema(NukiHubLockEntity)
    .extend({cv.GenerateID(CONF_NUKI_HUB_ID): cv.use_id(NukiHubComponent)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await lock.new_lock(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_NUKI_HUB_ID])
    cg.add(var.set_parent(parent))
