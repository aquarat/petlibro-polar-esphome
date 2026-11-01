"""Petlibro Polar PLAF109 tray (plate) controller.

Re-implements the stock AF109 2.0.38 tray logic for hardware revision 5.2.0
(analysis/static/README.md section 2, analysis/emulation/README.md section 5.3):
forward-only DC motor on an AW9523B H-bridge, one optical-gate mark per stock compartment,
homing by the widest mark, current-based stall recovery and a no-edge timeout.

compartments: 3 is the stock tray. 6, 9 and 12 are EXPERIMENTAL (1, 2, 3 or 4 positions per mark segment): positions between marks are
reached by running a fraction of the learned mark-to-mark time after the preceding mark.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor, esp32, sensor, switch, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_POSITION,
    CONF_TARGET,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RUNNING,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_MILLISECOND,
)

AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]
DEPENDENCIES = ["esp32"]
CODEOWNERS = []

CONF_MOTOR_PIN_A = "motor_pin_a"
CONF_MOTOR_PIN_B = "motor_pin_b"
CONF_GATE_PIN = "gate_pin"
CONF_COMPARTMENTS = "compartments"
CONF_CURRENT_PIN = "current_pin"
CONF_CURRENT_SENSING = "current_sensing"
CONF_STALL_THRESHOLD = "stall_threshold"
CONF_STALL_SAMPLES = "stall_samples"
CONF_STALL_RETRIES = "stall_retries"
CONF_EDGE_TIMEOUT = "edge_timeout"
CONF_MAX_MOVE_TIME = "max_move_time"
CONF_GATE_IGNORE = "gate_ignore"
CONF_HOME_MAX_EDGES = "home_max_edges"
CONF_HOME_WIDTH_RATIO = "home_width_ratio"
CONF_GATE_DEBOUNCE = "gate_debounce"
CONF_MAX_INVALID_SAMPLES = "max_invalid_samples"
CONF_ADC_INVALID = "adc_invalid"
CONF_POWER_SWITCHES = "power_switches"
CONF_MARK_INTERVAL = "mark_interval"
CONF_PULSE_WIDTH = "pulse_width"
CONF_MOTOR_CURRENT = "motor_current"
CONF_STATE_TEXT = "state_text"
CONF_HOMING_STATE = "homing_state"
CONF_FAULT = "fault"
CONF_MOVING = "moving"
CONF_HOMED = "homed"
CONF_ON_ARRIVED = "on_arrived"
CONF_ON_FAULT = "on_fault"
CONF_COMPARTMENT = "compartment"
CONF_FORWARD = "forward"
CONF_DURATION = "duration"

polar_tray_ns = cg.esphome_ns.namespace("polar_tray")
PolarTray = polar_tray_ns.class_("PolarTray", cg.Component)
HomeAction = polar_tray_ns.class_("HomeAction", automation.Action)
StepAction = polar_tray_ns.class_("StepAction", automation.Action)
StopAction = polar_tray_ns.class_("StopAction", automation.Action)
ClearFaultAction = polar_tray_ns.class_("ClearFaultAction", automation.Action)
GoToAction = polar_tray_ns.class_("GoToAction", automation.Action)
JogAction = polar_tray_ns.class_("JogAction", automation.Action)

# ESP32-C3: GPIO5 is the only ADC2 pin (ADC2 channel 0). Stock Read_PlateMotorAdc @0x4200d37e reads ADC2_CHAN0.
C3_ADC2_PINS = {5: 0}


def _validate_current_pin(value):
    value = pins.internal_gpio_pin_number(value)
    if value not in C3_ADC2_PINS:
        raise cv.Invalid("Tray current must be an ESP32-C3 ADC2 pin (GPIO5)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PolarTray),
        cv.Required(CONF_MOTOR_PIN_A): pins.gpio_output_pin_schema,  # P0_5
        cv.Required(CONF_MOTOR_PIN_B): pins.gpio_output_pin_schema,  # P0_6
        cv.Required(CONF_GATE_PIN): pins.internal_gpio_input_pin_schema,  # GPIO10, pull-up
        cv.Optional(CONF_COMPARTMENTS, default=3): cv.one_of(3, 6, 9, 12, int=True),
        cv.Optional(CONF_CURRENT_PIN, default=5): _validate_current_pin,
        cv.Optional(CONF_CURRENT_SENSING, default=True): cv.boolean,
        # plateStuckCurrent default 400 (NVS new_dev_attr +2, DEV_ATTR_newAttrInit @0x4200bf94), raw 12-bit counts
        cv.Optional(CONF_STALL_THRESHOLD, default=400): cv.int_range(min=1, max=4095),
        # 6 loops x 500 ms = 3 s (Feeding_Monitor @0x4200b8dc)
        cv.Optional(CONF_STALL_SAMPLES, default=6): cv.int_range(min=1, max=60),
        # retries < 3 (FEED_PLAN_plateHandle @0x4200b2c8)
        cv.Optional(CONF_STALL_RETRIES, default=3): cv.int_range(min=0, max=5),
        # plateErrTimeout default 60 s (new_dev_attr +8)
        cv.Optional(
            CONF_EDGE_TIMEOUT, default="60s"
        ): cv.positive_time_period_milliseconds,
        # OURS: hard cap on any single move including recoveries
        cv.Optional(
            CONF_MAX_MOVE_TIME, default="90s"
        ): cv.positive_time_period_milliseconds,
        # Close_SW_Interrupt + MicroSw_Debounce timer: 500 ms
        cv.Optional(
            CONF_GATE_IGNORE, default="500ms"
        ): cv.positive_time_period_milliseconds,
        # OURS: give up homing after this many counted edges without finding the home mark
        cv.Optional(CONF_HOME_MAX_EDGES, default=7): cv.int_range(min=4, max=20),
        # OURS: the widest mark must be >= this x the runner-up (NVS log ~510 vs ~160 ms)
        cv.Optional(CONF_HOME_WIDTH_RATIO, default=2.0): cv.float_range(min=1.0, max=10.0),
        # OURS: a gate level must hold this long to count (level-stable filter)
        cv.Optional(
            CONF_GATE_DEBOUNCE, default="10ms"
        ): cv.positive_time_period_milliseconds,
        # OURS: fault after this many consecutive invalid ADC2 samples while moving (0 = never)
        cv.Optional(CONF_MAX_INVALID_SAMPLES, default=20): cv.int_range(min=0, max=200),
        cv.Optional(CONF_POWER_SWITCHES, default=[]): cv.ensure_list(
            cv.use_id(switch.Switch)
        ),
        cv.Optional(CONF_POSITION): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:rotate-right"
        ),
        cv.Optional(CONF_TARGET): sensor.sensor_schema(
            accuracy_decimals=0, icon="mdi:target"
        ),
        cv.Optional(CONF_MARK_INTERVAL): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PULSE_WIDTH): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MOTOR_CURRENT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:current-dc",
        ),
        cv.Optional(CONF_ADC_INVALID): sensor.sensor_schema(
            unit_of_measurement="%",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_STATE_TEXT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_HOMING_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM
        ),
        cv.Optional(CONF_MOVING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING
        ),
        cv.Optional(CONF_HOMED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_ON_ARRIVED): automation.validate_automation({}),
        cv.Optional(CONF_ON_FAULT): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    a = await cg.gpio_pin_expression(config[CONF_MOTOR_PIN_A])
    b = await cg.gpio_pin_expression(config[CONF_MOTOR_PIN_B])
    cg.add(var.set_motor_pins(a, b))
    cg.add(var.set_gate_pin(await cg.gpio_pin_expression(config[CONF_GATE_PIN])))
    cg.add(var.set_compartments(config[CONF_COMPARTMENTS]))
    cg.add(
        var.set_current_sensing(
            config[CONF_CURRENT_SENSING], C3_ADC2_PINS[config[CONF_CURRENT_PIN]]
        )
    )
    if config[CONF_CURRENT_SENSING]:
        # IDF 5.5 adc_oneshot_new_unit() rejects ADC2 on the ESP32-C3 unless this is set
        # (components/esp_adc/adc_oneshot.c, Kconfig "ADC2 Digital Controller is not stable").
        esp32.add_idf_sdkconfig_option("CONFIG_ADC_ONESHOT_FORCE_USE_ADC2_ON_C3", True)
    cg.add(var.set_stall_threshold(config[CONF_STALL_THRESHOLD]))
    cg.add(var.set_stall_samples(config[CONF_STALL_SAMPLES]))
    cg.add(var.set_stall_retries(config[CONF_STALL_RETRIES]))
    cg.add(var.set_edge_timeout(config[CONF_EDGE_TIMEOUT]))
    cg.add(var.set_max_move_time(config[CONF_MAX_MOVE_TIME]))
    cg.add(var.set_gate_ignore(config[CONF_GATE_IGNORE]))
    cg.add(var.set_home_max_edges(config[CONF_HOME_MAX_EDGES]))
    cg.add(var.set_home_width_ratio(config[CONF_HOME_WIDTH_RATIO]))
    cg.add(var.set_gate_debounce(config[CONF_GATE_DEBOUNCE]))
    cg.add(var.set_max_invalid_samples(config[CONF_MAX_INVALID_SAMPLES]))
    for s in config[CONF_POWER_SWITCHES]:
        cg.add(var.add_power_switch(await cg.get_variable(s)))

    for key, setter in (
        (CONF_POSITION, "set_position_sensor"),
        (CONF_TARGET, "set_target_sensor"),
        (CONF_MARK_INTERVAL, "set_mark_interval_sensor"),
        (CONF_PULSE_WIDTH, "set_pulse_width_sensor"),
        (CONF_MOTOR_CURRENT, "set_current_sensor"),
        (CONF_ADC_INVALID, "set_adc_invalid_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await sensor.new_sensor(config[key])))
    for key, setter in (
        (CONF_STATE_TEXT, "set_state_text_sensor"),
        (CONF_HOMING_STATE, "set_homing_text_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))
    for key, setter in (
        (CONF_FAULT, "set_fault_binary_sensor"),
        (CONF_MOVING, "set_moving_binary_sensor"),
        (CONF_HOMED, "set_homed_binary_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await binary_sensor.new_binary_sensor(config[key])))

    for conf in config.get(CONF_ON_ARRIVED, []):
        await automation.build_automation(var.get_arrived_trigger(), [], conf)
    for conf in config.get(CONF_ON_FAULT, []):
        await automation.build_automation(
            var.get_fault_trigger(), [(cg.std_string, "reason")], conf
        )


TRAY_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(PolarTray)})


async def _simple_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


for _name, _cls in (
    ("polar_tray.home", HomeAction),
    ("polar_tray.step", StepAction),
    ("polar_tray.stop", StopAction),
    ("polar_tray.clear_fault", ClearFaultAction),
):
    automation.register_action(_name, _cls, TRAY_ACTION_SCHEMA, synchronous=True)(
        _simple_action
    )


@automation.register_action(
    "polar_tray.go_to",
    GoToAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarTray),
            cv.Required(CONF_COMPARTMENT): cv.templatable(cv.int_range(min=1, max=12)),  # checked at runtime
        }
    ),
    synchronous=True,
)
async def go_to_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_compartment(await cg.templatable(config[CONF_COMPARTMENT], args, cg.int_)))
    return var


@automation.register_action(
    "polar_tray.jog",
    JogAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarTray),
            cv.Optional(CONF_FORWARD, default=True): cv.templatable(cv.boolean),
            cv.Optional(CONF_DURATION, default="500ms"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def jog_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_forward(await cg.templatable(config[CONF_FORWARD], args, bool)))
    cg.add(var.set_duration(await cg.templatable(config[CONF_DURATION], args, cg.uint32)))
    return var
