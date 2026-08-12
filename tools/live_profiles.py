#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Public modem profiles for private, opt-in live-call scenarios."""

from __future__ import annotations

from dataclasses import dataclass


READINESS_RUNNABLE = "runnable"
READINESS_PREREQUISITE = "prerequisite"
V34_RECEIVE_PREREQUISITE = "AT+V34B symbol-rate control"
V34_TRANSMIT_PREREQUISITE = "AT+V34B and AT+V90U directional controls"
V90_PREREQUISITE = "V.90 datapump and AT+V90U control"
V92_PREREQUISITE = "V.92 datapump and AT+V92Q/AT+V92U controls"


@dataclass(frozen=True)
class ModemProfile:
    name: str
    family: str
    description: str
    at_commands: tuple[str, ...]
    receive_rate: int | None = None
    transmit_rate: int | None = None
    symbol_rate: int | None = None
    readiness: str = READINESS_RUNNABLE
    prerequisite: str = ""


# Keep the proven setup order intact. Per-plan commands may refine it, while
# ATS95 is applied after the complete profile so a later reset cannot disable
# the bounded CONNECT report used by the runner.
LINK_MODE_AT_COMMANDS = {
    "lapm": ("ATZ", "ATE0", "AT&A7", "ATX3", "AT+DS=3,1"),
    "raw": ("ATZ", "ATE0", "AT&A7", "ATX3", "AT\\N0", "AT+DS=0,1"),
}

V34_UPSTREAM_MIN_RATE = 4800
# The primary-rate limits match the implemented V.34 geometry. In particular,
# 2743 symbol/s is the exact 19200/7 cadence and permits rate units 2 through
# 11, so omitting it would leave a real V.34 operating point unplanned.
V34_RATES_BY_SYMBOL_RATE: dict[int, tuple[int, ...]] = {
    2400: tuple(range(2400, 21600 + 1, 2400)),
    2743: tuple(range(4800, 26400 + 1, 2400)),
    2800: tuple(range(4800, 26400 + 1, 2400)),
    3000: tuple(range(4800, 28800 + 1, 2400)),
    3200: tuple(range(4800, 31200 + 1, 2400)),
    3429: tuple(range(4800, 33600 + 1, 2400)),
}

# V.90 conventional upstream excludes the lower V.34 symbol rates. Keeping
# the receive and transmit sweeps separate avoids a low-value Cartesian matrix.
V90_UPSTREAM_RATES_BY_SYMBOL_RATE: dict[int, tuple[int, ...]] = {
    3000: tuple(range(4800, 28800 + 1, 2400)),
    3200: tuple(range(4800, 31200 + 1, 2400)),
    3429: tuple(range(4800, 33600 + 1, 2400)),
}
V90_DOWNSTREAM_RATES = tuple(
    (rate_units * 8000 + 3) // 6 for rate_units in range(21, 43)
)
V92_PCM_UPSTREAM_RATES = tuple(
    (rate_units * 8000 + 3) // 6 for rate_units in range(18, 37)
)
V90_REPRESENTATIVE_DOWNSTREAM_RATE = 56000
V90_REPRESENTATIVE_UPSTREAM_RATE = 31200
V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE = 3429


def _fixed_rate(datapump: int, rate: int) -> tuple[str, ...]:
    return (f"AT+MS={datapump},0,{rate},{rate}",)


def _v34_receive(rate: int, symbol_rate: int) -> tuple[str, ...]:
    # +MS supplies the receive-side cap used by the originating datapump; the
    # peer may select any valid rate up to it at the forced symbol cadence.
    return (f"AT+MS=34,0,300,{rate}", f"AT+V34B={symbol_rate}")


def _v34_transmit(rate: int, symbol_rate: int) -> tuple[str, ...]:
    # Leave the +MS receive range open, then pin conventional upstream
    # transmission through the directional +V90U control.
    return (
        "AT+MS=34,0,300,33600",
        f"AT+V90U={rate},{rate},{symbol_rate}",
        f"AT+V34B={symbol_rate}",
    )


def _v90_commands(
    datapump: int,
    downstream: int,
    upstream: int,
    symbol_rate: int,
    *,
    quick_connect: bool = False,
    pcm_upstream: bool = False,
) -> tuple[str, ...]:
    commands: list[str] = []
    if datapump == 92:
        commands.extend(
            (
                f"AT+V92Q={1 if quick_connect else 0}",
                f"AT+V92U={1 if pcm_upstream else 0}",
            )
        )
    commands.extend(
        (
            f"AT+MS={datapump},0,{downstream},{downstream}",
            f"AT+V90U={upstream},{upstream},{symbol_rate}",
        )
    )
    return tuple(commands)


