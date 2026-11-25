"""nuki_hub: a Nuki Opener or Smart Lock on MQTT, with the topics and payloads of Nuki Hub.

The Bluetooth protocol code (nuki_ble, nuki_lock_protocol, nuki_opener_protocol) is in this
directory as well.
"""
import logging
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.components import binary_sensor, esp32_ble, esp32_ble_tracker, logger, time as time_
from esphome.components.esp32_ble import BTLoggers
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option, arduino_bundles_libsodium
from esphome.components.noise import LIBSODIUM_VERSION
from esphome.const import CONF_ID, CONF_PAYLOAD, CONF_TIME_ID, CONF_TOPIC, CONF_TRIGGER_ID, CONF_TYPE
from esphome.core import CORE

LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@sjors-lemniscap"]
DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = ["esp32_ble_client", "json", "lock"]
MULTI_CONF = True

# ESP-IDF component compiling the libsodium modules ESPHome's port leaves out
NUKI_SODIUM_DIR = Path(__file__).resolve().parent / "nuki_sodium"

_KEY_BUILD_DONE = "nuki_hub_build_requirements"


def add_nuki_build_requirements():
    """Adds the libraries, sdkconfig options and build flags once per build."""
    if CORE.data.get(_KEY_BUILD_DONE):
        return
    CORE.data[_KEY_BUILD_DONE] = True

    if arduino_bundles_libsodium():
        # arduino-esp32 (below IDF 6.0) ships espressif/libsodium, and ESPHome's
        # noise component then falls back to its PlatformIO libsodium library.
        # Ignore that one so espressif's full libsodium is the only copy;
        # noise-c links against it via the shared managed-components requirement.
        cg.add_platformio_option("lib_ignore", ["libsodium"])
        add_idf_component(
            name="espressif/libsodium",
            ref="^1.0.20~2",
        )
        # libsodium's HMAC-SHA256 can return wrong results when backed by hardware SHA
        # acceleration via mbedTLS (likely related to esphome/esphome#12707, #13021, #13234).
        # Force libsodium to use its own SHA implementation instead.
        add_idf_sdkconfig_option("CONFIG_LIBSODIUM_USE_MBEDTLS_SHA", False)
    else:
        # Everywhere else ESPHome's noise component (`api` encryption) adds its own
        # esphome/libsodium as an ESP-IDF managed component, which can't coexist with
        # espressif/libsodium. Always use ESPHome's port (same version as noise, so
        # both requests dedupe) and compile the modules it leaves out in nuki_sodium.
        add_idf_component(
            name="esphome/libsodium",
            ref=LIBSODIUM_VERSION,
        )
        add_idf_component(
            name="nuki_sodium",
            path=str(NUKI_SODIUM_DIR),
        )

    add_idf_component(
        name="crc16",
        repo="https://github.com/AzonInc/Crc16.git",
    )
    # NukiBleEsp32 is vendored directly as component sources instead of a fetched IDF
    # component, see nuki_ble.cpp/.h, nuki_lock_protocol.cpp/.h, nuki_opener_protocol.cpp/.h
    # etc. in this directory. It talks to the Nuki device via ESPHome's own (Bluedroid-based)
    # BLE stack (esp32_ble_tracker/esp32_ble_client) instead of NimBLE.

    # Nuki relies on seeing every advertisement to catch lock-state-changed notifications
    # (a status bit inside the manufacturer data - see NukiBle::parse_device()), so
    # duplicate-advertisement filtering must stay off.
    add_idf_sdkconfig_option("CONFIG_BTDM_BLE_SCAN_DUPL", False)

    # Keep the lock's GATT services cached in NVS (like bluetooth_proxy's cache_services).
    # Without it, ESPHome clears Bluedroid's cache after every disconnect and each
    # connection runs a full service discovery (~5 s with Nuki locks) before a command
    # can be sent. A cache that no longer fits the lock is dropped by NukiBle.
    add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", True)

    # Build flags
    cg.add_build_flag("-Wno-unused-result")
    cg.add_build_flag("-Wno-ignored-qualifiers")
    cg.add_build_flag("-Wno-missing-field-initializers")
    cg.add_build_flag("-Wno-maybe-uninitialized")


