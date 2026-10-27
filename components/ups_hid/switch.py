import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

from . import CONF_UPS_HID_ID, UpsHid, require_allow_control, ups_hid_ns

DEPENDENCIES = ["ups_hid"]

UpsHidBeeperSwitch = ups_hid_ns.class_(
    "UpsHidBeeperSwitch", switch.Switch, cg.Parented.template(UpsHid)
)

CONF_BEEPER = "beeper"

CONFIG_SCHEMA = cv.Schema(
    {
        # Optional name for this block, so packages can be customised with !extend / !remove
        cv.Optional(CONF_ID): cv.string_strict,
        cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHid),
        # On: the UPS beeps on power problems. Off: it stays silent.
        cv.Optional(CONF_BEEPER): switch.switch_schema(
            UpsHidBeeperSwitch,
            icon="mdi:volume-high",
            entity_category=ENTITY_CATEGORY_CONFIG,
            # The UPS remembers the setting itself; restoring from flash would resend stale commands
            default_restore_mode="DISABLED",
        ),
    }
)

FINAL_VALIDATE_SCHEMA = require_allow_control


async def to_code(config):
    hub = await cg.get_variable(config[CONF_UPS_HID_ID])
    if conf := config.get(CONF_BEEPER):
        sw = await switch.new_switch(conf)
        await cg.register_parented(sw, hub)
        cg.add(hub.set_beeper_switch(sw))
