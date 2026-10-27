import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_UPS_HID_ID, UpsHid

DEPENDENCIES = ["ups_hid"]

TEXT_SENSORS = {
    # Plain English, e.g. "On battery, discharging"
    "status": text_sensor.text_sensor_schema(icon="mdi:information-outline"),
    # Raw NUT status codes, e.g. "OB DISCHRG"
    "status_code": text_sensor.text_sensor_schema(
        icon="mdi:code-tags", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    "manufacturer": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    "model": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    "serial_number": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    # Result of the last battery self-test, e.g. "Done and passed"
    "self_test_result": text_sensor.text_sensor_schema(
        icon="mdi:battery-check", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        # Optional name for this block, so packages can be customised with !extend / !remove
        cv.Optional(CONF_ID): cv.string_strict,
        cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHid),
        **{cv.Optional(key): schema for key, schema in TEXT_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_UPS_HID_ID])
    for key in TEXT_SENSORS:
        if conf := config.get(key):
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_text_sensor")(sens))