CONF_TOPIC_PREFIX = "topic_prefix"
CONF_SECURITY_PIN = "security_pin"
CONF_PAIRING_AS_APP = "pairing_as_app"
CONF_PAIRING_MODE_AT_BOOT = "pairing_mode_at_boot"
CONF_PAIRING = "pairing"
CONF_BLE_ADDRESS = "ble_address"
CONF_SECRET_KEY = "secret_key"
CONF_AUTHORIZATION_ID = "authorization_id"
CONF_QUERY_INTERVAL_STATE = "query_interval_state"
CONF_QUERY_INTERVAL_CONFIG = "query_interval_config"
CONF_QUERY_INTERVAL_BATTERY = "query_interval_battery"
CONF_QUERY_INTERVAL_KEYPAD = "query_interval_keypad"
CONF_QUERY_INTERVAL_AUTHORIZATIONS = "query_interval_authorizations"
CONF_QUERY_INTERVAL_TIME_CONTROL = "query_interval_time_control"
CONF_KEYPAD_CONTROL = "keypad_control"
CONF_KEYPAD_PUBLISH_CODE = "keypad_publish_code"
CONF_KEYPAD_CHECK_CODE = "keypad_check_code"
CONF_KEYPAD_FAST_LIST = "keypad_fast_list"
CONF_KEYPAD_TOPIC_PER_ENTRY = "keypad_topic_per_entry"
CONF_KEYPAD_DELETE_EXPIRED = "keypad_delete_expired"
CONF_AFTER = "after"
CONF_NAME_PREFIX = "name_prefix"
CONF_MAX_KEYPAD_ENTRIES = "max_keypad_entries"
CONF_MAX_AUTHORIZATION_ENTRIES = "max_authorization_entries"
CONF_MAX_LOG_ENTRIES = "max_log_entries"
CONF_PUBLISH_ACTIVITY_LOG = "publish_activity_log"
CONF_COMMAND_RETRIES = "command_retries"
CONF_COMMAND_RETRY_DELAY = "command_retry_delay"
CONF_BLE_GENERAL_TIMEOUT = "ble_general_timeout"
CONF_BLE_COMMAND_TIMEOUT = "ble_command_timeout"
CONF_BLE_ACTION_TIMEOUT = "ble_action_timeout"
CONF_UPDATE_TIME = "update_time"
CONF_PUBLISH_MAINTENANCE = "publish_maintenance"
CONF_MQTT_LOG = "mqtt_log"
CONF_LOG_MARKER_IN_FLASH = "log_marker_in_flash"
CONF_HOMEASSISTANT_DISCOVERY = "homeassistant_discovery"
CONF_NUKI_HUB_ID = "nuki_hub_id"
CONF_BLE_DISCONNECT_TIMEOUT = "ble_disconnect_timeout"
CONF_FAST_CONNECTION = "fast_connection"
CONF_KEEP_CONNECTED = "keep_connected"
CONF_CONNECTION_LINGER = "connection_linger"
CONF_MQTT_COMMANDS = "mqtt_commands"
CONF_CHALLENGE_PREFETCH = "challenge_prefetch"
CONF_BLUETOOTH_WATCHDOG = "bluetooth_watchdog"
CONF_INVALID_CODE_ALERT = "invalid_code_alert"
CONF_COUNT = "count"
CONF_WINDOW = "window"
CONF_DEBUG = "debug"
CONF_DEBUG_CONNECT = "connect"
CONF_DEBUG_COMMUNICATION = "communication"
CONF_DEBUG_READABLE_DATA = "readable_data"
CONF_DEBUG_HEX_DATA = "hex_data"
CONF_DEBUG_COMMAND = "command"
CONF_ACTION = "action"
CONF_PAIRING_MODE = "pairing_mode"
CONF_ON_MESSAGE = "on_message"
CONF_API_TRANSPORT = "api_transport"
CONF_MQTT_QOS = "mqtt_qos"
CONF_HYBRID_MODE = "hybrid_mode"
CONF_SEND_ACTIONS = "send_actions"
CONF_BLE_FALLBACK = "ble_fallback"
CONF_FALLBACK_TIMEOUT = "fallback_timeout"
CONF_OFFLINE_UPDATE_INTERVAL = "offline_update_interval"
CONF_AUTHORIZATION_ADD = "authorization_add"
CONF_DOOR_SENSOR_INPUT = "door_sensor_input"

