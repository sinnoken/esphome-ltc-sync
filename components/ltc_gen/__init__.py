"""ESPHome external component: synchronised SMPTE LTC generator over ESP-NOW."""

import re

from esphome import pins
import esphome.codegen as cg
from esphome.components import binary_sensor, esp32, sensor, text_sensor
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHANNEL,
    CONF_ID,
    CONF_PIN,
    CONF_TEMPERATURE,
    CONF_TIME_ID,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)
import esphome.final_validate as fv

CODEOWNERS = []
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]

ltc_gen_ns = cg.esphome_ns.namespace("ltc_gen")
LtcGen = ltc_gen_ns.class_("LtcGen", cg.Component)
Role = ltc_gen_ns.enum("Role", is_class=True)
Source = ltc_gen_ns.enum("Source", is_class=True)

CONF_ROLE = "role"
CONF_FPS = "fps"
CONF_INVERT = "invert"
CONF_PPS_PIN = "pps_pin"
CONF_LINE_PIN = "line_pin"
CONF_OUTPUT_LEVEL = "output_level"
CONF_INITIAL_WINDOW = "initial_window"
CONF_WINDOW = "window"
CONF_RESYNC_INTERVAL = "resync_interval"
CONF_TEMPERATURE_COMPENSATION = "temperature_compensation"
CONF_PERSIST_MODEL = "persist_temperature_model"
CONF_OUTPUT_OFFSET = "output_offset_us"
CONF_START_TIMECODE = "start_timecode"
CONF_PPM = "ppm"
CONF_SYNC_ERROR = "sync_error"
CONF_PHASE_ERROR = "phase_error"
CONF_TIMECODE = "timecode"
CONF_SOURCE = "source"
CONF_SYNC_STATE = "sync_state"
CONF_HOTSPOT_SSID = "hotspot_ssid"
CONF_ONLINE_TIMEOUT = "online_timeout"
CONF_ONLINE_AFTER_CHANGE = "online_after_change"
CONF_USB_KEEPS_ONLINE = "usb_keeps_online"
CONF_SYNC_GROUP = "sync_group"
CONF_SETTINGS_SYNC = "settings_sync"
CONF_LOCKED = "locked"

ROLES = {"node": Role.NODE, "master": Role.MASTER}
SOURCES = {"hotspot": Source.HOTSPOT, "espnow": Source.ESPNOW, "wifi": Source.WIFI}
# value = ltc_core::Rate enum
FPS = {"24": 0, "25": 1, "29.97df": 2, "29.97": 2, "30": 3}
NOMINAL = {0: 24, 1: 25, 2: 30, 3: 30}


def _fps(value):
    value = cv.string(value).lower().replace(" ", "")
    if value not in FPS:
        raise cv.Invalid(f"fps must be one of 24, 25, 29.97df, 30 (got {value})")
    return value


def _timecode(value):
    value = cv.string(value)
    m = re.fullmatch(r"(\d{1,2}):(\d{2}):(\d{2})[:;.](\d{2})", value)
    if not m:
        raise cv.Invalid("timecode must look like HH:MM:SS:FF")
    h, mi, s, f = (int(x) for x in m.groups())
    if h > 23 or mi > 59 or s > 59 or f > 29:
        raise cv.Invalid("timecode out of range")
    return [h, mi, s, f]


