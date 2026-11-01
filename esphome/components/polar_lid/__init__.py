"""Petlibro Polar PLAF109 lid ("door") controller.

Stock behaviour (analysis/static/README.md 2b, emulation 5.4): H-bridge on AW9523B P1_7/P1_6
(10 close, 01 open, 11 brake), lid position sensor on GPIO19, lid motor current on GPIO3
(ADC1 ch3, calibrated mV, doorStuckCurrent 400 mV for 3 s), IR pet sensor on GPIO20 aborts a close.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, switch, text_sensor, voltage_sampler
import esphome.config_validation as cv
from esphome.const import CONF_ID, DEVICE_CLASS_PROBLEM, ENTITY_CATEGORY_DIAGNOSTIC, UNIT_VOLT

AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "voltage_sampler"]

CONF_CLOSE_PIN = "close_pin"
CONF_OPEN_PIN = "open_pin"
CONF_LID_SENSOR = "lid_sensor"
CONF_PET_SENSOR = "pet_sensor"
CONF_CURRENT_SENSOR = "current_sensor"
CONF_STALL_THRESHOLD = "stall_threshold"
CONF_STALL_SAMPLES = "stall_samples"
CONF_STALL_RETRIES = "stall_retries"
CONF_MAX_MOVE_TIME = "max_move_time"
CONF_CHECK_SIGNAL_TIME = "check_signal_time"
CONF_NOCHECK_SIGNAL_TIME = "nocheck_signal_time"
CONF_RUN_ON = "run_on"
CONF_SENSOR_END_LEVEL = "sensor_end_level"
CONF_CLOSE_STALL_SAMPLES = "close_stall_samples"
CONF_CLOSE_SAMPLE_INTERVAL = "close_sample_interval"
CONF_INRUSH_BLANKING = "inrush_blanking"
CONF_MIN_TRAVEL = "min_travel"
CONF_POWER_SWITCHES = "power_switches"
CONF_STATE_TEXT = "state_text"
CONF_FAULT = "fault"
CONF_CLOSED = "closed"
CONF_MOTOR_CURRENT = "motor_current"
CONF_ON_OPENED = "on_opened"
CONF_ON_CLOSED = "on_closed"
CONF_ON_PET_REOPEN = "on_pet_reopen"
CONF_ON_FAULT = "on_fault"
CONF_PROFILE = "profile"
CONF_CHECK_PET = "check_pet"
CONF_OPEN = "open"
CONF_DURATION = "duration"

polar_lid_ns = cg.esphome_ns.namespace("polar_lid")
PolarLid = polar_lid_ns.class_("PolarLid", cg.Component)
LidProfile = polar_lid_ns.enum("LidProfile", is_class=True)
PROFILES = {"feed": LidProfile.FEED, "manual": LidProfile.MANUAL, "boot": LidProfile.BOOT}
OpenAction = polar_lid_ns.class_("OpenAction", automation.Action)
CloseAction = polar_lid_ns.class_("CloseAction", automation.Action)
StopAction = polar_lid_ns.class_("StopAction", automation.Action)
ClearFaultAction = polar_lid_ns.class_("ClearFaultAction", automation.Action)
JogAction = polar_lid_ns.class_("JogAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PolarLid),
        cv.Required(CONF_CLOSE_PIN): pins.gpio_output_pin_schema,  # P1_7
        cv.Required(CONF_OPEN_PIN): pins.gpio_output_pin_schema,  # P1_6
        cv.Optional(CONF_LID_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_PET_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_CURRENT_SENSOR): cv.use_id(voltage_sampler.VoltageSampler),
        # doorStuckCurrent 400 (new_dev_attr +0), compared with calibrated mV from Read_DoorMotorAdc @0x4200d402
        cv.Optional(CONF_STALL_THRESHOLD, default=0.400): cv.positive_float,
        cv.Optional(CONF_STALL_SAMPLES, default=6): cv.int_range(min=1, max=60),
        cv.Optional(CONF_STALL_RETRIES, default=3): cv.int_range(min=0, max=5),
        cv.Optional(CONF_MAX_MOVE_TIME, default="60s"): cv.positive_time_period_milliseconds,
        # doorCheckSignalTime 5 s, doorNotcheckSignalTime 10 s (dev_attr +0xb / +0xc, emulation log)
        cv.Optional(CONF_CHECK_SIGNAL_TIME, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_NOCHECK_SIGNAL_TIME, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RUN_ON, default="1000ms"): cv.positive_time_period_milliseconds,
        # Level of lid_sensor that means "at an end stop". Stock code implies high (open question 8);
        # determined by test plan step 4.
        cv.Optional(CONF_SENSOR_END_LEVEL, default=True): cv.boolean,
        # OURS anti-pinch: closing trips after this many samples at this interval, after inrush blanking
        cv.Optional(CONF_CLOSE_STALL_SAMPLES, default=2): cv.int_range(min=1, max=20),
        cv.Optional(CONF_CLOSE_SAMPLE_INTERVAL, default="100ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_INRUSH_BLANKING, default="300ms"): cv.positive_time_period_milliseconds,
        # OURS: the sensor is not accepted before this much travel time
        cv.Optional(CONF_MIN_TRAVEL, default="1000ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_POWER_SWITCHES, default=[]): cv.ensure_list(cv.use_id(switch.Switch)),
        cv.Optional(CONF_STATE_TEXT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_CLOSED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_MOTOR_CURRENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=3,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ON_OPENED): automation.validate_automation({}),
        cv.Optional(CONF_ON_CLOSED): automation.validate_automation({}),
        cv.Optional(CONF_ON_PET_REOPEN): automation.validate_automation({}),
        cv.Optional(CONF_ON_FAULT): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    c = await cg.gpio_pin_expression(config[CONF_CLOSE_PIN])
    o = await cg.gpio_pin_expression(config[CONF_OPEN_PIN])
    cg.add(var.set_motor_pins(c, o))
    if CONF_LID_SENSOR in config:
        cg.add(var.set_lid_sensor(await cg.get_variable(config[CONF_LID_SENSOR])))
    if CONF_PET_SENSOR in config:
        cg.add(var.set_pet_sensor(await cg.get_variable(config[CONF_PET_SENSOR])))
    if CONF_CURRENT_SENSOR in config:
        cg.add(var.set_current_sampler(await cg.get_variable(config[CONF_CURRENT_SENSOR])))
    cg.add(var.set_stall_threshold(config[CONF_STALL_THRESHOLD]))
    cg.add(var.set_stall_samples(config[CONF_STALL_SAMPLES]))
    cg.add(var.set_stall_retries(config[CONF_STALL_RETRIES]))
    cg.add(var.set_max_move_time(config[CONF_MAX_MOVE_TIME]))
    cg.add(var.set_feed_windows(config[CONF_CHECK_SIGNAL_TIME], config[CONF_NOCHECK_SIGNAL_TIME]))
    cg.add(var.set_run_on(config[CONF_RUN_ON]))
    cg.add(var.set_sensor_end_level(config[CONF_SENSOR_END_LEVEL]))
    cg.add(var.set_close_stall_samples(config[CONF_CLOSE_STALL_SAMPLES]))
    cg.add(var.set_close_sample_interval(config[CONF_CLOSE_SAMPLE_INTERVAL]))
    cg.add(var.set_inrush_blanking(config[CONF_INRUSH_BLANKING]))
    cg.add(var.set_min_travel(config[CONF_MIN_TRAVEL]))
    for s in config[CONF_POWER_SWITCHES]:
        cg.add(var.add_power_switch(await cg.get_variable(s)))
    if CONF_STATE_TEXT in config:
        cg.add(var.set_state_text_sensor(await text_sensor.new_text_sensor(config[CONF_STATE_TEXT])))
    if CONF_FAULT in config:
        cg.add(var.set_fault_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_FAULT])))
    if CONF_CLOSED in config:
        cg.add(var.set_closed_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_CLOSED])))
    if CONF_MOTOR_CURRENT in config:
        cg.add(var.set_current_sensor(await sensor.new_sensor(config[CONF_MOTOR_CURRENT])))
    for key, getter in (
        (CONF_ON_OPENED, "get_opened_trigger"),
        (CONF_ON_CLOSED, "get_closed_trigger"),
        (CONF_ON_PET_REOPEN, "get_pet_reopen_trigger"),
    ):
        for conf in config.get(key, []):
            await automation.build_automation(getattr(var, getter)(), [], conf)
    for conf in config.get(CONF_ON_FAULT, []):
        await automation.build_automation(var.get_fault_trigger(), [(cg.std_string, "reason")], conf)


LID_SIMPLE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(PolarLid)})


async def _simple_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


for _name, _cls in (("polar_lid.stop", StopAction), ("polar_lid.clear_fault", ClearFaultAction)):
    automation.register_action(_name, _cls, LID_SIMPLE_SCHEMA, synchronous=True)(_simple_action)


@automation.register_action(
    "polar_lid.open",
    OpenAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarLid),
            cv.Optional(CONF_PROFILE, default="manual"): cv.enum(PROFILES, lower=True),
        }
    ),
    synchronous=True,
)
async def open_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_profile(config[CONF_PROFILE]))
    return var


@automation.register_action(
    "polar_lid.close",
    CloseAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarLid),
            cv.Optional(CONF_PROFILE, default="manual"): cv.enum(PROFILES, lower=True),
            cv.Optional(CONF_CHECK_PET, default=False): cv.boolean,
        }
    ),
    synchronous=True,
)
async def close_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_profile(config[CONF_PROFILE]))
    cg.add(var.set_check_pet(config[CONF_CHECK_PET]))
    return var


@automation.register_action(
    "polar_lid.jog",
    JogAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarLid),
            cv.Required(CONF_OPEN): cv.templatable(cv.boolean),
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
    cg.add(var.set_open(await cg.templatable(config[CONF_OPEN], args, bool)))
    cg.add(var.set_duration(await cg.templatable(config[CONF_DURATION], args, cg.uint32)))
    return var