TYPE_OPENER = "opener"
TYPE_LOCK = "lock"

nuki_hub_ns = cg.esphome_ns.namespace("nuki_hub")
NukiHubComponent = nuki_hub_ns.class_("NukiHubComponent", cg.Component)
NukiHubOpener = nuki_hub_ns.class_("NukiHubOpener", NukiHubComponent)
NukiHubLock = nuki_hub_ns.class_("NukiHubLock", NukiHubComponent)

NukiHubActionAction = nuki_hub_ns.class_("NukiHubActionAction", automation.Action)
NukiHubPairingModeAction = nuki_hub_ns.class_("NukiHubPairingModeAction", automation.Action)
NukiHubUnpairAction = nuki_hub_ns.class_("NukiHubUnpairAction", automation.Action)
NukiHubCommandAction = nuki_hub_ns.class_("NukiHubCommandAction", automation.Action)
NukiHubMessageTrigger = nuki_hub_ns.class_(
    "NukiHubMessageTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)
NukiHubPairedCondition = nuki_hub_ns.class_("NukiHubPairedCondition", automation.Condition)
NukiHubAvailableCondition = nuki_hub_ns.class_("NukiHubAvailableCondition", automation.Condition)



def _hex_bytes(length):
    def validator(value):
        value = cv.string_strict(value).replace(":", "").replace(" ", "")
        try:
            data = bytes.fromhex(value)
        except ValueError as err:
            raise cv.Invalid("Must be a hexadecimal string") from err
        if len(data) != length:
            raise cv.Invalid(f"Must be {length} bytes ({length * 2} hexadecimal digits), got {len(data)}")
        return value.lower()

    return validator


def _validate_id_length(config):
    # The id is the name the Nuki device stores for this authorization and seeds the keys
    # the pairing is stored under
    if len(config[CONF_ID].id) > 32:
        raise cv.Invalid("The id is used as the pairing name and may be at most 32 characters long")
    return config


def _validate_topic_prefix(value):
    value = cv.string_strict(value)
    if not value or value.endswith("/") or any(c in value for c in "#+"):
        raise cv.Invalid("Must be a topic without wildcards and without a trailing slash")
    return value


_BASE_SCHEMA = (
    cv.Schema(
        {
            cv.Required(CONF_TOPIC_PREFIX): _validate_topic_prefix,
            cv.Optional(CONF_SECURITY_PIN, default=0): cv.int_range(min=0, max=999999),
            cv.Optional(CONF_PAIRING_AS_APP, default=False): cv.boolean,
            cv.Optional(CONF_PAIRING_MODE_AT_BOOT, default=False): cv.boolean,
            cv.Optional(CONF_PAIRING): cv.Schema(
                {
                    cv.Required(CONF_BLE_ADDRESS): cv.mac_address,
                    cv.Required(CONF_SECRET_KEY): _hex_bytes(32),
                    cv.Required(CONF_AUTHORIZATION_ID): _hex_bytes(4),
                }
            ),
            cv.Optional(CONF_QUERY_INTERVAL_STATE, default="30min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_QUERY_INTERVAL_CONFIG, default="60min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_QUERY_INTERVAL_BATTERY, default="30min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_QUERY_INTERVAL_KEYPAD, default="30min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_QUERY_INTERVAL_AUTHORIZATIONS, default="60min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_QUERY_INTERVAL_TIME_CONTROL, default="60min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_KEYPAD_CONTROL, default=True): cv.boolean,
            cv.Optional(CONF_KEYPAD_PUBLISH_CODE, default=False): cv.boolean,
            cv.Optional(CONF_KEYPAD_CHECK_CODE, default=False): cv.boolean,
            cv.Optional(CONF_KEYPAD_FAST_LIST, default=False): cv.boolean,
            cv.Optional(CONF_KEYPAD_TOPIC_PER_ENTRY, default=False): cv.boolean,
            cv.Optional(CONF_KEYPAD_DELETE_EXPIRED): cv.Schema(
                {
                    cv.Optional(CONF_AFTER, default="1h"): cv.positive_time_period_seconds,
                    cv.Optional(CONF_NAME_PREFIX, default=""): cv.string_strict,
                }
            ),
            cv.Optional(CONF_MAX_KEYPAD_ENTRIES, default=100): cv.int_range(min=1, max=250),
            cv.Optional(CONF_MAX_AUTHORIZATION_ENTRIES, default=20): cv.int_range(min=1, max=100),
            cv.Optional(CONF_MAX_LOG_ENTRIES, default=5): cv.int_range(min=1, max=50),
            cv.Optional(CONF_PUBLISH_ACTIVITY_LOG, default=True): cv.boolean,
            cv.Optional(CONF_COMMAND_RETRIES, default=3): cv.int_range(min=0, max=10),
            cv.Optional(CONF_COMMAND_RETRY_DELAY, default="1s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BLE_GENERAL_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BLE_COMMAND_TIMEOUT, default="3s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BLE_ACTION_TIMEOUT, default="15s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_UPDATE_TIME, default=False): cv.boolean,
            cv.Optional(CONF_PUBLISH_MAINTENANCE, default=True): cv.boolean,
            cv.Optional(CONF_MQTT_LOG): logger.is_log_level,
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(NukiHubMessageTrigger)}
            ),
            cv.Optional(CONF_LOG_MARKER_IN_FLASH, default=False): cv.boolean,
            # The topics as an action and an event of the ESPHome API, see docs/nuki_hub.md
            cv.Optional(CONF_API_TRANSPORT, default=False): cv.boolean,
            # 1 costs about 115 ms per command unless the broker sends without delay, see docs/topics.md
            cv.Optional(CONF_MQTT_QOS, default=0): cv.int_range(min=0, max=1),
            # Lets a client create authorizations: permanent access for whoever gets the key
            cv.Optional(CONF_AUTHORIZATION_ADD, default=False): cv.boolean,
            # true for Home Assistant's default prefix, or the prefix itself
            cv.Optional(CONF_HOMEASSISTANT_DISCOVERY, default=False): cv.Any(cv.boolean, _validate_topic_prefix),
            cv.Optional(CONF_BLE_DISCONNECT_TIMEOUT, default="2s"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(seconds=20))
            ),
            cv.Optional(CONF_BLUETOOTH_WATCHDOG, default="30min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_FAST_CONNECTION, default=True): cv.boolean,
            cv.Optional(CONF_KEEP_CONNECTED, default=False): cv.boolean,
            cv.Optional(CONF_CONNECTION_LINGER, default="15s"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(minutes=10))
            ),
            cv.Optional(CONF_MQTT_COMMANDS, default=True): cv.boolean,
            # Without a value: on for an Opener, off for a Smart Lock (not tried on one)
            cv.Optional(CONF_CHALLENGE_PREFETCH): cv.boolean,
            cv.Optional(CONF_INVALID_CODE_ALERT): cv.Schema(
                {
                    cv.Optional(CONF_COUNT, default=3): cv.int_range(min=2, max=20),
                    cv.Optional(CONF_WINDOW, default="5min"): cv.positive_time_period_seconds,
                }
            ),
            cv.Optional(CONF_DEBUG): cv.Schema(
                {
                    cv.Optional(CONF_DEBUG_CONNECT, default=False): cv.boolean,
                    cv.Optional(CONF_DEBUG_COMMUNICATION, default=False): cv.boolean,
                    cv.Optional(CONF_DEBUG_READABLE_DATA, default=False): cv.boolean,
                    cv.Optional(CONF_DEBUG_HEX_DATA, default=False): cv.boolean,
                    cv.Optional(CONF_DEBUG_COMMAND, default=False): cv.boolean,
                }
            ),
        }
    )
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _validate_update_time(config):
    if config[CONF_UPDATE_TIME] and CONF_TIME_ID not in config:
        raise cv.Invalid(f"{CONF_UPDATE_TIME} needs {CONF_TIME_ID}")
    if CONF_KEYPAD_DELETE_EXPIRED in config and not (config[CONF_UPDATE_TIME] and CONF_TIME_ID in config):
        # Expiry is judged by this ESP32's clock against times the Nuki device keeps in its own
        raise cv.Invalid(f"{CONF_KEYPAD_DELETE_EXPIRED} needs {CONF_TIME_ID} and {CONF_UPDATE_TIME}: true")
    return config


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            TYPE_OPENER: _BASE_SCHEMA.extend({cv.GenerateID(): cv.declare_id(NukiHubOpener)}),
            TYPE_LOCK: _BASE_SCHEMA.extend(
                {
                    cv.GenerateID(): cv.declare_id(NukiHubLock),
                    # Door sensor emulation: this binary sensor (on: open) is reported to the lock
                    # as its door sensor
                    cv.Optional(CONF_DOOR_SENSOR_INPUT): cv.use_id(binary_sensor.BinarySensor),
                    cv.Optional(CONF_HYBRID_MODE): cv.Schema(
                        {
                            cv.Optional(CONF_TOPIC_PREFIX): cv.All(cv.string_strict, cv.Length(min=1, max=48)),
                            cv.Optional(CONF_SEND_ACTIONS, default=False): cv.boolean,
                            cv.Optional(CONF_BLE_FALLBACK, default=True): cv.boolean,
                            cv.Optional(CONF_FALLBACK_TIMEOUT, default="2s"): cv.positive_time_period_milliseconds,
                            cv.Optional(
                                CONF_OFFLINE_UPDATE_INTERVAL, default="10min"
                            ): cv.positive_time_period_milliseconds,
                        }
                    ),
                }
            ),
        },
    ),
    esp32_ble.consume_connection_slots(1, "nuki_hub"),
    _validate_id_length,
    _validate_update_time,
    cv.only_on_esp32,
)


