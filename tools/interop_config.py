#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Load the private configuration for a paired local modem call."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import stat
from typing import Any, Mapping


CONFIG_MAX = 64 * 1024
ARGUMENT_MAX = 64
AT_COMMAND_MAX = 64
PAYLOAD_MAX = 512
LOOPBACK_ADDRESS = "127.0.0.1"
PORT_NAMES = (
    "originator_sip",
    "answerer_sip",
    "originator_rtp",
    "answerer_rtp",
)
SAFE_ENVIRONMENT_KEYS = frozenset(
    {
        "SIP_CODEC",
        "SIP_FRAME_MS",
        "SIP_GAIN",
        "SIP_JB_MS",
        "SIP_LOG_LEVEL",
        "SIP_REMOTE_CLOCK",
        "SIP_RX_GAIN",
        "SIP_TX_GAIN",
    }
)
ROOT_KEYS = frozenset(
    {
        "enabled",
        "artifact_directory",
        "originator",
        "answerer",
        "originator_payload_hex",
        "answerer_payload_hex",
        "startup_timeout_seconds",
        "setup_timeout_seconds",
        "connect_timeout_seconds",
        "online_timeout_seconds",
        "cleanup_timeout_seconds",
        "ports",
    }
)
ENDPOINT_KEYS = frozenset(
    {"executable", "arguments", "environment", "at_commands"}
)


class InteropConfigError(ValueError):
    """The private interop configuration is unsafe or inconsistent."""


@dataclass(frozen=True)
class EndpointConfig:
    executable: Path
    arguments: tuple[str, ...]
    environment: tuple[tuple[str, str], ...]
    at_commands: tuple[str, ...]

    def environment_dict(self) -> dict[str, str]:
        return dict(self.environment)


@dataclass(frozen=True)
class InteropPorts:
    originator_sip: int = 0
    answerer_sip: int = 0
    originator_rtp: int = 0
    answerer_rtp: int = 0

    def values(self) -> tuple[int, ...]:
        return tuple(getattr(self, name) for name in PORT_NAMES)


@dataclass(frozen=True)
class InteropConfig:
    enabled: bool
    artifact_directory: Path
    originator: EndpointConfig
    answerer: EndpointConfig
    originator_payload: bytes
    answerer_payload: bytes
    startup_timeout_seconds: float
    setup_timeout_seconds: float
    connect_timeout_seconds: float
    online_timeout_seconds: float
    cleanup_timeout_seconds: float
    ports: InteropPorts


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InteropConfigError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _safe_text(
    value: Any,
    field_name: str,
    *,
    allow_empty: bool = False,
    limit: int = 4096,
) -> str:
    if not isinstance(value, str):
        raise InteropConfigError(f"{field_name} must be text")
    if not value and not allow_empty:
        raise InteropConfigError(f"{field_name} must not be empty")
    if len(value) > limit or any(
        ord(character) < 0x20 or ord(character) == 0x7F for character in value
    ):
        raise InteropConfigError(f"{field_name} contains unsafe text")
    return value


def _bounded_number(
    value: Any,
    field_name: str,
    minimum: float,
    maximum: float,
) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise InteropConfigError(f"{field_name} must be a number")
    result = float(value)
    if not minimum <= result <= maximum:
        raise InteropConfigError(f"{field_name} is outside its allowed range")
    return result


def _hex_bytes(value: Any, field_name: str) -> bytes:
    if not isinstance(value, str):
        raise InteropConfigError(f"{field_name} must be text")
    if (
        not value
        or len(value) > PAYLOAD_MAX * 2
        or len(value) % 2
        or re.fullmatch(r"[0-9a-fA-F]+", value) is None
    ):
        raise InteropConfigError(
            f"{field_name} must be nonempty bounded hexadecimal bytes"
        )
    return bytes.fromhex(value)


