#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Curated profiles for the external-modem live campaign."""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Iterable


ROLE_ORIGINATOR = "originator"
ROLE_CALLBACK = "callback"
ROLES = (ROLE_ORIGINATOR, ROLE_CALLBACK)
SUITE_LEVEL = {"smoke": 0, "standard": 1, "extended": 2}
FAMILY_ORDER = (
    "bell103",
    "v21",
    "bell212",
    "v22",
    "v22bis",
    "v23",
    "v32",
    "v32bis",
    "v34",
    "v90",
    "v92",
)

LINK_PREFIX = ("ATZ", "ATE0", "AT&A7", "ATX3", "AT+DS=3,1")
LINK_SUFFIX = ("ATE0V1Q0", "ATS95=7")


@dataclass(frozen=True)
class CampaignCase:
    name: str
    family: str
    role: str
    line_role: str
    feature: str
    receive_rate: int | None
    transmit_rate: int | None
    symbol_rate: int | None
    at_commands: tuple[str, ...]
    callback_features: tuple[str, ...]
    level: int


def _commands(*specific: str) -> tuple[str, ...]:
    return LINK_PREFIX + tuple(specific) + LINK_SUFFIX


def _case(
    stem: str,
    family: str,
    role: str,
    commands: tuple[str, ...],
    *,
    receive_rate: int | None,
    transmit_rate: int | None,
    symbol_rate: int | None,
    feature: str = "analogue",
    line_role: str = "analogue",
    callback_features: tuple[str, ...] = ("V42",),
    level: int = 1,
) -> CampaignCase:
    return CampaignCase(
        name=f"{stem}-{role}",
        family=family,
        role=role,
        line_role=line_role,
        feature=feature,
        receive_rate=receive_rate,
        transmit_rate=transmit_rate,
        symbol_rate=symbol_rate,
        at_commands=commands,
        callback_features=callback_features,
        level=level,
    )


def _both_roles(
    stem: str,
    family: str,
    commands: tuple[str, ...],
    **fields: object,
) -> tuple[CampaignCase, CampaignCase]:
    return (
        _case(stem, family, ROLE_ORIGINATOR, commands, **fields),
        _case(stem, family, ROLE_CALLBACK, commands, **fields),
    )


def _fixed(
    stem: str,
    family: str,
    datapump: int,
    rate: int,
    symbol_rate: int,
    *,
    level: int = 1,
    callback_features: tuple[str, ...] = ("V42",),
) -> tuple[CampaignCase, CampaignCase]:
    return _both_roles(
        stem,
        family,
        _commands(f"AT+MS={datapump},0,{rate},{rate}"),
        receive_rate=rate,
        transmit_rate=rate,
        symbol_rate=symbol_rate,
        level=level,
        callback_features=callback_features,
    )


def _v34(
    rate: int, symbol_rate: int, level: int
) -> tuple[CampaignCase, CampaignCase]:
    return _both_roles(
        f"v34-{rate}-b{symbol_rate}",
        "v34",
        _commands(
            f"AT+MS=34,0,{rate},{rate}",
            f"AT+V34B={symbol_rate}",
        ),
        receive_rate=rate,
        transmit_rate=rate,
        symbol_rate=symbol_rate,
        feature="v34-forced-rate",
        level=level,
    )


def _digital(
    stem: str,
    family: str,
    datapump: int,
    downstream: int,
    upstream: int,
    symbol_rate: int,
    *,
    level: int,
    quick_connect: bool = False,
) -> tuple[CampaignCase, CampaignCase]:
    specific: list[str] = []
    if datapump == 92:
        specific.extend(
            (
                "AT+DSC=3",
                f"AT+V92Q={int(quick_connect)}",
                "AT+V92U=0",
            )
        )
    specific.extend(
        (
            "AT+V90D=1",
            f"AT+MS={datapump},0,{downstream},{downstream}",
            f"AT+V90U={upstream},{upstream},{symbol_rate}",
        )
    )
    return _both_roles(
        stem,
        family,
        _commands(*specific),
        receive_rate=downstream,
        transmit_rate=upstream,
        symbol_rate=symbol_rate,
        feature="quick-connect" if quick_connect else "digital-headend",
        line_role="digital-headend",
        callback_features=("V42", "V44") if datapump == 92 else ("V42",),
        level=level,
    )


def _pcm_upstream(
    upstream: int, level: int
) -> tuple[CampaignCase, CampaignCase]:
    return _both_roles(
        f"v92-pcmup-{upstream}",
        "v92",
        _commands(
            "AT+DSC=3",
            "AT+V92Q=0",
            f"AT+V92U=1,{upstream}",
            "AT+V90D=1",
            "AT+MS=92,0,25000,56000",
        ),
        receive_rate=56000,
        transmit_rate=upstream,
        symbol_rate=8000,
        feature="pcm-upstream",
        line_role="digital-headend",
        callback_features=("V42", "V44", "PCM"),
        level=level,
    )