def _final_validate(config):
    full_config = fv.full_config.get()
    if any(conf.get(CONF_API_TRANSPORT) for conf in (config if isinstance(config, list) else [config])):
        api_conf = full_config.get("api")
        if api_conf is None or not api_conf.get("custom_services") or not api_conf.get("homeassistant_services"):
            raise cv.Invalid(
                "api_transport needs an api: component with 'custom_services: true' and 'homeassistant_services: true'"
            )
    if "mqtt" not in full_config and any(
        CONF_HYBRID_MODE in conf for conf in (config if isinstance(config, list) else [config])
    ):
        raise cv.Invalid("hybrid_mode needs an mqtt: component connected to the broker the lock publishes to")
    if "mqtt" not in full_config:
        # Without MQTT only the lock entity (lock: - platform: nuki_hub) and the automations remain
        LOGGER.warning("nuki_hub: no mqtt: component configured, the Nuki Hub topics are not available.")
        return config
    mqtt_conf = full_config.get("mqtt", {})
    if not mqtt_conf.get("idf_send_async", False):
        LOGGER.warning(
            "nuki_hub: set 'idf_send_async: true' in mqtt:. Without it a publish waits for the network, and a "
            "stalled connection to the broker can hold the main loop until the watchdog restarts the ESP32."
        )
    if mqtt_conf.get("discovery", True) is not False and "api" in full_config:
        LOGGER.warning("nuki_hub: consider 'discovery: false' in mqtt: to not announce all entities over MQTT as well.")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # The id identifies this ESP32 to the Nuki device during pairing and seeds the keys the
    # pairing is stored under
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_ID].id)
    await cg.register_component(var, config)

    # The embedded Bluetooth client gets its events from the tracker, its setup() and loop()
    # are called by the component
    esp32_ble.register_bt_logger(BTLoggers.GATT)
    if config[CONF_TYPE] == TYPE_OPENER:
        await esp32_ble_tracker.register_client(var.get_nuki_opener(), config)
    else:
        await esp32_ble_tracker.register_client(var.get_nuki_lock(), config)

    if not CORE.data.get("nuki_scan_parameters_set"):
        CORE.data["nuki_scan_parameters_set"] = True
        # Nuki recommends continuous active scanning with interval == window (40ms), see lock.py
        # It is a setting of the tracker, so it applies to all Bluetooth scanning.
        ble_tracker = await cg.get_variable(config[esp32_ble_tracker.CONF_ESP32_BLE_ID])
        cg.add(ble_tracker.set_scan_interval(64))
        cg.add(ble_tracker.set_scan_window(64))
        cg.add(ble_tracker.set_scan_active(True))
        cg.add(ble_tracker.set_scan_continuous(True))

    cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
    cg.add(var.set_security_pin(config[CONF_SECURITY_PIN]))
    cg.add(var.set_pairing_as_app(config[CONF_PAIRING_AS_APP]))
    cg.add(var.set_pairing_mode_at_boot(config[CONF_PAIRING_MODE_AT_BOOT]))
    if pairing := config.get(CONF_PAIRING):
        cg.add(
            var.set_import_credentials(
                list(pairing[CONF_BLE_ADDRESS].parts),
                list(bytes.fromhex(pairing[CONF_SECRET_KEY])),
                list(bytes.fromhex(pairing[CONF_AUTHORIZATION_ID])),
            )
        )
    cg.add(var.set_query_interval_state(config[CONF_QUERY_INTERVAL_STATE]))
    cg.add(var.set_query_interval_config(config[CONF_QUERY_INTERVAL_CONFIG]))
    cg.add(var.set_query_interval_battery(config[CONF_QUERY_INTERVAL_BATTERY]))
    cg.add(var.set_query_interval_keypad(config[CONF_QUERY_INTERVAL_KEYPAD]))
    cg.add(var.set_query_interval_auth(config[CONF_QUERY_INTERVAL_AUTHORIZATIONS]))
    cg.add(var.set_query_interval_time_control(config[CONF_QUERY_INTERVAL_TIME_CONTROL]))
    cg.add(var.set_keypad_control(config[CONF_KEYPAD_CONTROL]))
    cg.add(var.set_keypad_publish_code(config[CONF_KEYPAD_PUBLISH_CODE]))
    cg.add(var.set_keypad_check_code(config[CONF_KEYPAD_CHECK_CODE]))
    cg.add(var.set_keypad_fast_list(config[CONF_KEYPAD_FAST_LIST]))
    cg.add(var.set_keypad_topic_per_entry(config[CONF_KEYPAD_TOPIC_PER_ENTRY]))
    if expired := config.get(CONF_KEYPAD_DELETE_EXPIRED):
        # At least a minute, so a code is never removed in the second it runs out
        cg.add(var.set_keypad_delete_expired(max(60, expired[CONF_AFTER].total_seconds), expired[CONF_NAME_PREFIX]))
    cg.add(var.set_max_keypad_entries(config[CONF_MAX_KEYPAD_ENTRIES]))
    cg.add(var.set_max_auth_entries(config[CONF_MAX_AUTHORIZATION_ENTRIES]))
    cg.add(var.set_max_log_entries(config[CONF_MAX_LOG_ENTRIES]))
    cg.add(var.set_publish_auth_log(config[CONF_PUBLISH_ACTIVITY_LOG]))
    cg.add(var.set_command_retries(config[CONF_COMMAND_RETRIES]))
    cg.add(var.set_command_retry_delay(config[CONF_COMMAND_RETRY_DELAY]))
    cg.add(var.set_ble_general_timeout(config[CONF_BLE_GENERAL_TIMEOUT]))
    cg.add(var.set_ble_command_timeout(config[CONF_BLE_COMMAND_TIMEOUT]))
    cg.add(var.set_ble_action_timeout(config[CONF_BLE_ACTION_TIMEOUT]))
    cg.add(var.set_update_time(config[CONF_UPDATE_TIME]))
    cg.add(var.set_publish_maintenance(config[CONF_PUBLISH_MAINTENANCE]))
    cg.add(var.set_log_marker_in_flash(config[CONF_LOG_MARKER_IN_FLASH]))
    cg.add(var.set_authorization_add(config[CONF_AUTHORIZATION_ADD]))
    if CONF_DOOR_SENSOR_INPUT in config:
        cg.add(var.set_door_sensor_input(await cg.get_variable(config[CONF_DOOR_SENSOR_INPUT])))
    if hybrid := config.get(CONF_HYBRID_MODE):
        cg.add(
            var.set_hybrid(
                hybrid[CONF_SEND_ACTIONS],
                hybrid[CONF_BLE_FALLBACK],
                hybrid[CONF_FALLBACK_TIMEOUT],
                hybrid[CONF_OFFLINE_UPDATE_INTERVAL],
            )
        )
        if CONF_TOPIC_PREFIX in hybrid:
            cg.add(var.set_hybrid_topic_prefix(hybrid[CONF_TOPIC_PREFIX].rstrip("/")))
    cg.add(var.set_mqtt_qos(config[CONF_MQTT_QOS]))
    if config[CONF_API_TRANSPORT]:
        cg.add_define("USE_NUKI_HUB_API")
        cg.add(var.set_api_transport(True))
    if discovery := config[CONF_HOMEASSISTANT_DISCOVERY]:
        cg.add(var.set_homeassistant_discovery("homeassistant" if discovery is True else discovery))
    cg.add(var.set_ble_disconnect_timeout(config[CONF_BLE_DISCONNECT_TIMEOUT]))
    cg.add(var.set_fast_connection(config[CONF_FAST_CONNECTION]))
    cg.add(var.set_keep_connected(config[CONF_KEEP_CONNECTED]))
    cg.add(var.set_connection_linger(config[CONF_CONNECTION_LINGER]))
    cg.add(var.set_mqtt_commands(config[CONF_MQTT_COMMANDS]))
    cg.add(var.set_challenge_prefetch(config.get(CONF_CHALLENGE_PREFETCH, config[CONF_TYPE] == TYPE_OPENER)))
    cg.add(var.set_bluetooth_watchdog(config[CONF_BLUETOOTH_WATCHDOG]))
    if alert := config.get(CONF_INVALID_CODE_ALERT):
        cg.add(var.set_invalid_code_alert(alert[CONF_COUNT], alert[CONF_WINDOW]))
    if CONF_MQTT_LOG in config:
        logger.request_log_listener()
        cg.add(var.set_mqtt_log_level(logger.LOG_LEVELS[config[CONF_MQTT_LOG]]))
    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    if debug := config.get(CONF_DEBUG):
        cg.add(
            var.set_debug(
                debug[CONF_DEBUG_CONNECT],
                debug[CONF_DEBUG_COMMUNICATION],
                debug[CONF_DEBUG_READABLE_DATA],
                debug[CONF_DEBUG_HEX_DATA],
                debug[CONF_DEBUG_COMMAND],
            )
        )

    for conf in config.get(CONF_ON_MESSAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "topic"), (cg.std_string, "payload")], conf)

    add_nuki_build_requirements()


