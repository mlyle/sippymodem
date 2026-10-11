#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Load private settings for the external-modem live campaign."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any, Mapping


CONFIG_MAX = 64 * 1024
LIST_MAX = 64
TEXT_MAX = 1024
SIP_ENVIRONMENT_KEYS = frozenset(
    {
        "SIP_AUTO_ANSWER",
        "SIP_BOUND_ADDR",
        "SIP_CODEC",
        "SIP_DOMAIN",
        "SIP_FRAME_MS",
        "SIP_GAIN",
        "SIP_ID",
        "SIP_JB_MS",
        "SIP_LOGIN",
        "SIP_LOG_LEVEL",
        "SIP_PASSWORD",
        "SIP_PORT",
        "SIP_PROXY",
        "SIP_PUBLIC_ADDR",
        "SIP_REALM",
        "SIP_REGISTER",
        "SIP_REG_URI",
        "SIP_REMOTE_CLOCK",
        "SIP_RTP_PORT",
        "SIP_RX_GAIN",
        "SIP_SERVER",
        "SIP_TARGET",
        "SIP_TX_GAIN",
        "SIP_USER",
    }
)
PRIVATE_ENVIRONMENT_KEYS = frozenset(
    {
        "SIP_BOUND_ADDR",
        "SIP_DOMAIN",
        "SIP_ID",
        "SIP_LOGIN",
        "SIP_PASSWORD",
        "SIP_PROXY",
        "SIP_PUBLIC_ADDR",
        "SIP_REALM",
        "SIP_REG_URI",
        "SIP_SERVER",
        "SIP_TARGET",
        "SIP_USER",
    }
)
ROOT_KEYS = frozenset(
    {
        "enabled",
        "reference_name",
        "callback_port",
        "artifact_directory",
        "build_cache",
        "candidate",
        "outbound_environment",
        "callback_request_environment",
        "callback_answer_environment",
        "startup_timeout_seconds",
        "setup_timeout_seconds",
        "connect_timeout_seconds",
        "callback_request_timeout_seconds",
        "callback_marker_timeout_seconds",
        "callback_return_timeout_seconds",
        "callback_release_seconds",
        "online_timeout_seconds",
        "online_settle_seconds",
        "retrain_timeout_seconds",
        "cleanup_timeout_seconds",
        "inter_call_seconds",
        "busy_retry_seconds",
        "busy_retry_limit",
    }
)
BUILD_KEYS = frozenset(
    {"repository", "revision", "command", "executable", "arguments"}
)
SIP_STANDARD_PORT = 5060


class CampaignConfigError(ValueError):
    """Campaign configuration is malformed or crosses a private boundary."""


@dataclass(frozen=True)
class CandidateBuildConfig:
    repository: Path
    revision: str
    command: tuple[str, ...]
    executable: Path
    arguments: tuple[str, ...]


@dataclass(frozen=True)
class CampaignConfig:
    enabled: bool
    artifact_directory: Path
    build_cache: Path
    candidate: CandidateBuildConfig
    outbound_environment: tuple[tuple[str, str], ...]
    callback_request_environment: tuple[tuple[str, str], ...]
    callback_answer_environment: tuple[tuple[str, str], ...]
    startup_timeout_seconds: float
    setup_timeout_seconds: float
    connect_timeout_seconds: float
    callback_request_timeout_seconds: float
    callback_marker_timeout_seconds: float
    callback_return_timeout_seconds: float
    callback_release_seconds: float
    online_timeout_seconds: float
    online_settle_seconds: float
    retrain_timeout_seconds: float
    cleanup_timeout_seconds: float
    inter_call_seconds: float
    busy_retry_seconds: float
    busy_retry_limit: int
    reference_name: str = "ata"
    callback_port: int | None = None

    def environment(self, purpose: str) -> dict[str, str]:
        values = {
            "outbound": self.outbound_environment,
            "callback-request": self.callback_request_environment,
            "callback-answer": self.callback_answer_environment,
        }
        return dict(values[purpose])


def _without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CampaignConfigError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _text(
    value: Any,
    field: str,
    *,
    allow_empty: bool = False,
    limit: int = TEXT_MAX,
) -> str:
    if not isinstance(value, str) or (not value and not allow_empty):
        raise CampaignConfigError(f"{field} must be text")
    if len(value) > limit or any(
        ord(character) < 0x20 or ord(character) == 0x7F for character in value
    ):
        raise CampaignConfigError(f"{field} contains unsafe text")
    return value


def _path(value: Any, field: str, base: Path) -> Path:
    path = Path(_text(value, field))
    if not path.is_absolute():
        path = base / path
    return path.absolute()


