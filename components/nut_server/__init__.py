"""A NUT (Network UPS Tools) server, so NAS devices and computers can monitor the UPS directly."""

import logging

import esphome.codegen as cg
from esphome.components import socket
from esphome.components.ups_common import UpsSource
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@jamesgoodhouse"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket", "ups_common"]

CONF_UPS_HID_ID = "ups_hid_id"
CONF_UPS_NAME = "ups_name"
CONF_UPS_DESCRIPTION = "ups_description"
CONF_MAX_CONNECTIONS = "max_connections"
CONF_REQUIRE_LOGIN = "require_login"
CONF_USERS = "users"
CONF_ROLE = "role"
CONF_SYNOLOGY_COMPATIBLE = "synology_compatible"
CONF_REBOOT_IF_HA_UNREACHABLE = "reboot_if_home_assistant_unreachable"

ROLE_PRIMARY = "primary"
ROLE_SECONDARY = "secondary"

# Synology DSM's built-in NUT client always uses this UPS name and login
SYNOLOGY_UPS_NAME = "ups"
SYNOLOGY_USERNAME = "monuser"
SYNOLOGY_PASSWORD = "secret"

# ESPHome's default api reboot_timeout
API_DEFAULT_REBOOT_TIMEOUT = cv.time_period("15min")

nut_server_ns = cg.esphome_ns.namespace("nut_server")
NutServer = nut_server_ns.class_("NutServer", cg.Component)


def _nut_word(value):
    """UPS names and usernames are single words in the NUT protocol."""
    value = cv.string_strict(value)
    if not value or any(c.isspace() or c == '"' for c in value):
        raise cv.Invalid("must be a single word without spaces or quotes")
    return value


USER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_USERNAME): _nut_word,
        cv.Required(CONF_PASSWORD): cv.string_strict,
        # primary: the one machine allowed to tell everyone else to shut down.
        # secondary: monitors the UPS and shuts itself down.
        cv.Optional(CONF_ROLE, default=ROLE_SECONDARY): cv.one_of(
            ROLE_PRIMARY, ROLE_SECONDARY, lower=True
        ),
    }
)


def _validate(config):
    users = config[CONF_USERS]
    names = [user[CONF_USERNAME] for user in users]
    for name in names:
        if names.count(name) > 1:
            raise cv.Invalid(f"User '{name}' is listed more than once", [CONF_USERS])

    if config[CONF_SYNOLOGY_COMPATIBLE]:
        if config[CONF_UPS_NAME] != SYNOLOGY_UPS_NAME:
            raise cv.Invalid(
                f"Synology only connects to a UPS named '{SYNOLOGY_UPS_NAME}'. "
                f"Remove ups_name or set it to '{SYNOLOGY_UPS_NAME}' when synology_compatible is on.",
                [CONF_UPS_NAME],
            )
        if SYNOLOGY_USERNAME in names:
            raise cv.Invalid(
                f"synology_compatible already adds the '{SYNOLOGY_USERNAME}' user; "
                "remove it from users.",
                [CONF_USERS],
            )

    if config[CONF_REQUIRE_LOGIN] and not users and not config[CONF_SYNOLOGY_COMPATIBLE]:
        raise cv.Invalid(
            "require_login is on but no users are defined, so nobody could connect. "
            "Add at least one user.",
            [CONF_REQUIRE_LOGIN],
        )

    socket.consume_sockets(config[CONF_MAX_CONNECTIONS], "nut_server")(config)
    socket.consume_sockets(1, "nut_server", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NutServer),
            # Which UPS to serve; picked automatically when there is only one
            cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsSource),
            # Clients connect to <ups_name>@<this device's IP>
            cv.Optional(CONF_UPS_NAME, default="ups"): _nut_word,
            # Shown to clients; defaults to the UPS manufacturer and model
            cv.Optional(CONF_UPS_DESCRIPTION): cv.string_strict,
            cv.Optional(CONF_PORT, default=3493): cv.port,
            cv.Optional(CONF_MAX_CONNECTIONS, default=4): cv.int_range(min=1, max=8),
            # When off (the default, like stock NUT), anyone on the network can read UPS status
            cv.Optional(CONF_REQUIRE_LOGIN, default=False): cv.boolean,
            cv.Optional(CONF_USERS, default=[]): cv.ensure_list(USER_SCHEMA),
            # Adds Synology's built-in login (monuser / secret) as a read-only secondary user
            cv.Optional(CONF_SYNOLOGY_COMPATIBLE, default=False): cv.boolean,
            # ESPHome normally reboots after 15 minutes without Home Assistant. That would interrupt
            # UPS monitoring during exactly the outage it exists for, so it is turned off by default.
            cv.Optional(CONF_REBOOT_IF_HA_UNREACHABLE, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


def _final_validate(config):
    if config[CONF_REBOOT_IF_HA_UNREACHABLE]:
        return config
    api_conf = fv.full_config.get().get("api")
    if api_conf is None:
        return config
    timeout = api_conf.get("reboot_timeout")
    if timeout not in (None, API_DEFAULT_REBOOT_TIMEOUT) and timeout.total_milliseconds != 0:
        _LOGGER.warning(
            "api: reboot_timeout is set to %s, so this device will reboot when Home Assistant has been "
            "unreachable that long, briefly interrupting UPS monitoring for NUT clients. "
            "Remove reboot_timeout (nut_server then turns the reboot off) or set it to 0s.",
            timeout,
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    source = await cg.get_variable(config[CONF_UPS_HID_ID])
    cg.add(var.set_source(source))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_connections(config[CONF_MAX_CONNECTIONS]))
    cg.add(var.set_ups_name(config[CONF_UPS_NAME]))
    if CONF_UPS_DESCRIPTION in config:
        cg.add(var.set_ups_description(config[CONF_UPS_DESCRIPTION]))
    cg.add(var.set_require_login(config[CONF_REQUIRE_LOGIN]))
    for user in config[CONF_USERS]:
        cg.add(
            var.add_user(
                user[CONF_USERNAME],
                user[CONF_PASSWORD],
                user[CONF_ROLE] == ROLE_PRIMARY,
            )
        )
    if config[CONF_SYNOLOGY_COMPATIBLE]:
        cg.add(var.add_user(SYNOLOGY_USERNAME, SYNOLOGY_PASSWORD, False))

    # Keep serving NUT clients when Home Assistant is down (e.g. it runs on the NAS that is shutting down)
    api_conf = CORE.config.get("api")
    if (
        not config[CONF_REBOOT_IF_HA_UNREACHABLE]
        and api_conf is not None
        and api_conf.get("reboot_timeout") == API_DEFAULT_REBOOT_TIMEOUT
    ):
        api = await cg.get_variable(api_conf[CONF_ID])
        cg.add(api.set_reboot_timeout(0))
        cg.add(var.set_api_reboot_disabled(True))