# Automations

@automation.register_action(
    "nuki_hub.action",
    NukiHubActionAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(NukiHubComponent),
            cv.Required(CONF_ACTION): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def nuki_hub_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_action(await cg.templatable(config[CONF_ACTION], args, cg.std_string)))
    return var


@automation.register_action(
    "nuki_hub.set_pairing_mode",
    NukiHubPairingModeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(NukiHubComponent),
            cv.Required(CONF_PAIRING_MODE): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def nuki_hub_set_pairing_mode_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_pairing_mode(await cg.templatable(config[CONF_PAIRING_MODE], args, cg.bool_)))
    return var


_ID_ONLY_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(NukiHubComponent)})


@automation.register_action("nuki_hub.unpair", NukiHubUnpairAction, _ID_ONLY_SCHEMA, synchronous=True)
async def nuki_hub_unpair_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("nuki_hub.paired", NukiHubPairedCondition, _ID_ONLY_SCHEMA)
async def nuki_hub_paired_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("nuki_hub.available", NukiHubAvailableCondition, _ID_ONLY_SCHEMA)
async def nuki_hub_available_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "nuki_hub.command",
    NukiHubCommandAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(NukiHubComponent),
            cv.Required(CONF_TOPIC): cv.templatable(cv.string),
            cv.Required(CONF_PAYLOAD): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def nuki_hub_command_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_topic(await cg.templatable(config[CONF_TOPIC], args, cg.std_string)))
    cg.add(var.set_payload(await cg.templatable(config[CONF_PAYLOAD], args, cg.std_string)))
    return var