def _relative_path(value: Any, field: str) -> Path:
    text = _text(value, field)
    pure = PurePosixPath(text)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise CampaignConfigError(f"{field} must be a safe relative path")
    return Path(*pure.parts)


def _string_list(
    value: Any, field: str, *, allow_empty_items: bool = False
) -> tuple[str, ...]:
    if not isinstance(value, list) or len(value) > LIST_MAX:
        raise CampaignConfigError(f"{field} must be a bounded list")
    return tuple(
        _text(
            item,
            f"{field}[{index}]",
            allow_empty=allow_empty_items,
        )
        for index, item in enumerate(value)
    )


def _environment(value: Any, field: str) -> tuple[tuple[str, str], ...]:
    if not isinstance(value, dict):
        raise CampaignConfigError(f"{field} must be an object")
    unknown = set(value) - SIP_ENVIRONMENT_KEYS
    if unknown:
        raise CampaignConfigError(
            f"{field} contains unsupported key: {sorted(unknown)[0]}"
        )
    return tuple(
        sorted(
            (
                key,
                _text(item, f"{field}.{key}", limit=256),
            )
            for key, item in value.items()
        )
    )


def _environment_default(
    environment: tuple[tuple[str, str], ...], key: str, value: str
) -> tuple[tuple[str, str], ...]:
    result = dict(environment)
    result.setdefault(key, value)
    return tuple(sorted(result.items()))


def _sip_port(
    environment: tuple[tuple[str, str], ...], field: str
) -> int:
    value = dict(environment)["SIP_PORT"]
    if not value.isascii() or not value.isdecimal():
        raise CampaignConfigError(f"{field}.SIP_PORT must be a decimal port")
    port = int(value, 10)
    if port > 65535:
        raise CampaignConfigError(f"{field}.SIP_PORT is outside its allowed range")
    return port