def _v92_pcmup_commands(
    downstream_min: int, downstream_max: int, upstream: int
) -> tuple[str, ...]:
    return (
        "AT+V92Q=0",
        f"AT+V92U=1,{upstream}",
        f"AT+MS=92,0,{downstream_min},{downstream_max}",
    )


def _add(profiles: dict[str, ModemProfile], profile: ModemProfile) -> None:
    if profile.name in profiles:
        raise RuntimeError(f"duplicate live-call profile {profile.name}")
    if profile.readiness not in {
        READINESS_RUNNABLE,
        READINESS_PREREQUISITE,
    }:
        raise RuntimeError(f"invalid readiness for profile {profile.name}")
    if (profile.readiness == READINESS_PREREQUISITE) != bool(
        profile.prerequisite
    ):
        raise RuntimeError(f"invalid prerequisite for profile {profile.name}")
    profiles[profile.name] = profile


def _add_digital_profile(
    profiles: dict[str, ModemProfile],
    *,
    name: str,
    family: str,
    description: str,
    at_commands: tuple[str, ...],
    receive_rate: int | None = None,
    transmit_rate: int | None = None,
    symbol_rate: int | None = None,
) -> None:
    _add(
        profiles,
        ModemProfile(
            name=name,
            family=family,
            description=description,
            at_commands=at_commands,
            receive_rate=receive_rate,
            transmit_rate=transmit_rate,
            symbol_rate=symbol_rate,
            readiness=READINESS_PREREQUISITE,
            prerequisite=(
                V90_PREREQUISITE if family == "v90" else V92_PREREQUISITE
            ),
        ),
    )


