#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Load private live-call settings without exposing their values."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import re
import stat
from typing import Any


if __package__:
    from .live_profiles import (
        LINK_MODE_AT_COMMANDS,
        PROFILE_GROUPS,
        PROFILES,
        select_profiles,
        scenario_commands,
    )
else:
    from live_profiles import (
        LINK_MODE_AT_COMMANDS,
        PROFILE_GROUPS,
        PROFILES,
        select_profiles,
        scenario_commands,
    )


CONFIG_ENVIRONMENT_KEYS = frozenset(
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
        "SIP_TX_GAIN",
        "SIP_TARGET",
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
        "slmodemd",
        "artifact_directory",
        "environment",
        "at_commands",
        "startup_timeout_seconds",
        "call_timeout_seconds",
        "online_timeout_seconds",
        "hold_seconds",
        "inter_scenario_seconds",
        "plans",
    }
)
PLAN_KEYS = frozenset(
    {
        "name",
        "enabled",
        "profiles",
        "profile_groups",
        "link_modes",
        "attempts",
        "at_commands",
        "online_send_hex",
        "online_expect_hex",
        "round_trip_probe",
    }
)
PLAN_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9_.-]{0,62}[a-z0-9])?\Z")
ONLINE_PATTERN_MAX = 512
AT_COMMAND_MAX = 64
ROUND_TRIP_PROBES = frozenset({"ping"})
MAX_SCENARIOS = 512
MAX_PLANS = 64


class LiveCallConfigError(ValueError):
    """A configuration field is unsafe or inconsistent."""


@dataclass(frozen=True)
class ScenarioPlan:
    name: str
    enabled: bool
    profiles: tuple[str, ...]
    profile_groups: tuple[str, ...]
    link_modes: tuple[str, ...]
    attempts: int
    at_commands: tuple[str, ...]
    online_send: bytes = b""
    online_expect: bytes = b""
    round_trip_probe: str = ""


@dataclass(frozen=True)
class CallScenario:
    name: str
    plan_name: str
    profile_name: str
    family: str
    link_mode: str
    attempt: int
    readiness: str
    prerequisite: str
    at_commands: tuple[str, ...]
    online_send: bytes = b""
    online_expect: bytes = b""
    round_trip_probe: str = ""