def _tc_to_frames(tc, rate):
    h, m, s, f = tc
    nom = NOMINAL[rate]
    frames = ((h * 60 + m) * 60 + s) * nom + f
    if rate == 2:  # drop frame
        total_min = h * 60 + m
        frames -= 2 * (total_min - total_min // 10)
    return frames


def _validate_source(config):
    if config[CONF_OUTPUT_LEVEL] == "line" and CONF_LINE_PIN not in config:
        raise cv.Invalid("output_level: line needs line_pin (the line-level resistor tap)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LtcGen),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_PPS_PIN): pins.internal_gpio_output_pin_schema,
            # second resistor tap for line level (docs/hardware.md); `pin` is the mic tap
            cv.Optional(CONF_LINE_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_OUTPUT_LEVEL, default="mic"): cv.one_of("mic", "line", lower=True),
            cv.Optional(CONF_INVERT, default=False): cv.boolean,
            cv.Optional(CONF_ROLE, default="node"): cv.one_of(*ROLES, lower=True),
            cv.Optional(CONF_FPS, default="30"): _fps,
            cv.Optional(CONF_SOURCE, default="hotspot"): cv.one_of(*SOURCES, lower=True),
            cv.Optional(CONF_HOTSPOT_SSID): cv.ssid,
            # hybrid (source: hotspot + wifi: component): stay online this long after boot
            cv.Optional(CONF_ONLINE_TIMEOUT, default="30min"): cv.positive_time_period_milliseconds,
            # ... and go to low power this long after the last setting change
            cv.Optional(CONF_ONLINE_AFTER_CHANGE, default="2min"): cv.positive_time_period_milliseconds,
            # stay online while plugged into a computer (USB data connection)
            cv.Optional(CONF_USB_KEEPS_ONLINE, default=True): cv.boolean,
            # nodes with the same sync_group share fps / jam / intervals over ESP-NOW
            cv.Optional(CONF_SETTINGS_SYNC, default=True): cv.boolean,
            cv.Optional(CONF_SYNC_GROUP, default="ltc"): cv.string_strict,
            # hotspot: optional (scanned when omitted); espnow: must match the master
            cv.Optional(CONF_CHANNEL): cv.int_range(min=1, max=13),
            cv.Optional(CONF_INITIAL_WINDOW, default="12s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=9)),
            ),
            cv.Optional(CONF_WINDOW, default="3s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=1)),
            ),
            # 0s = calibrate at boot (+ 1, 5, 15 min) and then free-run forever
            # default: 10min (hotspot / espnow), 1min (wifi: the radio is on anyway)
            cv.Optional(CONF_RESYNC_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TEMPERATURE_COMPENSATION, default=True): cv.boolean,
            cv.Optional(CONF_PERSIST_MODEL, default=True): cv.boolean,
            # constant correction in µs (measure once with a scope, see docs)
            cv.Optional(CONF_OUTPUT_OFFSET, default=0): cv.int_range(min=-100000, max=100000),
            cv.Optional(CONF_START_TIMECODE, default="00:00:00:00"): _timecode,
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_TIMECODE): text_sensor.text_sensor_schema(icon="mdi:timer-outline"),
            cv.Optional(CONF_SYNC_STATE): text_sensor.text_sensor_schema(
                icon="mdi:sync", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_LOCKED): binary_sensor.binary_sensor_schema(
                icon="mdi:lock-clock", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_PPM): sensor.sensor_schema(
                unit_of_measurement="ppm",
                accuracy_decimals=3,
                icon="mdi:sine-wave",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SYNC_ERROR): sensor.sensor_schema(
                unit_of_measurement="µs",
                accuracy_decimals=0,
                icon="mdi:timer-sand",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PHASE_ERROR): sensor.sensor_schema(
                unit_of_measurement="µs",
                accuracy_decimals=1,
                icon="mdi:chart-bell-curve",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_source,
)


def _final_validate(config):
    full = fv.full_config.get()
    if config[CONF_ROLE] == "node":
        if config[CONF_SOURCE] == "wifi" and "wifi" not in full:
            raise cv.Invalid("ltc_gen source: wifi needs the 'wifi:' component (connect to the hotspot / router)")
        if config[CONF_SOURCE] == "espnow" and "wifi" in full:
            raise cv.Invalid("ltc_gen source: espnow switches the radio by itself; remove 'wifi:'")
        if config[CONF_SOURCE] == "hotspot" and "wifi" not in full and CONF_HOTSPOT_SSID not in config:
            raise cv.Invalid("source: hotspot without wifi needs hotspot_ssid (the name of your phone's hotspot)")
        if config[CONF_SOURCE] == "hotspot" and "wifi" in full:
            # hybrid: wifi is switched off for low-power mode; nothing may reboot because of that
            api = full.get("api")
            if api is not None and api.get("reboot_timeout") and api["reboot_timeout"].total_milliseconds != 0:
                raise cv.Invalid("hybrid ltc_gen node: set 'api: reboot_timeout: 0s' (wifi is off in low-power mode)")
            if full["wifi"].get("reboot_timeout") and full["wifi"]["reboot_timeout"].total_milliseconds != 0:
                raise cv.Invalid("hybrid ltc_gen node: set 'wifi: reboot_timeout: 0s' (wifi is off in low-power mode)")
    if "espnow" in full:
        raise cv.Invalid(
            "ltc_gen drives ESP-NOW directly; it cannot be combined with the 'espnow:' component."
        )
    for s in full.get("sensor", []):
        if s.get("platform") == "internal_temperature":
            raise cv.Invalid(
                "ltc_gen uses the internal temperature sensor itself; use its 'temperature:' "
                "option instead of the internal_temperature platform."
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    esp32.include_builtin_idf_component("esp_driver_rmt")
    esp32.include_builtin_idf_component("esp_driver_tsens")
    esp32.include_builtin_idf_component("esp_driver_gpio")
    esp32.include_builtin_idf_component("esp_wifi")
    esp32.include_builtin_idf_component("esp_phy")
    esp32.include_builtin_idf_component("esp_event")
    esp32.include_builtin_idf_component("esp_netif")
    esp32.include_builtin_idf_component("esp_driver_usb_serial_jtag")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    if CONF_LINE_PIN in config:
        lp = await cg.gpio_pin_expression(config[CONF_LINE_PIN])
        cg.add(var.set_line_pin(lp))
    cg.add(var.set_initial_level(1 if config[CONF_OUTPUT_LEVEL] == "line" else 0))
    if CONF_PPS_PIN in config:
        pps = await cg.gpio_pin_expression(config[CONF_PPS_PIN])
        cg.add(var.set_pps_pin(pps))
    cg.add(var.set_invert(config[CONF_INVERT]))
    cg.add(var.set_role(ROLES[config[CONF_ROLE]]))
    rate = FPS[config[CONF_FPS]]
    cg.add(var.set_rate(rate))
    cg.add(var.set_source(SOURCES[config[CONF_SOURCE]]))
    cg.add(var.set_online_timeout(config[CONF_ONLINE_TIMEOUT].total_milliseconds))
    cg.add(var.set_after_change(config[CONF_ONLINE_AFTER_CHANGE].total_milliseconds))
    cg.add(var.set_usb_keeps_online(config[CONF_USB_KEEPS_ONLINE]))
    cg.add(var.set_settings_sync(config[CONF_SETTINGS_SYNC]))
    cg.add(var.set_sync_group(config[CONF_SYNC_GROUP]))
    if CONF_HOTSPOT_SSID in config:
        cg.add(var.set_hotspot_ssid(config[CONF_HOTSPOT_SSID]))
    if CONF_CHANNEL in config:
        cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_initial_window(config[CONF_INITIAL_WINDOW].total_milliseconds))
    cg.add(var.set_window(config[CONF_WINDOW].total_milliseconds))
    if CONF_RESYNC_INTERVAL in config:
        resync = config[CONF_RESYNC_INTERVAL].total_milliseconds
    else:
        resync = 60000 if config[CONF_SOURCE] == "wifi" else 600000
    cg.add(var.set_resync_interval(resync))
    cg.add(var.set_temperature_compensation(config[CONF_TEMPERATURE_COMPENSATION]))
    cg.add(var.set_persist_model(config[CONF_PERSIST_MODEL]))
    cg.add(var.set_output_offset(config[CONF_OUTPUT_OFFSET]))
    cg.add(var.set_start_timecode_frames(_tc_to_frames(config[CONF_START_TIMECODE], rate)))

    if CONF_TIME_ID in config:
        cg.add_define("LTC_GEN_USE_TIME")
        t = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(t))

    if CONF_TIMECODE in config:
        s = await text_sensor.new_text_sensor(config[CONF_TIMECODE])
        cg.add(var.set_timecode_sensor(s))
    if CONF_SYNC_STATE in config:
        s = await text_sensor.new_text_sensor(config[CONF_SYNC_STATE])
        cg.add(var.set_state_sensor(s))
    if CONF_LOCKED in config:
        s = await binary_sensor.new_binary_sensor(config[CONF_LOCKED])
        cg.add(var.set_locked_sensor(s))
    for key, setter in (
        (CONF_PPM, "set_ppm_sensor"),
        (CONF_SYNC_ERROR, "set_sync_error_sensor"),
        (CONF_PHASE_ERROR, "set_phase_error_sensor"),
        (CONF_TEMPERATURE, "set_temperature_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))