def _build_profiles() -> dict[str, ModemProfile]:
    profiles: dict[str, ModemProfile] = {}
    lower = (
        ("bell103_300", "bell103", 103, 300, 300, "Bell 103 at 300 bit/s"),
        ("v21_300", "v21", 21, 300, 300, "V.21 at 300 bit/s"),
        ("bell212_1200", "bell212", 212, 1200, 600, "Bell 212A at 1200 bit/s"),
        ("v22_1200", "v22", 22, 1200, 600, "V.22 at 1200 bit/s"),
        ("v22bis_2400", "v22bis", 122, 2400, 600, "V.22bis at 2400 bit/s"),
    )
    for name, family, datapump, rate, symbol_rate, description in lower:
        _add(
            profiles,
            ModemProfile(
                name=name,
                family=family,
                description=description,
                at_commands=_fixed_rate(datapump, rate),
                receive_rate=rate,
                transmit_rate=rate,
                symbol_rate=symbol_rate,
            ),
        )

    # V.22 600 bit/s requires a peer selected for the same out-of-band mode,
    # so it is intentionally kept out of the ordinary implemented group.
    _add(
        profiles,
        ModemProfile(
            name="v22_600",
            family="v22",
            description="V.22 at 600 bit/s with a matched peer",
            at_commands=_fixed_rate(22, 600),
            receive_rate=600,
            transmit_rate=600,
            symbol_rate=600,
        ),
    )
    _add(
        profiles,
        ModemProfile(
            name="v23_1200_75",
            family="v23",
            description="V.23 at 1200/75 bit/s",
            at_commands=("AT+MS=23,0,300,1200",),
            receive_rate=1200,
            transmit_rate=75,
            symbol_rate=1200,
        ),
    )
    for rate in (45, 50):
        _add(
            profiles,
            ModemProfile(
                name=f"baudot_{rate}",
                family="baudot",
                description=f"Baudot TTY/TDD at {rate} baud",
                at_commands=_fixed_rate(45, rate),
                receive_rate=rate,
                transmit_rate=rate,
                symbol_rate=rate,
            ),
        )
    for rate in (4800, 9600):
        _add(
            profiles,
            ModemProfile(
                name=f"v32_{rate}",
                family="v32",
                description=f"V.32 at {rate} bit/s",
                at_commands=_fixed_rate(32, rate),
                receive_rate=rate,
                transmit_rate=rate,
                symbol_rate=2400,
            ),
        )
    # The V.32 family datapump owns all V.32bis rates and is registered as 32;
    # enum value 132 is only an unregistered legacy alias in this tree.
    for rate in (7200, 12000, 14400):
        _add(
            profiles,
            ModemProfile(
                name=f"v32bis_{rate}",
                family="v32bis",
                description=f"V.32bis at {rate} bit/s",
                at_commands=_fixed_rate(32, rate),
                receive_rate=rate,
                transmit_rate=rate,
                symbol_rate=2400,
            ),
        )

    # The current source datapump supports one symmetric, mandatory
    # 2400-symbol/s V.34 baseline selected entirely through AT+MS.
    _add(
        profiles,
        ModemProfile(
            name="v34_9600",
            family="v34",
            description="current symmetric V.34 baseline at 9600 bit/s",
            at_commands=_fixed_rate(34, 9600),
            receive_rate=9600,
            transmit_rate=9600,
            symbol_rate=2400,
        ),
    )
    for symbol_rate, rates in V34_RATES_BY_SYMBOL_RATE.items():
        for rate in rates:
            _add(
                profiles,
                ModemProfile(
                    name=f"v34_rx_{rate}_b{symbol_rate}",
                    family="v34",
                    description=(
                        f"future V.34 receive cap {rate} bit/s at "
                        f"{symbol_rate} baud"
                    ),
                    at_commands=_v34_receive(rate, symbol_rate),
                    receive_rate=rate,
                    symbol_rate=symbol_rate,
                    readiness=READINESS_PREREQUISITE,
                    prerequisite=V34_RECEIVE_PREREQUISITE,
                ),
            )
            if rate >= V34_UPSTREAM_MIN_RATE:
                _add(
                    profiles,
                    ModemProfile(
                        name=f"v34_tx_{rate}_b{symbol_rate}",
                        family="v34",
                        description=(
                            f"future V.34 transmit cap {rate} bit/s at "
                            f"{symbol_rate} baud"
                        ),
                        at_commands=_v34_transmit(rate, symbol_rate),
                        transmit_rate=rate,
                        symbol_rate=symbol_rate,
                        readiness=READINESS_PREREQUISITE,
                        prerequisite=V34_TRANSMIT_PREREQUISITE,
                    ),
                )

    for datapump, family in ((90, "v90"), (92, "v92")):
        prerequisite = (
            V90_PREREQUISITE if datapump == 90 else V92_PREREQUISITE
        )
        for downstream in V90_DOWNSTREAM_RATES:
            _add_digital_profile(
                profiles,
                name=f"{family}_down_{downstream}",
                family=family,
                description=f"future V.{datapump} downstream cap sweep",
                at_commands=_v90_commands(
                    datapump,
                    downstream,
                    V90_REPRESENTATIVE_UPSTREAM_RATE,
                    V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
                ),
                receive_rate=downstream,
                transmit_rate=V90_REPRESENTATIVE_UPSTREAM_RATE,
                symbol_rate=V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
            )
        for symbol_rate, upstream_rates in (
            V90_UPSTREAM_RATES_BY_SYMBOL_RATE.items()
        ):
            for upstream in upstream_rates:
                _add_digital_profile(
                    profiles,
                    name=f"{family}_up_{upstream}_b{symbol_rate}",
                    family=family,
                    description=(
                        f"future V.{datapump} conventional upstream cap sweep"
                    ),
                    at_commands=_v90_commands(
                        datapump,
                        V90_REPRESENTATIVE_DOWNSTREAM_RATE,
                        upstream,
                        symbol_rate,
                    ),
                    receive_rate=V90_REPRESENTATIVE_DOWNSTREAM_RATE,
                    transmit_rate=upstream,
                    symbol_rate=symbol_rate,
                )
        if any(
            profile.family == family
            and profile.prerequisite != prerequisite
            for profile in profiles.values()
        ):
            raise RuntimeError(f"inconsistent {family} profile prerequisite")

    for upstream in V92_PCM_UPSTREAM_RATES:
        _add_digital_profile(
            profiles,
            name=f"v92_pcmup_{upstream}",
            family="v92",
            description="future V.92 PCM upstream cap sweep",
            at_commands=_v92_pcmup_commands(56000, 56000, upstream),
            receive_rate=56000,
            transmit_rate=upstream,
            symbol_rate=8000,
        )
    _add_digital_profile(
        profiles,
        name="v92_headend_control",
        family="v92",
        description="future conventional V.92 head-end control call",
        at_commands=(
            "AT+V92Q=0",
            "AT+V92U=0",
            "AT+MS=92,0,25000,56000",
        ),
        receive_rate=56000,
    )
    _add_digital_profile(
        profiles,
        name="v92_pcmup_originator_28000",
        family="v92",
        description="future V.92 PCM-upstream originator call at 28000 bit/s",
        at_commands=_v92_pcmup_commands(25000, 56000, 28000),
        receive_rate=56000,
        transmit_rate=28000,
        symbol_rate=8000,
    )
    _add_digital_profile(
        profiles,
        name="v92_qc_no_history",
        family="v92",
        description="future V.92 quick-connect request without retained history",
        at_commands=_v90_commands(
            92,
            V90_REPRESENTATIVE_DOWNSTREAM_RATE,
            V90_REPRESENTATIVE_UPSTREAM_RATE,
            V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
            quick_connect=True,
        ),
        receive_rate=V90_REPRESENTATIVE_DOWNSTREAM_RATE,
        transmit_rate=V90_REPRESENTATIVE_UPSTREAM_RATE,
        symbol_rate=V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
    )
    _add_digital_profile(
        profiles,
        name="v90_analog_fallback",
        family="v90",
        description="future V.90 request against an analogue peer",
        at_commands=_v90_commands(
            90,
            V90_REPRESENTATIVE_DOWNSTREAM_RATE,
            V90_REPRESENTATIVE_UPSTREAM_RATE,
            V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
        ),
        receive_rate=V90_REPRESENTATIVE_DOWNSTREAM_RATE,
        transmit_rate=V90_REPRESENTATIVE_UPSTREAM_RATE,
        symbol_rate=V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
    )
    for quick_connect in (False, True):
        for pcm_upstream in (False, True):
            feature = f"qc-{'on' if quick_connect else 'off'}-pcmup-"
            feature += "on" if pcm_upstream else "off"
            _add_digital_profile(
                profiles,
                name=f"v92_analog_fallback_{feature}",
                family="v92",
                description=f"future V.92 {feature} analogue fallback request",
                at_commands=_v90_commands(
                    92,
                    V90_REPRESENTATIVE_DOWNSTREAM_RATE,
                    V90_REPRESENTATIVE_UPSTREAM_RATE,
                    V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
                    quick_connect=quick_connect,
                    pcm_upstream=pcm_upstream,
                ),
                receive_rate=V90_REPRESENTATIVE_DOWNSTREAM_RATE,
                transmit_rate=V90_REPRESENTATIVE_UPSTREAM_RATE,
                symbol_rate=V90_REPRESENTATIVE_UPSTREAM_SYMBOL_RATE,
            )
    return profiles