def _fallback(
    datapump: int, role: str
) -> CampaignCase:
    family = f"v{datapump}"
    specific = []
    if datapump == 92:
        specific.extend(("AT+DSC=3", "AT+V92Q=0", "AT+V92U=0"))
    specific.extend(("AT+V90D=0", f"AT+MS={datapump},0,25000,56000"))
    return _case(
        f"{family}-analogue-fallback",
        family,
        role,
        _commands(*specific),
        receive_rate=None,
        transmit_rate=None,
        symbol_rate=None,
        feature="analogue-fallback",
        callback_features=("V42",),
        level=1,
    )


def _build_cases() -> tuple[CampaignCase, ...]:
    cases: list[CampaignCase] = []

    def add(items: Iterable[CampaignCase]) -> None:
        cases.extend(items)

    add(_fixed("bell103-300", "bell103", 103, 300, 300, callback_features=()))
    add(_fixed("v21-300", "v21", 21, 300, 300, level=0, callback_features=()))
    add(_fixed("bell212-1200", "bell212", 212, 1200, 600))
    add(_fixed("v22-1200", "v22", 22, 1200, 600))
    add(_fixed("v22bis-2400", "v22bis", 122, 2400, 600, level=0))
    add(
        _both_roles(
            "v23-1200-75",
            "v23",
            _commands("AT+MS=23,0,300,1200"),
            receive_rate=1200,
            transmit_rate=75,
            symbol_rate=1200,
            callback_features=(),
            level=1,
        )
    )
    add(_fixed("v32-4800", "v32", 32, 4800, 2400))
    add(_fixed("v32-9600", "v32", 32, 9600, 2400, level=0))
    for rate in (7200, 12000, 14400):
        add(_fixed(f"v32bis-{rate}", "v32bis", 32, rate, 2400))

    v34_points = (
        (2400, 2400, 1),
        (9600, 2400, 0),
        (14400, 2800, 1),
        (19200, 3000, 1),
        (26400, 3200, 1),
        (33600, 3429, 1),
        (4800, 2800, 2),
        (12000, 3000, 2),
        (21600, 3200, 2),
        (28800, 3429, 2),
    )
    for rate, symbol_rate, level in v34_points:
        add(_v34(rate, symbol_rate, level))

    digital_points = (
        (28000, 14400, 3000, 1),
        (42667, 26400, 3200, 1),
        (56000, 31200, 3429, 0),
        (34667, 19200, 3000, 2),
        (50667, 33600, 3429, 2),
    )
    for downstream, upstream, symbol_rate, level in digital_points:
        add(
            _digital(
                f"v90-d{downstream}-u{upstream}-b{symbol_rate}",
                "v90",
                90,
                downstream,
                upstream,
                symbol_rate,
                level=level,
            )
        )

    v92_points = (
        (56000, 31200, 3429, 1),
        (28000, 14400, 3000, 2),
        (42667, 26400, 3200, 2),
    )
    for downstream, upstream, symbol_rate, level in v92_points:
        add(
            _digital(
                f"v92-d{downstream}-u{upstream}-b{symbol_rate}",
                "v92",
                92,
                downstream,
                upstream,
                symbol_rate,
                level=level,
            )
        )

    for upstream, level in (
        (24000, 1),
        (28000, 2),
        (36000, 0),
        (42667, 2),
        (48000, 1),
    ):
        add(_pcm_upstream(upstream, level))

    quick = _digital(
        "v92-quick-connect",
        "v92",
        92,
        56000,
        31200,
        3429,
        quick_connect=True,
        level=2,
    )
    cases.append(replace(quick[1], level=1))
    cases.append(quick[0])
    for datapump in (90, 92):
        for role in ROLES:
            cases.append(_fallback(datapump, role))

    add(_fixed("v22-600", "v22", 22, 600, 600, level=2))

    names = [case.name for case in cases]
    if len(names) != len(set(names)):
        raise RuntimeError("duplicate live campaign case")
    return tuple(cases)


CASES = _build_cases()
CASES_BY_NAME = {case.name: case for case in CASES}


def cases_for_suite(name: str) -> tuple[CampaignCase, ...]:
    level = SUITE_LEVEL[name]
    return tuple(case for case in CASES if case.level <= level)


def select_cases(
    suite: str,
    *,
    names: Iterable[str] = (),
    families: Iterable[str] = (),
    roles: Iterable[str] = (),
    rates: Iterable[int] = (),
    symbol_rates: Iterable[int] = (),
    features: Iterable[str] = (),
) -> tuple[CampaignCase, ...]:
    requested_names = tuple(dict.fromkeys(names))
    selected = (
        [CASES_BY_NAME[name] for name in requested_names]
        if requested_names
        else list(cases_for_suite(suite))
    )
    family_set = set(families)
    role_set = set(roles)
    rate_set = set(rates)
    symbol_set = set(symbol_rates)
    feature_set = set(features)
    if family_set:
        selected = [case for case in selected if case.family in family_set]
    if role_set:
        selected = [case for case in selected if case.role in role_set]
    if rate_set:
        selected = [
            case
            for case in selected
            if case.receive_rate in rate_set or case.transmit_rate in rate_set
        ]
    if symbol_set:
        selected = [case for case in selected if case.symbol_rate in symbol_set]
    if feature_set:
        selected = [case for case in selected if case.feature in feature_set]
    return tuple(selected)
