"""Petlibro Polar PLAF109 Peltier thermostat (stock CTRL_THREAD_coolHanlde @0x4200e6f6 plus user extensions).

UNTESTED ON HARDWARE: the fan wiring is unknown, so `allowed` defaults to false and the runtime enable
switch must be off at boot (restore_mode: ALWAYS_OFF).
"""

import esphome.codegen as cg
from esphome.components import binary_sensor, number, sensor, switch, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INTERVAL,
    CONF_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_MINUTE,
    UNIT_PERCENT,
)
from esphome.components.polar_feeder import PolarFeeder

AUTO_LOAD = ["sensor", "text_sensor"]
DEPENDENCIES = ["switch", "number"]

CONF_PELTIER = "peltier"
CONF_ON_BATTERY = "on_battery"
CONF_ENABLE = "enable"
CONF_THAW_RULE = "thaw_rule"
CONF_FEEDER = "feeder"
CONF_SETPOINT = "setpoint"
CONF_HYSTERESIS = "hysteresis"
CONF_MAX_ON = "max_on_minutes"
CONF_ALLOWED = "allowed"
CONF_MIN_OFF_TIME = "min_off_time"
CONF_FREEZE_GUARD = "freeze_guard"
CONF_DUTY = "duty"
CONF_SINCE_OFF = "since_off"
CONF_STATE_TEXT = "state_text"
CONF_POWER_SWITCHES = "power_switches"
CONF_ASSERT_POWER_SWITCHES = "assert_power_switches"

polar_cooling_ns = cg.esphome_ns.namespace("polar_cooling")
PolarCooling = polar_cooling_ns.class_("PolarCooling", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PolarCooling),
        cv.Required(CONF_PELTIER): cv.use_id(switch.Switch),  # AW9523B P1_4 ("GPIO_PTC_ON")
        cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Required(CONF_ENABLE): cv.use_id(switch.Switch),
        cv.Optional(CONF_ON_BATTERY): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_THAW_RULE): cv.use_id(switch.Switch),
        cv.Optional(CONF_FEEDER): cv.use_id(PolarFeeder),
        cv.Optional(CONF_SETPOINT): cv.use_id(number.Number),
        cv.Optional(CONF_HYSTERESIS): cv.use_id(number.Number),
        cv.Optional(CONF_MAX_ON): cv.use_id(number.Number),
        cv.Optional(CONF_ALLOWED, default=False): cv.boolean,
        # stock: 600 loops x 500 ms = 300 s
        cv.Optional(CONF_INTERVAL, default="5min"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MIN_OFF_TIME, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FREEZE_GUARD, default=1.0): cv.float_,
        cv.Optional(CONF_DUTY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:percent",
        ),
        cv.Optional(CONF_SINCE_OFF): sensor.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_STATE_TEXT): text_sensor.text_sensor_schema(),
        # Switched on before the Peltier (stock DC state of P0_4/P0_7)
        cv.Optional(CONF_POWER_SWITCHES, default=[]): cv.ensure_list(cv.use_id(switch.Switch)),
        # false only for the fan-wiring test (P1_4 alone)
        cv.Optional(CONF_ASSERT_POWER_SWITCHES, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_peltier(await cg.get_variable(config[CONF_PELTIER])))
    cg.add(var.set_temperature(await cg.get_variable(config[CONF_TEMPERATURE])))
    cg.add(var.set_enable(await cg.get_variable(config[CONF_ENABLE])))
    for key, setter in (
        (CONF_ON_BATTERY, "set_on_battery"),
        (CONF_THAW_RULE, "set_thaw_rule"),
        (CONF_FEEDER, "set_feeder"),
        (CONF_SETPOINT, "set_setpoint"),
        (CONF_HYSTERESIS, "set_hysteresis"),
        (CONF_MAX_ON, "set_max_on"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[key])))
    cg.add(var.set_allowed(config[CONF_ALLOWED]))
    cg.add(var.set_interval(config[CONF_INTERVAL]))
    cg.add(var.set_min_off(config[CONF_MIN_OFF_TIME]))
    cg.add(var.set_freeze_guard(config[CONF_FREEZE_GUARD]))
    if CONF_DUTY in config:
        cg.add(var.set_duty_sensor(await sensor.new_sensor(config[CONF_DUTY])))
    if CONF_SINCE_OFF in config:
        cg.add(var.set_since_off_sensor(await sensor.new_sensor(config[CONF_SINCE_OFF])))
    cg.add(var.set_assert_power_switches(config[CONF_ASSERT_POWER_SWITCHES]))
    for sw in config[CONF_POWER_SWITCHES]:
        cg.add(var.add_power_switch(await cg.get_variable(sw)))
    if CONF_STATE_TEXT in config:
        cg.add(var.set_state_text(await text_sensor.new_text_sensor(config[CONF_STATE_TEXT])))
