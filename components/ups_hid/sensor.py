import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_HERTZ,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
)

from . import CONF_UPS_HID_ID, UpsHid

DEPENDENCIES = ["ups_hid"]

SENSORS = {
    # Battery charge, percent of full
    "battery_charge": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        device_class=DEVICE_CLASS_BATTERY,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
    ),
    # Time the battery can keep things running at the current load
    "battery_runtime_remaining": sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
    ),
    "battery_voltage": sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    # Voltage coming from the wall outlet into the UPS
    "wall_voltage": sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    # Voltage the UPS supplies to plugged-in equipment
    "output_voltage": sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    "wall_frequency": sensor.sensor_schema(
        unit_of_measurement=UNIT_HERTZ,
        device_class=DEVICE_CLASS_FREQUENCY,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    # How much of the UPS's capacity the plugged-in equipment is using
    "load_percent": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
        icon="mdi:gauge",
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHid),
        **{cv.Optional(key): schema for key, schema in SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_UPS_HID_ID])
    for key in SENSORS:
        if conf := config.get(key):
            sens = await sensor.new_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_sensor")(sens))
