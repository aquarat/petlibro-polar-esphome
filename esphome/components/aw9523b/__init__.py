"""AW9523B 16-bit I2C I/O expander (Awinic), modelled on the stock pca9554 / xl9535 components.

Petlibro Polar PLAF109 usage: I2C 0x58, ID register 0x10 reads 0x23, RSTN on GPIO9.
Stock init (AW9523B_i2cInit @0x4200d9c8, analysis/static/README.md section 1):
  GPIO9 low 200 ms then high; 0x12/0x13 = 0xFF (GPIO mode, no LED current mode);
  0x04 = 0x03 (P0_0/P0_1 inputs); 0x05 = 0x00 (P1 outputs); 0x11 = 0x10 (P0 push-pull).
This component writes the output registers BEFORE the direction registers so that
outputs that must be high (motor brake = 1,1) never glitch low when they become outputs.

Pin numbering follows the stock helper FUN_ram_4200d8a6: 0..7 = P0_0..P0_7, 8..15 = P1_0..P1_7.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import gpio_expander, i2c
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OUTPUT,
    CONF_RESET_PIN,
)
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType

AUTO_LOAD = ["gpio_expander"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

CONF_AW9523B = "aw9523b"
CONF_RESET_DURATION = "reset_duration"
CONF_INITIAL_HIGH_PINS = "initial_high_pins"
CONF_INPUT_PINS = "input_pins"
CONF_PORT0_PUSH_PULL = "port0_push_pull"
CONF_EXPECTED_ID = "expected_id"
CONF_FAIL_SAFE_PINS = "fail_safe_pins"
CONF_VERIFY_INTERVAL = "verify_interval"
CONF_ON_RECOVER = "on_recover"

aw9523b_ns = cg.esphome_ns.namespace("aw9523b")
AW9523BComponent = aw9523b_ns.class_("AW9523BComponent", cg.Component, i2c.I2CDevice)
AW9523BGPIOPin = aw9523b_ns.class_(
    "AW9523BGPIOPin", cg.GPIOPin, cg.Parented.template(AW9523BComponent)
)

_pin_number = cv.int_range(min=0, max=15)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.declare_id(AW9523BComponent),
            # Stock: GPIO9 low 200 ms, then high (@0x4200d9c8).
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(
                CONF_RESET_DURATION, default="200ms"
            ): cv.positive_time_period_milliseconds,
            # Pins driven high from the very first output write (before direction is set).
            cv.Optional(CONF_INITIAL_HIGH_PINS, default=[]): cv.ensure_list(_pin_number),
            # Pins kept as inputs even if no pin schema claims them (stock: P0_0, P0_1).
            # Every other pin becomes an output (stock writes 0x04=0x03, 0x05=0x00).
            cv.Optional(CONF_INPUT_PINS, default=[]): cv.ensure_list(_pin_number),
            # GCR 0x11 bit 4: P0 push-pull (stock 0x10). Chip default is open-drain.
            cv.Optional(CONF_PORT0_PUSH_PULL, default=True): cv.boolean,
            cv.Optional(CONF_EXPECTED_ID, default=0x23): cv.hex_uint8_t,
            # OURS: pins forced to their initial level on shutdown and on recovery (brakes, Peltier off)
            cv.Optional(CONF_FAIL_SAFE_PINS, default=[]): cv.ensure_list(_pin_number),
            # OURS: read back outputs/direction/GCR; 0 disables
            cv.Optional(
                CONF_VERIFY_INTERVAL, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_RECOVER): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x58))
)


def _mask(pins_list) -> int:
    m = 0
    for p in pins_list:
        m |= 1 << p
    return m


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    if reset := config.get(CONF_RESET_PIN):
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(reset)))
    cg.add(var.set_reset_duration(config[CONF_RESET_DURATION]))
    cg.add(var.set_initial_outputs(_mask(config[CONF_INITIAL_HIGH_PINS])))
    cg.add(var.set_input_mask(_mask(config[CONF_INPUT_PINS])))
    cg.add(var.set_port0_push_pull(config[CONF_PORT0_PUSH_PULL]))
    cg.add(var.set_expected_id(config[CONF_EXPECTED_ID]))
    cg.add(var.set_fail_safe_mask(_mask(config[CONF_FAIL_SAFE_PINS])))
    cg.add(var.set_verify_interval(config[CONF_VERIFY_INTERVAL]))
    for conf in config.get(CONF_ON_RECOVER, []):
        await automation.build_automation(
            var.get_recover_trigger(), [(cg.std_string, "reason")], conf
        )


def validate_mode(value: ConfigType) -> ConfigType:
    if not (value[CONF_INPUT] or value[CONF_OUTPUT]):
        raise cv.Invalid("Mode must be either input or output")
    if value[CONF_INPUT] and value[CONF_OUTPUT]:
        raise cv.Invalid("Mode must be either input or output")
    return value


AW9523B_PIN_SCHEMA = pins.gpio_base_schema(
    AW9523BGPIOPin,
    _pin_number,
    modes=[CONF_INPUT, CONF_OUTPUT],
    mode_validator=validate_mode,
).extend(
    {
        cv.Required(CONF_AW9523B): pins.use_id_or_address(AW9523BComponent),
    }
)


@pins.PIN_SCHEMA_REGISTRY.register(CONF_AW9523B, AW9523B_PIN_SCHEMA)
async def aw9523b_pin_to_code(config: ConfigType) -> MockObj:
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_AW9523B])
    cg.add(var.set_parent(parent))
    cg.add(var.set_pin(config[CONF_NUMBER]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))
    return var