def _endpoint(value: Any, field_name: str, base: Path) -> EndpointConfig:
    if not isinstance(value, dict):
        raise InteropConfigError(f"{field_name} must be an object")
    unknown = set(value) - ENDPOINT_KEYS
    if unknown:
        raise InteropConfigError(
            f"{field_name} contains unsupported key: {sorted(unknown)[0]}"
        )

    executable = Path(
        _safe_text(value.get("executable"), f"{field_name}.executable")
    )
    if not executable.is_absolute():
        executable = base / executable
    executable = executable.absolute()

    arguments_value = value.get("arguments", [])
    if not isinstance(arguments_value, list) or len(arguments_value) > ARGUMENT_MAX:
        raise InteropConfigError(f"{field_name}.arguments must be a bounded list")
    arguments = tuple(
        _safe_text(
            argument,
            f"{field_name}.arguments[{index}]",
            allow_empty=True,
            limit=1024,
        )
        for index, argument in enumerate(arguments_value)
    )

    environment_value = value.get("environment", {})
    if not isinstance(environment_value, dict):
        raise InteropConfigError(f"{field_name}.environment must be an object")
    unknown_environment = set(environment_value) - SAFE_ENVIRONMENT_KEYS
    if unknown_environment:
        raise InteropConfigError(
            f"{field_name}.environment contains unsupported key: "
            f"{sorted(unknown_environment)[0]}"
        )
    environment = tuple(
        sorted(
            (
                key,
                _safe_text(item, f"{field_name}.environment.{key}", limit=256),
            )
            for key, item in environment_value.items()
        )
    )

    commands_value = value.get("at_commands")
    if (
        not isinstance(commands_value, list)
        or not commands_value
        or len(commands_value) > AT_COMMAND_MAX
    ):
        raise InteropConfigError(f"{field_name}.at_commands must be a bounded list")
    commands: list[str] = []
    for index, command_value in enumerate(commands_value):
        command = _safe_text(
            command_value,
            f"{field_name}.at_commands[{index}]",
            limit=256,
        )
        try:
            command.encode("ascii")
        except UnicodeEncodeError as error:
            raise InteropConfigError(
                f"{field_name}.at_commands[{index}] must be ASCII"
            ) from error
        normalized = command.upper()
        if not normalized.startswith("AT"):
            raise InteropConfigError(
                f"{field_name}.at_commands[{index}] must begin with AT"
            )
        if normalized.startswith("ATD") or normalized in {"ATA", "ATO"}:
            raise InteropConfigError(
                f"{field_name}.at_commands[{index}] is not a setup command"
            )
        commands.append(command)
    return EndpointConfig(executable, arguments, environment, tuple(commands))


def _ports(value: Any) -> InteropPorts:
    if value is None:
        return InteropPorts()
    if not isinstance(value, dict) or set(value) != set(PORT_NAMES):
        raise InteropConfigError("ports must contain all four named ports")
    numbers: list[int] = []
    for name in PORT_NAMES:
        port = value[name]
        if isinstance(port, bool) or not isinstance(port, int):
            raise InteropConfigError(f"ports.{name} must be an integer")
        if port != 0 and not 1024 <= port <= 65535:
            raise InteropConfigError(f"ports.{name} is outside its allowed range")
        numbers.append(port)
    if any(numbers) and (not all(numbers) or len(set(numbers)) != len(numbers)):
        raise InteropConfigError("ports must be all zero or four distinct ports")
    return InteropPorts(**dict(zip(PORT_NAMES, numbers)))


def _read_json(path: Path) -> tuple[dict[str, Any], os.stat_result]:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise InteropConfigError("configuration file is unavailable") from error
    try:
        attributes = os.fstat(descriptor)
        if not stat.S_ISREG(attributes.st_mode):
            raise InteropConfigError("configuration must be a regular file")
        with os.fdopen(descriptor, "r", encoding="utf-8") as stream:
            descriptor = -1
            text = stream.read(CONFIG_MAX + 1)
    except (OSError, UnicodeError) as error:
        raise InteropConfigError("configuration file is unreadable") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if len(text) > CONFIG_MAX:
        raise InteropConfigError("configuration file is too large")
    try:
        value = json.loads(text, object_pairs_hook=_object_without_duplicates)
    except (json.JSONDecodeError, InteropConfigError) as error:
        raise InteropConfigError("configuration is not valid JSON") from error
    if not isinstance(value, dict):
        raise InteropConfigError("configuration root must be an object")
    return value, attributes


