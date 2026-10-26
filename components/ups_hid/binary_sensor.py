import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_UPS_HID_ID, UpsHid

DEPENDENCIES = ["ups_hid"]

BINARY_SENSORS = {
    # The UPS is plugged into the ESP32's USB port and recognised
    "ups_connected": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    # Wall power is out and the UPS is running from its battery
    "on_battery": binary_sensor.binary_sensor_schema(
        icon="mdi:power-plug-off",
    ),
    "charging": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_BATTERY_CHARGING,
    ),
    # The UPS says the battery is nearly empty; this is when computers should shut down
    "low_battery": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_BATTERY,
    ),
    "battery_needs_replacement": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
    # More equipment is plugged in than the UPS can supply
    "overloaded": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHid),
        **{cv.Optional(key): schema for key, schema in BINARY_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_UPS_HID_ID])
    for key in BINARY_SENSORS:
        if conf := config.get(key):
            sens = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_binary_sensor")(sens))
