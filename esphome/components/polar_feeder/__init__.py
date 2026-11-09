"""Petlibro Polar PLAF109 meal scheduler and feed sequence (see polar_feeder.h for the run model).

Stock references: analysis/static/README.md sections 2b (feed sequence), 3 (thaw rule);
analysis/android-app/README.md section 4 (feed now / close now / plate semantics).
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    datetime,
    number,
    output,
    select,
    sensor,
    switch,
    text_sensor,
    time,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID, DEVICE_CLASS_PROBLEM

from esphome.components.polar_lid import PolarLid
from esphome.components.polar_tray import PolarTray

AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]
DEPENDENCIES = ["polar_tray", "polar_lid", "time", "datetime", "number", "switch", "select", "output"]

CONF_TRAY = "tray"
CONF_LID = "lid"
CONF_PET_SENSOR = "pet_sensor"
CONF_TEMPERATURE = "temperature"
CONF_IR_EMITTER = "ir_emitter"
CONF_MODE_SELECT = "mode_select"
CONF_OPEN_MINUTES = "open_minutes"
CONF_LEAD_MINUTES = "lead_minutes"
CONF_FILLED = "filled_compartments"
CONF_DEFAULT_OPEN_MINUTES = "default_open_minutes"
CONF_DEFAULT_LEAD_MINUTES = "default_lead_minutes"
CONF_MEALS = "meals"
CONF_TIME = "time"
CONF_ENABLED = "enabled"
CONF_THAW_THRESHOLD = "thaw_threshold"
CONF_THAW_LEAD = "thaw_lead"
CONF_PET_LEAVE_TIME = "pet_leave_time"
CONF_STALL_RETRY_DELAY = "stall_retry_delay"
CONF_MAX_CLOSE_STALLS = "max_close_stalls"
CONF_IR_LEVEL = "ir_level"
CONF_IR_SETTLE = "ir_settle"
CONF_BOOT_GRACE = "boot_grace"
CONF_POWER_SWITCHES = "power_switches"
CONF_STATE_TEXT = "state_text"
CONF_LAST_EVENT = "last_event"
CONF_NEXT_MEAL = "next_meal"
CONF_NEXT_COMPARTMENT = "next_compartment"
CONF_EMPTY = "empty"
CONF_FEEDING = "feeding"
CONF_THAW = "thaw"
CONF_ANTI_PINCH = "anti_pinch"
CONF_ON_PREPARE = "on_prepare"
CONF_ON_FEED_START = "on_feed_start"
CONF_ON_LID_OPENED = "on_lid_opened"
CONF_ON_FEED_END = "on_feed_end"
CONF_MINUTES = "minutes"
CONF_SERVE = "serve"
CONF_SERVE_ENABLED = "enabled"
CONF_SERVE_START = "start"
CONF_SERVE_INTERVAL = "interval"
CONF_SERVE_CHOICE = "compartment_choice"
CONF_SERVE_NO_SHOW = "no_show_minutes"
CONF_SERVE_PET_GONE = "pet_gone_minutes"
CONF_SERVE_MAX_OPEN = "max_open_minutes"
CONF_SERVE_REST = "rest_compartment"
CONF_SERVE_SETTLE = "ir_settle"
CONF_OD_ENABLED = "on_demand_enabled"
CONF_OD_MIN_OPEN = "on_demand_min_open_minutes"
CONF_OD_COOLDOWN = "on_demand_cooldown_minutes"
CONF_OD_HOLD = "on_demand_hold"
CONF_OD_REARM = "on_demand_rearm"

polar_feeder_ns = cg.esphome_ns.namespace("polar_feeder")
PolarFeeder = polar_feeder_ns.class_("PolarFeeder", cg.Component)
FeedNowAction = polar_feeder_ns.class_("FeedNowAction", automation.Action)
OpenLidAction = polar_feeder_ns.class_("OpenLidAction", automation.Action)
CloseNowAction = polar_feeder_ns.class_("CloseNowAction", automation.Action)
RefilledAction = polar_feeder_ns.class_("RefilledAction", automation.Action)
BootCloseAction = polar_feeder_ns.class_("BootCloseAction", automation.Action)
CancelAction = polar_feeder_ns.class_("CancelAction", automation.Action)

MEAL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TIME): cv.use_id(datetime.TimeEntity),
        cv.Required(CONF_ENABLED): cv.use_id(switch.Switch),
        cv.Optional(CONF_OPEN_MINUTES): cv.use_id(number.Number),
    }
)

# serve_and_return mode (OURS): interval schedule, storage compartments at the cooled rear (docs/feeding-strategies.md)
SERVE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SERVE_ENABLED): cv.use_id(switch.Switch),
        cv.Optional(CONF_SERVE_START): cv.use_id(datetime.TimeEntity),
        cv.Optional(CONF_SERVE_INTERVAL): cv.use_id(number.Number),
        cv.Optional(CONF_SERVE_CHOICE): cv.use_id(select.Select),
        cv.Optional(CONF_SERVE_NO_SHOW): cv.use_id(number.Number),
        cv.Optional(CONF_SERVE_PET_GONE): cv.use_id(number.Number),
        cv.Optional(CONF_SERVE_MAX_OPEN): cv.use_id(number.Number),
        cv.Optional(CONF_SERVE_REST, default=1): cv.int_range(min=1, max=12),
        # IR receiver reads "present" until the emitter has settled; presence is ignored this long after switch-on
        cv.Optional(CONF_SERVE_SETTLE, default="3s"): cv.positive_time_period_milliseconds,
        # serve_on_demand: beam-triggered serve cycles
        cv.Optional(CONF_OD_ENABLED): cv.use_id(switch.Switch),
        cv.Optional(CONF_OD_MIN_OPEN): cv.use_id(number.Number),
        cv.Optional(CONF_OD_COOLDOWN): cv.use_id(number.Number),
        # beam must stay broken this long to count, and be clear this long before it can trigger
        cv.Optional(CONF_OD_HOLD, default="300ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_OD_REARM, default="3s"): cv.positive_time_period_milliseconds,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PolarFeeder),
        cv.Required(CONF_TRAY): cv.use_id(PolarTray),
        cv.Required(CONF_LID): cv.use_id(PolarLid),
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_PET_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_IR_EMITTER): cv.use_id(output.FloatOutput),
        cv.Optional(CONF_MODE_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_OPEN_MINUTES): cv.use_id(number.Number),
        cv.Optional(CONF_LEAD_MINUTES): cv.use_id(number.Number),
        cv.Optional(CONF_FILLED): cv.use_id(number.Number),
        # app: closeDoorTime default 4 h (analysis/android-app section 4)
        cv.Optional(CONF_DEFAULT_OPEN_MINUTES, default=240): cv.int_range(min=0, max=1440),
        # One_PlanCheck @0x4200ab0a: 1800 s before the plan
        cv.Optional(CONF_DEFAULT_LEAD_MINUTES, default=30): cv.int_range(min=0, max=240),
        cv.Optional(CONF_MEALS, default=[]): cv.All(
            cv.ensure_list(MEAL_SCHEMA), cv.Length(max=12)
        ),
        # checkOnePlanBeforeOneHour @0x42008bca: 13.0 C (@0x3c0fae60), plan exactly 1 h away
        cv.Optional(CONF_THAW_THRESHOLD, default=13.0): cv.float_,
        cv.Optional(CONF_THAW_LEAD, default="60min"): cv.positive_time_period_seconds,
        # Feed_Handle: no pet on GPIO20 for 20 loops (10 s)
        cv.Optional(CONF_PET_LEAVE_TIME, default="10s"): cv.positive_time_period_milliseconds,
        # OURS: wait after a close stall before retrying, and give up after N stalls
        cv.Optional(CONF_STALL_RETRY_DELAY, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_CLOSE_STALLS, default=3): cv.int_range(min=1, max=10),
        # BSP_setExistState @0x4200cfae: duty 127 of 256
        cv.Optional(CONF_IR_LEVEL, default=127.0 / 256.0): cv.percentage,
        # OURS: IR receiver settle time after the emitter is switched on, before a close may start
        cv.Optional(CONF_IR_SETTLE, default="1s"): cv.positive_time_period_milliseconds,
        # OURS: no pre-positioning and no meal start during this time after boot
        cv.Optional(CONF_BOOT_GRACE, default="60s"): cv.positive_time_period_milliseconds,
        # Switched on for the whole feed sequence (P0_4/P0_7 may power the IR / gate sensors)
        cv.Optional(CONF_POWER_SWITCHES, default=[]): cv.ensure_list(cv.use_id(switch.Switch)),
        cv.Optional(CONF_STATE_TEXT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_EVENT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_NEXT_MEAL): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_NEXT_COMPARTMENT): sensor.sensor_schema(accuracy_decimals=0),
        cv.Optional(CONF_EMPTY): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_FEEDING): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_THAW): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_ANTI_PINCH): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_SERVE): SERVE_SCHEMA,
        cv.Optional(CONF_ON_PREPARE): automation.validate_automation({}),
        cv.Optional(CONF_ON_FEED_START): automation.validate_automation({}),
        cv.Optional(CONF_ON_LID_OPENED): automation.validate_automation({}),
        cv.Optional(CONF_ON_FEED_END): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_tray(await cg.get_variable(config[CONF_TRAY])))
    cg.add(var.set_lid(await cg.get_variable(config[CONF_LID])))
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    for key, setter in (
        (CONF_PET_SENSOR, "set_pet_sensor"),
        (CONF_TEMPERATURE, "set_temperature"),
        (CONF_IR_EMITTER, "set_ir_emitter"),
        (CONF_MODE_SELECT, "set_mode_select"),
        (CONF_OPEN_MINUTES, "set_open_minutes_number"),
        (CONF_LEAD_MINUTES, "set_lead_minutes_number"),
        (CONF_FILLED, "set_filled_number"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[key])))
    cg.add(var.set_default_open_minutes(config[CONF_DEFAULT_OPEN_MINUTES]))
    cg.add(var.set_default_lead_minutes(config[CONF_DEFAULT_LEAD_MINUTES]))
    for meal in config[CONF_MEALS]:
        t = await cg.get_variable(meal[CONF_TIME])
        en = await cg.get_variable(meal[CONF_ENABLED])
        om = await cg.get_variable(meal[CONF_OPEN_MINUTES]) if CONF_OPEN_MINUTES in meal else cg.nullptr
        cg.add(var.add_meal(t, en, om))
    if CONF_SERVE in config:
        sc = config[CONF_SERVE]
        for key, setter in (
            (CONF_SERVE_ENABLED, "set_serve_enabled_switch"),
            (CONF_SERVE_START, "set_serve_start_time"),
            (CONF_SERVE_INTERVAL, "set_serve_interval_number"),
            (CONF_SERVE_CHOICE, "set_serve_choice_select"),
            (CONF_SERVE_NO_SHOW, "set_serve_no_show_number"),
            (CONF_SERVE_PET_GONE, "set_serve_pet_gone_number"),
            (CONF_SERVE_MAX_OPEN, "set_serve_max_open_number"),
            (CONF_OD_ENABLED, "set_on_demand_enabled_switch"),
            (CONF_OD_MIN_OPEN, "set_on_demand_min_open_number"),
            (CONF_OD_COOLDOWN, "set_on_demand_cooldown_number"),
        ):
            if key in sc:
                cg.add(getattr(var, setter)(await cg.get_variable(sc[key])))
        cg.add(var.set_serve_rest_compartment(sc[CONF_SERVE_REST]))
        cg.add(var.set_serve_settle(sc[CONF_SERVE_SETTLE]))
        cg.add(var.set_on_demand_hold(sc[CONF_OD_HOLD]))
        cg.add(var.set_on_demand_rearm(sc[CONF_OD_REARM]))
    cg.add(var.set_thaw_threshold(config[CONF_THAW_THRESHOLD]))
    cg.add(var.set_thaw_lead(config[CONF_THAW_LEAD]))
    cg.add(var.set_pet_leave_time(config[CONF_PET_LEAVE_TIME]))
    cg.add(var.set_stall_retry_delay(config[CONF_STALL_RETRY_DELAY]))
    cg.add(var.set_max_close_stalls(config[CONF_MAX_CLOSE_STALLS]))
    cg.add(var.set_ir_level(config[CONF_IR_LEVEL]))
    cg.add(var.set_ir_settle(config[CONF_IR_SETTLE]))
    cg.add(var.set_boot_grace(config[CONF_BOOT_GRACE]))
    for sw in config[CONF_POWER_SWITCHES]:
        cg.add(var.add_power_switch(await cg.get_variable(sw)))
    for key, setter in (
        (CONF_STATE_TEXT, "set_state_text_sensor"),
        (CONF_LAST_EVENT, "set_event_text_sensor"),
        (CONF_NEXT_MEAL, "set_next_meal_text_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))
    if CONF_NEXT_COMPARTMENT in config:
        cg.add(var.set_next_compartment_sensor(await sensor.new_sensor(config[CONF_NEXT_COMPARTMENT])))
    for key, setter in (
        (CONF_EMPTY, "set_empty_binary_sensor"),
        (CONF_FEEDING, "set_feeding_binary_sensor"),
        (CONF_THAW, "set_thaw_binary_sensor"),
        (CONF_ANTI_PINCH, "set_anti_pinch_binary_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await binary_sensor.new_binary_sensor(config[key])))
    for key, getter in (
        (CONF_ON_PREPARE, "get_prepare_trigger"),
        (CONF_ON_FEED_START, "get_feed_start_trigger"),
        (CONF_ON_LID_OPENED, "get_lid_opened_trigger"),
    ):
        for conf in config.get(key, []):
            await automation.build_automation(getattr(var, getter)(), [(cg.int_, "compartment")], conf)
    for conf in config.get(CONF_ON_FEED_END, []):
        await automation.build_automation(
            var.get_feed_end_trigger(), [(cg.int_, "compartment"), (cg.std_string, "result")], conf
        )


SIMPLE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(PolarFeeder)})


async def _simple_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


for _name, _cls in (
    ("polar_feeder.open_lid", OpenLidAction),
    ("polar_feeder.close_now", CloseNowAction),
    ("polar_feeder.refilled", RefilledAction),
    ("polar_feeder.boot_close", BootCloseAction),
    ("polar_feeder.cancel", CancelAction),
):
    automation.register_action(_name, _cls, SIMPLE_SCHEMA, synchronous=True)(_simple_action)


@automation.register_action(
    "polar_feeder.feed_now",
    FeedNowAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarFeeder),
            # -1 = global lid-open time, 0 = until close
            cv.Optional(CONF_MINUTES, default=-1): cv.templatable(cv.int_range(min=-1, max=1440)),
        }
    ),
    synchronous=True,
)
async def feed_now_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_minutes(await cg.templatable(config[CONF_MINUTES], args, cg.int_)))
    return var