PROFILES = _build_profiles()


def _named(prefix: str) -> tuple[str, ...]:
    return tuple(name for name in PROFILES if name.startswith(prefix))


PROFILE_GROUPS: dict[str, tuple[str, ...]] = {
    "lower": (
        "bell103_300",
        "v21_300",
        "bell212_1200",
        "v22_1200",
        "v22bis_2400",
        "v23_1200_75",
    ),
    "v22-600-matched": ("v22_600",),
    "baudot": _named("baudot_"),
    "v32": _named("v32_"),
    "v32bis": _named("v32bis_"),
    "v34-current": ("v34_9600",),
    "v34-rx": _named("v34_rx_"),
    "v34-tx": _named("v34_tx_"),
    "v90-down": _named("v90_down_"),
    "v90-up": _named("v90_up_"),
    "v92-down": _named("v92_down_"),
    "v92-up": _named("v92_up_"),
    "v92-pcmup": tuple(
        f"v92_pcmup_{rate}" for rate in V92_PCM_UPSTREAM_RATES
    ),
    "v92-headend": (
        "v92_headend_control",
        "v92_pcmup_originator_28000",
    ),
    "v92-qc": ("v92_qc_no_history",),
    "analog-fallback": (
        "v90_analog_fallback",
        *_named("v92_analog_fallback_"),
    ),
}
PROFILE_GROUPS["v34"] = PROFILE_GROUPS["v34-rx"] + PROFILE_GROUPS["v34-tx"]
PROFILE_GROUPS["v90"] = PROFILE_GROUPS["v90-down"] + PROFILE_GROUPS["v90-up"]
PROFILE_GROUPS["v92"] = (
    PROFILE_GROUPS["v92-down"]
    + PROFILE_GROUPS["v92-up"]
    + PROFILE_GROUPS["v92-pcmup"]
    + PROFILE_GROUPS["v92-qc"]
)
PROFILE_GROUPS["implemented"] = (
    PROFILE_GROUPS["lower"]
    + PROFILE_GROUPS["baudot"]
    + PROFILE_GROUPS["v32"]
    + PROFILE_GROUPS["v32bis"]
    + PROFILE_GROUPS["v34-current"]
)
PROFILE_GROUPS["future-digital"] = (
    PROFILE_GROUPS["v90"]
    + PROFILE_GROUPS["v92"]
    + PROFILE_GROUPS["v92-headend"]
    + PROFILE_GROUPS["analog-fallback"]
)
PROFILE_GROUPS["all"] = tuple(PROFILES)


def select_profiles(
    profile_names: tuple[str, ...], group_names: tuple[str, ...]
) -> tuple[ModemProfile, ...]:
    """Expand public selectors in order and remove duplicate profiles."""

    selected: list[str] = []
    for group in group_names:
        selected.extend(PROFILE_GROUPS[group])
    selected.extend(profile_names)
    return tuple(PROFILES[name] for name in dict.fromkeys(selected))


def scenario_commands(
    profile: ModemProfile,
    link_mode: str,
    common_commands: tuple[str, ...],
    plan_commands: tuple[str, ...],
) -> tuple[str, ...]:
    return (
        LINK_MODE_AT_COMMANDS[link_mode]
        + common_commands
        + profile.at_commands
        + plan_commands
        + ("ATE0V1Q0", "ATS95=7")
    )