@dataclass(frozen=True)
class LiveCallConfig:
    slmodemd: Path
    artifact_directory: Path
    environment: tuple[tuple[str, str], ...]
    at_commands: tuple[str, ...]
    startup_timeout_seconds: float
    call_timeout_seconds: float
    online_timeout_seconds: float
    hold_seconds: float
    inter_scenario_seconds: float
    plans: tuple[ScenarioPlan, ...]

    def environment_dict(self) -> dict[str, str]:
        return dict(self.environment)


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise LiveCallConfigError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _safe_text(value: Any, field: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise LiveCallConfigError(f"{field} must be text")
    if not value and not allow_empty:
        raise LiveCallConfigError(f"{field} must not be empty")
    if len(value) >= 256 or any(
        ord(character) < 0x20 or ord(character) == 0x7F for character in value
    ):
        raise LiveCallConfigError(f"{field} contains unsafe text")
    return value


def _commands(value: Any, field: str) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise LiveCallConfigError(f"{field} must be a list")
    commands = tuple(
        _safe_text(command, f"{field}[{index}]")
        for index, command in enumerate(value)
    )
    if len(commands) > AT_COMMAND_MAX:
        raise LiveCallConfigError(f"{field} contains too many commands")
    if any(not command.upper().startswith("AT") for command in commands):
        raise LiveCallConfigError(f"{field} entries must begin with AT")
    return commands


def _hex_bytes(value: Any, field: str) -> bytes:
    if not isinstance(value, str):
        raise LiveCallConfigError(f"{field} must be text")
    if (
        len(value) > ONLINE_PATTERN_MAX * 2
        or len(value) % 2 != 0
        or re.fullmatch(r"[0-9a-fA-F]*", value) is None
    ):
        raise LiveCallConfigError(f"{field} must be bounded hexadecimal bytes")
    return bytes.fromhex(value)


def _bounded_number(
    value: Any, field: str, minimum: float, maximum: float
) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise LiveCallConfigError(f"{field} must be a number")
    result = float(value)
    if not minimum <= result <= maximum:
        raise LiveCallConfigError(f"{field} is outside its allowed range")
    return result


def _environment(value: Any) -> tuple[tuple[str, str], ...]:
    if not isinstance(value, dict):
        raise LiveCallConfigError("environment must be an object")
    unknown = set(value) - CONFIG_ENVIRONMENT_KEYS
    if unknown:
        raise LiveCallConfigError(
            f"environment contains unsupported key: {sorted(unknown)[0]}"
        )
    return tuple(
        sorted(
            (key, _safe_text(item, f"environment.{key}"))
            for key, item in value.items()
        )
    )


def _selector_list(
    value: Any, field: str, available: frozenset[str]
) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise LiveCallConfigError(f"{field} must be a list")
    selected = tuple(
        _safe_text(item, f"{field}[{index}]")
        for index, item in enumerate(value)
    )
    if len(selected) != len(set(selected)):
        raise LiveCallConfigError(f"{field} must not contain duplicates")
    unknown = set(selected) - available
    if unknown:
        raise LiveCallConfigError(
            f"{field} contains unsupported name: {sorted(unknown)[0]}"
        )
    return selected


def _attempts(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise LiveCallConfigError(f"{field} must be an integer")
    if not 1 <= value <= 20:
        raise LiveCallConfigError(f"{field} is outside its allowed range")
    return value


def _plan(value: Any, index: int) -> ScenarioPlan:
    field = f"plans[{index}]"
    if not isinstance(value, dict):
        raise LiveCallConfigError(f"{field} must be an object")
    unknown = set(value) - PLAN_KEYS
    if unknown:
        raise LiveCallConfigError(
            f"{field} contains unsupported key: {sorted(unknown)[0]}"
        )
    name = _safe_text(value.get("name"), f"{field}.name")
    if PLAN_NAME_RE.fullmatch(name) is None:
        raise LiveCallConfigError(f"{field}.name has an invalid form")
    profiles = _selector_list(
        value.get("profiles", []),
        f"{field}.profiles",
        frozenset(PROFILES),
    )
    profile_groups = _selector_list(
        value.get("profile_groups", []),
        f"{field}.profile_groups",
        frozenset(PROFILE_GROUPS),
    )
    if not profiles and not profile_groups:
        raise LiveCallConfigError(f"{field} selects no profiles")
    link_modes = _selector_list(
        value.get("link_modes", ["lapm"]),
        f"{field}.link_modes",
        frozenset(LINK_MODE_AT_COMMANDS),
    )
    if not link_modes:
        raise LiveCallConfigError(f"{field}.link_modes must not be empty")
    enabled = value.get("enabled", False)
    if not isinstance(enabled, bool):
        raise LiveCallConfigError(f"{field}.enabled must be boolean")
    online_send = _hex_bytes(
        value.get("online_send_hex", ""), f"{field}.online_send_hex"
    )
    online_expect = _hex_bytes(
        value.get("online_expect_hex", ""), f"{field}.online_expect_hex"
    )
    round_trip_probe = value.get("round_trip_probe", "")
    if not isinstance(round_trip_probe, str) or (
        round_trip_probe and round_trip_probe not in ROUND_TRIP_PROBES
    ):
        raise LiveCallConfigError(f"{field}.round_trip_probe is unsupported")
    if online_send and not online_expect:
        raise LiveCallConfigError(
            f"{field}.online_send_hex requires an expected response"
        )
    if round_trip_probe and (not online_expect or online_send):
        raise LiveCallConfigError(
            f"{field}.round_trip_probe requires an initial receive pattern and "
            "cannot be combined with online_send_hex"
        )
    return ScenarioPlan(
        name=name,
        enabled=enabled,
        profiles=profiles,
        profile_groups=profile_groups,
        link_modes=link_modes,
        attempts=_attempts(value.get("attempts", 1), f"{field}.attempts"),
        at_commands=_commands(value.get("at_commands", []), f"{field}.at_commands"),
        online_send=online_send,
        online_expect=online_expect,
        round_trip_probe=round_trip_probe,
    )


def _private_configuration_is_protected(
    mode: int,
    environment: tuple[tuple[str, str], ...],
    plans: tuple[ScenarioPlan, ...],
) -> bool:
    contains_private_values = bool(
        PRIVATE_ENVIRONMENT_KEYS.intersection(dict(environment))
    )
    if not contains_private_values and not any(plan.enabled for plan in plans):
        return True
    return stat.S_IMODE(mode) & 0o077 == 0


def load_live_call_config(path: str | Path) -> LiveCallConfig:
    source = Path(path)
    try:
        document = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicates,
        )
        source_mode = source.stat().st_mode
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise LiveCallConfigError(
            f"cannot load live-call configuration: {error}"
        ) from error
    if not isinstance(document, dict):
        raise LiveCallConfigError("configuration root must be an object")
    unknown = set(document) - ROOT_KEYS
    if unknown:
        raise LiveCallConfigError(
            f"configuration contains unsupported key: {sorted(unknown)[0]}"
        )
    environment = _environment(document.get("environment", {}))
    plan_values = document.get("plans")
    if not isinstance(plan_values, list) or not plan_values:
        raise LiveCallConfigError("plans must be a nonempty list")
    if len(plan_values) > MAX_PLANS:
        raise LiveCallConfigError(f"plans must contain at most {MAX_PLANS} entries")
    plans = tuple(_plan(value, index) for index, value in enumerate(plan_values))
    names = [plan.name for plan in plans]
    if len(names) != len(set(names)):
        raise LiveCallConfigError("plan names must be unique")
    if not _private_configuration_is_protected(
        source_mode, environment, plans
    ):
        raise LiveCallConfigError(
            "configuration containing private values or enabled plans must be "
            "readable only by its owner"
        )
    if any(plan.enabled for plan in plans) and "SIP_TARGET" not in dict(environment):
        raise LiveCallConfigError(
            "enabled plans require one direct environment.SIP_TARGET"
        )
    parent = source.parent
    config = LiveCallConfig(
        slmodemd=parent
        / _safe_text(document.get("slmodemd", "src/slmodemd"), "slmodemd"),
        artifact_directory=parent
        / _safe_text(
            document.get("artifact_directory", "live-call-artifacts"),
            "artifact_directory",
        ),
        environment=environment,
        at_commands=_commands(document.get("at_commands", []), "at_commands"),
        startup_timeout_seconds=_bounded_number(
            document.get("startup_timeout_seconds", 15),
            "startup_timeout_seconds",
            0.1,
            120,
        ),
        call_timeout_seconds=_bounded_number(
            document.get("call_timeout_seconds", 90),
            "call_timeout_seconds",
            1,
            600,
        ),
        online_timeout_seconds=_bounded_number(
            document.get("online_timeout_seconds", 15),
            "online_timeout_seconds",
            0.1,
            120,
        ),
        hold_seconds=_bounded_number(
            document.get("hold_seconds", 3), "hold_seconds", 0, 60
        ),
        inter_scenario_seconds=_bounded_number(
            document.get("inter_scenario_seconds", 0),
            "inter_scenario_seconds",
            0,
            300,
        ),
        plans=plans,
    )
    scenario_counts = {
        plan.name: len(expand_plan(config, plan)) for plan in plans
    }
    scenario_count = sum(scenario_counts.values())
    if scenario_count > MAX_SCENARIOS:
        raise LiveCallConfigError(
            f"plans expand beyond {MAX_SCENARIOS} scenarios"
        )
    return config


def expand_plan(config: LiveCallConfig, plan: ScenarioPlan) -> tuple[CallScenario, ...]:
    profiles = select_profiles(plan.profiles, plan.profile_groups)
    scenario_count = len(profiles) * len(plan.link_modes) * plan.attempts
    if scenario_count > MAX_SCENARIOS:
        raise LiveCallConfigError(
            f"plan {plan.name} expands beyond {MAX_SCENARIOS} scenarios"
        )
    scenarios = []
    for profile in profiles:
        for link_mode in plan.link_modes:
            for attempt in range(1, plan.attempts + 1):
                scenarios.append(
                    CallScenario(
                        name=f"{plan.name}.{profile.name}.{link_mode}.{attempt}",
                        plan_name=plan.name,
                        profile_name=profile.name,
                        family=profile.family,
                        link_mode=link_mode,
                        attempt=attempt,
                        readiness=profile.readiness,
                        prerequisite=profile.prerequisite,
                        at_commands=scenario_commands(
                            profile,
                            link_mode,
                            config.at_commands,
                            plan.at_commands,
                        ),
                        online_send=plan.online_send,
                        online_expect=plan.online_expect,
                        round_trip_probe=plan.round_trip_probe,
                    )
                )
    return tuple(scenarios)


def public_manifest(config: LiveCallConfig) -> dict[str, Any]:
    """Return useful plan metadata without paths, endpoints, or credentials."""

    plans = []
    for plan in config.plans:
        scenarios = expand_plan(config, plan)
        plans.append(
            {
                "name": plan.name,
                "enabled": plan.enabled,
                "profiles": list(plan.profiles),
                "profile_groups": list(plan.profile_groups),
                "link_modes": list(plan.link_modes),
                "attempts": plan.attempts,
                "scenario_count": len(scenarios),
                "runnable_scenario_count": sum(
                    scenario.readiness == "runnable"
                    for scenario in scenarios
                ),
                "prerequisite_scenario_count": sum(
                    scenario.readiness == "prerequisite"
                    for scenario in scenarios
                ),
                "at_command_count": len(plan.at_commands),
                "terminal_data": bool(plan.online_expect),
                "round_trip_probe": plan.round_trip_probe or None,
                "scenarios": [
                    {
                        "name": scenario.name,
                        "profile": scenario.profile_name,
                        "family": scenario.family,
                        "link_mode": scenario.link_mode,
                        "attempt": scenario.attempt,
                        "readiness": scenario.readiness,
                        "prerequisite": scenario.prerequisite or None,
                    }
                    for scenario in scenarios
                ],
            }
        )
    return {
        "environment_keys": [key for key, _value in config.environment],
        "setup_command_count": len(config.at_commands),
        "inter_scenario_seconds": config.inter_scenario_seconds,
        "plans": plans,
    }
