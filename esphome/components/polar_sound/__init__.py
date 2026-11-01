"""Petlibro Polar PLAF109 sound chip on GPIO6 (one-wire; WTN6-family protocol inferred, UNTESTED on hardware).

Stock: BSP_aduioInit @0x4200d738 (GPIO6 output, idle high), send routine FUN_ram_4200d7e8 (low 5 ms, 8 bits LSB
first, "1" = high 3 / low 1 ms, "0" = high 1 / low 3 ms), gate FUN_ram_4200c276, ringer FUN_ram_4200a23e.
Stock sends only 0xEF (feed job accepted, Feed_Handle @0x4200ae46) and 0x01 (meal call). See
docs/esphome-firmware.md "Sound".

OURS: non-blocking esp_timer transmitter, queue, volume prefix, quiet hours, "call the pet" repeater.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor, datetime, number, select, switch, text_sensor, time
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PIN, CONF_TIME_ID

AUTO_LOAD = ["select", "text_sensor", "binary_sensor"]
DEPENDENCIES = ["switch", "number", "time", "datetime"]

CONF_START_LOW = "start_low"
CONF_BIT_LONG = "bit_long"
CONF_BIT_SHORT = "bit_short"
CONF_COMMAND_GAP = "command_gap"
CONF_VOLUME_BEFORE_PLAY = "volume_before_play"
CONF_ENABLE = "enable"
CONF_VOLUME = "volume"
CONF_QUIET_ENABLE = "quiet_enable"
CONF_QUIET_START = "quiet_start"
CONF_QUIET_END = "quiet_end"
CONF_PET_SENSOR = "pet_sensor"
CONF_PET_SETTLE = "pet_settle"
CONF_CALL_ENABLE = "call_enable"
CONF_CALL_REPEATS = "call_repeats"
CONF_CALL_INTERVAL = "call_interval"
CONF_CALL_STOP_ON_PET = "call_stop_on_pet"
CONF_CALL_DEFAULT = "call_default"
CONF_SOUNDS = "sounds"
CONF_LISTED = "listed_indices"
CONF_PLAY_SELECT = "play_select"
CONF_CALL_SOUND = "call_sound"
CONF_STATUS = "status"
CONF_INDEX = "index"
CONF_CODE = "code"
CONF_FORCE = "force"
CONF_LEVEL = "level"
CONF_MEAL = "meal"

MAX_VOICE_INDEX = 0xDF  # WTN6 voices 0x00..0xDF [I]

polar_sound_ns = cg.esphome_ns.namespace("polar_sound")
PolarSound = polar_sound_ns.class_("PolarSound", cg.Component)
SoundSelect = polar_sound_ns.class_("SoundSelect", select.Select, cg.Parented.template(PolarSound))

PlayAction = polar_sound_ns.class_("PlayAction", automation.Action)
SendAction = polar_sound_ns.class_("SendAction", automation.Action)
SetVolumeAction = polar_sound_ns.class_("SetVolumeAction", automation.Action)
StopAction = polar_sound_ns.class_("StopAction", automation.Action)
LoopAction = polar_sound_ns.class_("LoopAction", automation.Action)
CallStartAction = polar_sound_ns.class_("CallStartAction", automation.Action)
CallStopAction = polar_sound_ns.class_("CallStopAction", automation.Action)
CallActiveCondition = polar_sound_ns.class_("CallActiveCondition", automation.Condition)


def _validate(config):
    listed = config[CONF_LISTED]
    for idx in config[CONF_SOUNDS]:
        if idx >= listed:
            raise cv.Invalid(f"sounds: index {idx} is not below listed_indices ({listed})")
    if config[CONF_CALL_DEFAULT] >= listed:
        raise cv.Invalid(f"call_default {config[CONF_CALL_DEFAULT]} is not below listed_indices ({listed})")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PolarSound),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            # FUN_ram_4200d7e8 timing: vTaskDelay(5) / (3) / (1) at a 1 ms tick
            cv.Optional(CONF_START_LOW, default="5ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_BIT_LONG, default="3ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_BIT_SHORT, default="1ms"): cv.positive_time_period_microseconds,
            # OURS: idle-high time between frames (stock never sends two bytes back to back)
            cv.Optional(CONF_COMMAND_GAP, default="20ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_VOLUME_BEFORE_PLAY, default=True): cv.boolean,
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_ENABLE): cv.use_id(switch.Switch),
            cv.Optional(CONF_VOLUME): cv.use_id(number.Number),
            cv.Inclusive(CONF_QUIET_ENABLE, "quiet"): cv.use_id(switch.Switch),
            cv.Inclusive(CONF_QUIET_START, "quiet"): cv.use_id(datetime.TimeEntity),
            cv.Inclusive(CONF_QUIET_END, "quiet"): cv.use_id(datetime.TimeEntity),
            cv.Optional(CONF_PET_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_PET_SETTLE, default="2s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_CALL_ENABLE): cv.use_id(switch.Switch),
            cv.Optional(CONF_CALL_REPEATS): cv.use_id(number.Number),
            cv.Optional(CONF_CALL_INTERVAL): cv.use_id(number.Number),
            cv.Optional(CONF_CALL_STOP_ON_PET): cv.use_id(switch.Switch),
            # stock meal call = sound 1 (FEED_PLAN_openDoorHandle @0x4200945e, FUN_ram_4200a23e)
            cv.Optional(CONF_CALL_DEFAULT, default=1): cv.int_range(min=0, max=MAX_VOICE_INDEX),
            cv.Optional(CONF_SOUNDS, default={1: "meal call (stock)"}): cv.Schema(
                {cv.int_range(min=0, max=MAX_VOICE_INDEX): cv.string_strict}
            ),
            cv.Optional(CONF_LISTED, default=16): cv.int_range(min=2, max=MAX_VOICE_INDEX + 1),
            cv.Optional(CONF_PLAY_SELECT): select.select_schema(SoundSelect, icon="mdi:volume-high"),
            cv.Optional(CONF_CALL_SOUND): select.select_schema(SoundSelect, icon="mdi:bullhorn"),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(icon="mdi:music-note"),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pin(await cg.gpio_pin_expression(config[CONF_PIN])))
    cg.add(var.set_timing(config[CONF_START_LOW], config[CONF_BIT_LONG], config[CONF_BIT_SHORT]))
    cg.add(var.set_command_gap(config[CONF_COMMAND_GAP]))
    cg.add(var.set_volume_before_play(config[CONF_VOLUME_BEFORE_PLAY]))
    cg.add(var.set_pet_settle(config[CONF_PET_SETTLE]))
    cg.add(var.set_default_call_index(config[CONF_CALL_DEFAULT]))
    for key, setter in (
        (CONF_TIME_ID, "set_time"),
        (CONF_ENABLE, "set_enable"),
        (CONF_VOLUME, "set_volume_number"),
        (CONF_PET_SENSOR, "set_pet_sensor"),
        (CONF_CALL_ENABLE, "set_call_enable"),
        (CONF_CALL_REPEATS, "set_call_repeats"),
        (CONF_CALL_INTERVAL, "set_call_interval"),
        (CONF_CALL_STOP_ON_PET, "set_call_stop_on_pet"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[key])))
    if CONF_QUIET_ENABLE in config:
        cg.add(
            var.set_quiet(
                await cg.get_variable(config[CONF_QUIET_ENABLE]),
                await cg.get_variable(config[CONF_QUIET_START]),
                await cg.get_variable(config[CONF_QUIET_END]),
            )
        )
    # Option i plays voice index i.
    options = [
        f"0x{i:02X} {config[CONF_SOUNDS].get(i, '(unknown)')}" for i in range(config[CONF_LISTED])
    ]
    if CONF_PLAY_SELECT in config:
        sel = await select.new_select(config[CONF_PLAY_SELECT], options=options)
        await cg.register_parented(sel, var)
        cg.add(sel.set_play_on_select(True))
        cg.add(var.set_play_select(sel))
    if CONF_CALL_SOUND in config:
        sel = await select.new_select(config[CONF_CALL_SOUND], options=options)
        await cg.register_parented(sel, var)
        cg.add(var.set_call_select(sel))
    if CONF_STATUS in config:
        cg.add(var.set_status(await text_sensor.new_text_sensor(config[CONF_STATUS])))


SIMPLE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(PolarSound)})


async def _simple_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


for _name, _cls in (
    ("polar_sound.stop", StopAction),
    ("polar_sound.loop", LoopAction),
    ("polar_sound.call_stop", CallStopAction),
):
    automation.register_action(_name, _cls, SIMPLE_SCHEMA, synchronous=True)(_simple_action)


@automation.register_condition("polar_sound.is_calling", CallActiveCondition, SIMPLE_SCHEMA)
async def is_calling_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "polar_sound.play",
    PlayAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarSound),
            cv.Required(CONF_INDEX): cv.templatable(cv.int_range(min=0, max=MAX_VOICE_INDEX)),
            # true: ignore quiet hours (the enable switch still applies)
            cv.Optional(CONF_FORCE, default=False): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def play_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.int_)))
    cg.add(var.set_force(await cg.templatable(config[CONF_FORCE], args, bool)))
    return var


@automation.register_action(
    "polar_sound.send",
    SendAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarSound),
            cv.Required(CONF_CODE): cv.templatable(cv.int_range(min=0, max=255)),
            cv.Optional(CONF_FORCE, default=False): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def send_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_code(await cg.templatable(config[CONF_CODE], args, cg.int_)))
    cg.add(var.set_force(await cg.templatable(config[CONF_FORCE], args, bool)))
    return var


@automation.register_action(
    "polar_sound.set_volume",
    SetVolumeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PolarSound),
            cv.Required(CONF_LEVEL): cv.templatable(cv.int_range(min=0, max=15)),
        }
    ),
    synchronous=True,
)
async def set_volume_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_level(await cg.templatable(config[CONF_LEVEL], args, cg.int_)))
    return var


@automation.register_action(
    "polar_sound.call_start",
    CallStartAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(PolarSound),
            # true: feeding hook, needs the call_enable switch; false: manual call
            cv.Optional(CONF_MEAL, default=False): cv.boolean,
        }
    ),
    synchronous=True,
)
async def call_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_meal(config[CONF_MEAL]))
    return var