def load_interop_config(path: Path) -> InteropConfig:
    """Load a bounded config, enforcing owner-only access when enabled."""

    value, attributes = _read_json(path)
    unknown = set(value) - ROOT_KEYS
    if unknown:
        raise InteropConfigError(
            f"configuration contains unsupported key: {sorted(unknown)[0]}"
        )
    enabled = value.get("enabled")
    if not isinstance(enabled, bool):
        raise InteropConfigError("enabled must be a boolean")
    if enabled and (
        attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise InteropConfigError(
            "enabled configuration must be private and owner-owned"
        )

    base = path.absolute().parent
    artifact_directory = Path(
        _safe_text(value.get("artifact_directory"), "artifact_directory")
    )
    if not artifact_directory.is_absolute():
        artifact_directory = base / artifact_directory
    artifact_directory = artifact_directory.absolute()
    originator = _endpoint(value.get("originator"), "originator", base)
    answerer = _endpoint(value.get("answerer"), "answerer", base)
    originator_payload = _hex_bytes(
        value.get("originator_payload_hex"), "originator_payload_hex"
    )
    answerer_payload = _hex_bytes(
        value.get("answerer_payload_hex"), "answerer_payload_hex"
    )
    if originator_payload in answerer_payload or answerer_payload in originator_payload:
        raise InteropConfigError(
            "directional payloads must be distinct and non-containing"
        )

    config = InteropConfig(
        enabled=enabled,
        artifact_directory=artifact_directory,
        originator=originator,
        answerer=answerer,
        originator_payload=originator_payload,
        answerer_payload=answerer_payload,
        startup_timeout_seconds=_bounded_number(
            value.get("startup_timeout_seconds", 15),
            "startup_timeout_seconds",
            0.1,
            120,
        ),
        setup_timeout_seconds=_bounded_number(
            value.get("setup_timeout_seconds", 30),
            "setup_timeout_seconds",
            0.1,
            120,
        ),
        connect_timeout_seconds=_bounded_number(
            value.get("connect_timeout_seconds", 180),
            "connect_timeout_seconds",
            1,
            600,
        ),
        online_timeout_seconds=_bounded_number(
            value.get("online_timeout_seconds", 30),
            "online_timeout_seconds",
            0.1,
            120,
        ),
        cleanup_timeout_seconds=_bounded_number(
            value.get("cleanup_timeout_seconds", 5),
            "cleanup_timeout_seconds",
            0.1,
            30,
        ),
        ports=_ports(value.get("ports")),
    )
    if enabled:
        for endpoint in (originator, answerer):
            if not endpoint.executable.is_file() or not os.access(
                endpoint.executable, os.X_OK
            ):
                raise InteropConfigError("an endpoint executable is unavailable")
    return config


def build_child_environment(
    endpoint: EndpointConfig,
    role: str,
    ports: InteropPorts,
    recording_prefix: Path,
    base_environment: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Build a loopback-only child environment without inherited SIP state."""

    if role not in {"originator", "answerer"}:
        raise InteropConfigError("invalid endpoint role")
    environment = dict(os.environ if base_environment is None else base_environment)
    for key in tuple(environment):
        if key.startswith("SIP_") or key == "SLMODEMD_CONFIG":
            environment.pop(key, None)
    environment.update(endpoint.environment_dict())
    sip_port = getattr(ports, f"{role}_sip")
    rtp_port = getattr(ports, f"{role}_rtp")
    prefix = str(recording_prefix.absolute())
    environment.update(
        {
            "SIP_BOUND_ADDR": LOOPBACK_ADDRESS,
            "SIP_PUBLIC_ADDR": LOOPBACK_ADDRESS,
            "SIP_PORT": str(sip_port),
            "SIP_RTP_PORT": str(rtp_port),
            "SIP_REGISTER": "0",
            "SIP_AUTO_ANSWER": "0",
            "SIP_RECORD_PREFIX": prefix,
            "SIP_DIRECT_RTP": "1",
            "SIP_DIRECT_RTP_PORT": str(rtp_port),
            "SIP_DIRECT_RTP_RECORD": prefix,
            # A local modem comparison must consume each arrived RTP frame.
            # Wall-clock playout can turn host scheduling delays into
            # synthetic silence inside an otherwise continuous waveform.
            "SIP_REMOTE_CLOCK": "1",
            "SIP_DIRECT_RTP_REMOTE_CLOCK": "1",
        }
    )
    if role == "originator":
        environment["SIP_TARGET"] = (
            f"sip:interop@{LOOPBACK_ADDRESS}:{ports.answerer_sip}"
        )
    return environment