def _number(value: Any, field: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CampaignConfigError(f"{field} must be a number")
    result = float(value)
    if not minimum <= result <= maximum:
        raise CampaignConfigError(f"{field} is outside its allowed range")
    return result


def _integer(value: Any, field: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise CampaignConfigError(f"{field} must be an integer")
    if not minimum <= value <= maximum:
        raise CampaignConfigError(f"{field} is outside its allowed range")
    return value


def _candidate(value: Any, base: Path) -> CandidateBuildConfig:
    if not isinstance(value, dict):
        raise CampaignConfigError("candidate must be an object")
    unknown = set(value) - BUILD_KEYS
    if unknown:
        raise CampaignConfigError(
            f"candidate contains unsupported key: {sorted(unknown)[0]}"
        )
    command = _string_list(value.get("command"), "candidate.command")
    if not command:
        raise CampaignConfigError("candidate.command must not be empty")
    return CandidateBuildConfig(
        repository=_path(value.get("repository"), "candidate.repository", base),
        revision=_text(value.get("revision"), "candidate.revision", limit=256),
        command=command,
        executable=_relative_path(
            value.get("executable"), "candidate.executable"
        ),
        arguments=_string_list(
            value.get("arguments", []),
            "candidate.arguments",
            allow_empty_items=True,
        ),
    )


def _read(path: Path) -> tuple[dict[str, Any], os.stat_result]:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise CampaignConfigError("campaign configuration is unavailable") from error
    try:
        attributes = os.fstat(descriptor)
        if not stat.S_ISREG(attributes.st_mode):
            raise CampaignConfigError("campaign configuration must be a regular file")
        with os.fdopen(descriptor, "r", encoding="utf-8") as stream:
            descriptor = -1
            text = stream.read(CONFIG_MAX + 1)
    except (OSError, UnicodeError) as error:
        raise CampaignConfigError("campaign configuration is unreadable") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if len(text) > CONFIG_MAX:
        raise CampaignConfigError("campaign configuration is too large")
    try:
        value = json.loads(text, object_pairs_hook=_without_duplicates)
    except json.JSONDecodeError as error:
        raise CampaignConfigError("campaign configuration is invalid JSON") from error
    if not isinstance(value, dict):
        raise CampaignConfigError("campaign configuration must be an object")
    return value, attributes


def load_campaign_config(path: Path) -> CampaignConfig:
    value, attributes = _read(path)
    unknown = set(value) - ROOT_KEYS
    if unknown:
        raise CampaignConfigError(
            f"campaign configuration contains unsupported key: {sorted(unknown)[0]}"
        )
    enabled = value.get("enabled")
    if not isinstance(enabled, bool):
        raise CampaignConfigError("enabled must be boolean")
    if enabled and (
        attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise CampaignConfigError(
            "enabled campaign configuration must be owner-only"
        )
    base = path.absolute().parent
    reference_name = value.get("reference_name", "ata")
    if not isinstance(reference_name, str) or re.fullmatch(
        r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}", reference_name
    ) is None:
        raise CampaignConfigError("reference_name must be a short identifier")
    outbound = _environment(
        value.get("outbound_environment"), "outbound_environment"
    )
    request = _environment_default(
        _environment(
            value.get("callback_request_environment"),
            "callback_request_environment",
        ),
        "SIP_PORT",
        "0",
    )
    answer = _environment_default(
        _environment(
            value.get("callback_answer_environment"),
            "callback_answer_environment",
        ),
        "SIP_PORT",
        str(SIP_STANDARD_PORT),
    )
    request_port = _sip_port(request, "callback_request_environment")
    answer_port = _sip_port(answer, "callback_answer_environment")
    if answer_port == 0:
        raise CampaignConfigError(
            "callback answer SIP_PORT must be a fixed nonzero port"
        )
    callback_port = value.get("callback_port")
    if callback_port is not None:
        callback_port = _integer(callback_port, "callback_port", 1, 65535)
        if callback_port != answer_port:
            raise CampaignConfigError("callback_port must match callback answer SIP_PORT")
    if request_port == answer_port:
        raise CampaignConfigError("callback request and answer SIP_PORT must differ")
    if enabled:
        if "SIP_TARGET" not in dict(outbound):
            raise CampaignConfigError("outbound environment requires SIP_TARGET")
        if "SIP_TARGET" not in dict(request):
            raise CampaignConfigError(
                "callback request environment requires SIP_TARGET"
            )
        if "SIP_TARGET" in dict(answer):
            raise CampaignConfigError(
                "callback answer environment must not contain SIP_TARGET"
            )
    return CampaignConfig(
        enabled=enabled,
        reference_name=reference_name,
        callback_port=callback_port,
        artifact_directory=_path(
            value.get("artifact_directory"), "artifact_directory", base
        ),
        build_cache=_path(value.get("build_cache"), "build_cache", base),
        candidate=_candidate(value.get("candidate"), base),
        outbound_environment=outbound,
        callback_request_environment=request,
        callback_answer_environment=answer,
        startup_timeout_seconds=_number(
            value.get("startup_timeout_seconds", 15),
            "startup_timeout_seconds",
            1,
            120,
        ),
        setup_timeout_seconds=_number(
            value.get("setup_timeout_seconds", 5),
            "setup_timeout_seconds",
            1,
            60,
        ),
        connect_timeout_seconds=_number(
            value.get("connect_timeout_seconds", 90),
            "connect_timeout_seconds",
            5,
            300,
        ),
        callback_request_timeout_seconds=_number(
            value.get("callback_request_timeout_seconds", 90),
            "callback_request_timeout_seconds",
            5,
            300,
        ),
        callback_marker_timeout_seconds=_number(
            value.get("callback_marker_timeout_seconds", 7),
            "callback_marker_timeout_seconds",
            5,
            300,
        ),
        callback_return_timeout_seconds=_number(
            value.get("callback_return_timeout_seconds", 180),
            "callback_return_timeout_seconds",
            10,
            600,
        ),
        callback_release_seconds=_number(
            value.get("callback_release_seconds", 2),
            "callback_release_seconds",
            0,
            30,
        ),
        online_timeout_seconds=_number(
            value.get("online_timeout_seconds", 15),
            "online_timeout_seconds",
            1,
            120,
        ),
        online_settle_seconds=_number(
            value.get("online_settle_seconds", 1),
            "online_settle_seconds",
            0,
            10,
        ),
        retrain_timeout_seconds=_number(
            value.get("retrain_timeout_seconds", 42),
            "retrain_timeout_seconds",
            5,
            60,
        ),
        cleanup_timeout_seconds=_number(
            value.get("cleanup_timeout_seconds", 5),
            "cleanup_timeout_seconds",
            1,
            30,
        ),
        inter_call_seconds=_number(
            value.get("inter_call_seconds", 7),
            "inter_call_seconds",
            0,
            120,
        ),
        busy_retry_seconds=_number(
            value.get("busy_retry_seconds", 10),
            "busy_retry_seconds",
            0,
            300,
        ),
        busy_retry_limit=_integer(
            value.get("busy_retry_limit", 9),
            "busy_retry_limit",
            0,
            20,
        ),
    )


def child_environment(
    configured: Mapping[str, str],
    recording_prefix: Path,
    base: Mapping[str, str] | None = None,
) -> dict[str, str]:
    environment = dict(os.environ if base is None else base)
    for key in SIP_ENVIRONMENT_KEYS | {"SIP_RECORD_PREFIX"}:
        environment.pop(key, None)
    environment.update(configured)
    environment["SIP_RECORD_PREFIX"] = str(recording_prefix.absolute())
    return environment
