import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

from . import CONF_UPS_HID_ID, UpsHid, require_allow_control, ups_hid_ns

DEPENDENCIES = ["ups_hid"]

UpsHidCommandButton = ups_hid_ns.class_(
    "UpsHidCommandButton", button.Button, cg.Parented.template(UpsHid)
)

# Button key -> (NUT instant command, icon)
BUTTONS = {
    # Silence an alarm that is already sounding; the UPS beeps again at the next event
    "mute_beeper": ("beeper.mute", "mdi:volume-off"),
    # A short battery test; the result shows in the self_test_result text sensor
    "battery_self_test": ("test.battery.start.quick", "mdi:battery-check"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        # Optional name for this block, so packages can be customised with !extend / !remove
        cv.Optional(CONF_ID): cv.string_strict,
        cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHid),
        **{
            cv.Optional(key): button.button_schema(
                UpsHidCommandButton, icon=icon, entity_category=ENTITY_CATEGORY_CONFIG
            )
            for key, (_, icon) in BUTTONS.items()
        },
    }
)

FINAL_VALIDATE_SCHEMA = require_allow_control


async def to_code(config):
    hub = await cg.get_variable(config[CONF_UPS_HID_ID])
    for key, (command, _) in BUTTONS.items():
        if conf := config.get(key):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, hub)
            cg.add(btn.set_command(command))
