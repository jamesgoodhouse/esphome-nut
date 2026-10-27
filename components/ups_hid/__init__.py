"""Read a USB UPS (HID Power Device class) from an ESP32 with a USB host port."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.ups_common import UpsSource
from esphome.components.usb_host import USBClient
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@jamesgoodhouse"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["usb_host", "ups_common"]
MULTI_CONF = False

CONF_UPS_HID_ID = "ups_hid_id"
CONF_ON_STATUS_CHANGE = "on_status_change"
CONF_ALLOW_CONTROL = "allow_control"

ups_hid_ns = cg.esphome_ns.namespace("ups_hid")
UpsHid = ups_hid_ns.class_("UpsHid", USBClient, UpsSource)

# Chips with a USB OTG host controller (the same list ESPHome's usb_host supports)
USB_HOST_VARIANTS = [
    getattr(esp32, name)
    for name in (
        "VARIANT_ESP32S2",
        "VARIANT_ESP32S3",
        "VARIANT_ESP32P4",
        "VARIANT_ESP32H4",
        "VARIANT_ESP32S31",
    )
    if hasattr(esp32, name)
]

UNSUPPORTED_CHIP_HELP = (
    "ups_hid needs a chip with USB host support (ESP32-S2, ESP32-S3 or ESP32-P4) "
    "to talk to a USB UPS."
)


def _require_usb_host_chip(config):
    if not CORE.is_esp32:
        raise cv.Invalid(
            f"{UNSUPPORTED_CHIP_HELP} This device is not an ESP32 at all ({CORE.target_platform})."
        )
    variant = esp32.get_esp32_variant()
    if variant not in USB_HOST_VARIANTS:
        board = esp32.get_board()
        raise cv.Invalid(
            f"{UNSUPPORTED_CHIP_HELP} This board ('{board}') uses the {variant} chip, "
            "which has no USB host hardware. Use an ESP32-S3 board such as "
            "'esp32-s3-devkitc-1' instead (and set `board:` to match)."
        )
    return config


def control_allowed() -> bool:
    """Whether the (single) ups_hid block has allow_control turned on. For final validation."""
    ups = fv.full_config.get().get("ups_hid") or {}
    return bool(ups.get(CONF_ALLOW_CONTROL, False))


def require_allow_control(config):
    """Final validation for entities that change the UPS."""
    if not control_allowed():
        raise cv.Invalid(
            "These UPS controls change settings on the UPS, so they must be turned on explicitly: "
            "add `allow_control: true` under `ups_hid:`."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(UpsHid),
            # How often to read the UPS. Status changes pushed by the UPS arrive immediately.
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="15s"
            ): cv.positive_time_period_milliseconds,
            # Runs with `status` set to the NUT status code ("OL CHRG", "OB DISCHRG", ...)
            # whenever it changes, and with an empty string when no UPS is connected.
            cv.Optional(CONF_ON_STATUS_CHANGE): automation.validate_automation(),
            # Let Home Assistant and NUT clients change UPS settings and run commands
            # (beeper, battery self-test). Off by default: the UPS is only read.
            cv.Optional(CONF_ALLOW_CONTROL, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _require_usb_host_chip,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_allow_control(config[CONF_ALLOW_CONTROL]))
    for conf in config.get(CONF_ON_STATUS_CHANGE, []):
        await automation.build_automation(
            var.get_status_change_trigger(), [(cg.std_string, "status")], conf
        )
