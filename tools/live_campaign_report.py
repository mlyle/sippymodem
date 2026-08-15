#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Schedule and summarize bounded live-campaign attempts."""

from __future__ import annotations

from dataclasses import dataclass
import random
from typing import Iterable, Sequence


if __package__:
    from .live_campaign_profiles import CampaignCase, FAMILY_ORDER
else:
    from live_campaign_profiles import CampaignCase, FAMILY_ORDER


MATURITY_FULL = "full"
MATURITY_RETRAIN = "retrain"
MATURITY_DATA = "data"
MATURITY_CONNECT = "connect"
MATURITY_NO_CONNECT = "no-connect"
MATURITY_ORDER = (
    MATURITY_FULL,
    MATURITY_RETRAIN,
    MATURITY_DATA,
    MATURITY_CONNECT,
    MATURITY_NO_CONNECT,
)
OUTCOME_COMPLETE = "complete"
OUTCOME_BUSY = "busy"
OUTCOME_UNAVAILABLE = "unavailable"
OUTCOME_SETUP_ERROR = "setup-error"
OUTCOME_INFRASTRUCTURE_ERROR = "infrastructure-error"
ERROR_OUTCOMES = {
    OUTCOME_UNAVAILABLE,
    OUTCOME_SETUP_ERROR,
    OUTCOME_INFRASTRUCTURE_ERROR,
}
FAMILY_LABELS = {
    "bell103": "Bell 103",
    "v21": "V.21",
    "bell212": "Bell 212",
    "v22": "V.22",
    "v22bis": "V.22bis",
    "v23": "V.23",
    "v32": "V.32",
    "v32bis": "V.32bis",
    "v34": "V.34",
    "v90": "V.90",
    "v92": "V.92",
}


@dataclass(frozen=True)
class ScheduledAttempt:
    case: CampaignCase
    ordinal: int
    extra: bool = False


@dataclass(frozen=True)
class AttemptResult:
    case_name: str
    family: str
    role: str
    ordinal: int
    extra: bool
    physical_try: int
    outcome: str
    connected: bool = False
    initial_data: bool = False
    retrain_connected: bool = False
    retrain_data: bool = False
    retrain_requested: bool = True
    duration_seconds: float = 0.0
    modulation: str | None = None
    receive_rate: int | None = None
    transmit_rate: int | None = None
    detail: str = ""
    artifact: str = ""

    @property
    def maturity(self) -> str | None:
        if self.outcome != OUTCOME_COMPLETE:
            return None
        if (
            self.initial_data
            and self.retrain_requested
            and self.retrain_connected
            and self.retrain_data
        ):
            return MATURITY_FULL
        if (
            self.initial_data
            and self.retrain_requested
            and self.retrain_connected
        ):
            return MATURITY_RETRAIN
        if self.initial_data:
            return MATURITY_DATA
        if self.connected:
            return MATURITY_CONNECT
        return MATURITY_NO_CONNECT

    @property
    def counts_as_attempt(self) -> bool:
        return self.outcome != OUTCOME_BUSY

    def successful(self) -> bool:
        expected = MATURITY_FULL if self.retrain_requested else MATURITY_DATA
        return self.maturity == expected


def base_schedule(
    cases: Sequence[CampaignCase],
    attempts: int,
    seed: int,
    *,
    randomize: bool = True,
) -> tuple[ScheduledAttempt, ...]:
    if not cases or not 1 <= attempts <= 20:
        raise ValueError("invalid campaign schedule")
    generator = random.Random(seed)
    schedule: list[ScheduledAttempt] = []
    for ordinal in range(1, attempts + 1):
        round_cases = list(cases)
        if randomize:
            generator.shuffle(round_cases)
        schedule.extend(
            ScheduledAttempt(case, ordinal, False) for case in round_cases
        )
    return tuple(schedule)


def extra_schedule(
    cases: Sequence[CampaignCase],
    results: Iterable[AttemptResult],
    attempts: int,
    extra_attempts: int,
    seed: int,
) -> tuple[ScheduledAttempt, ...]:
    if not 0 <= extra_attempts <= 5:
        raise ValueError("invalid extra-attempt count")
    failed = {
        result.case_name
        for result in results
        if not result.extra and result.counts_as_attempt and not result.successful()
    }
    selected = [case for case in cases if case.name in failed]
    generator = random.Random(seed ^ 0xC6A4A7935BD1E995)
    schedule: list[ScheduledAttempt] = []
    for offset in range(extra_attempts):
        round_cases = list(selected)
        generator.shuffle(round_cases)
        schedule.extend(
            ScheduledAttempt(case, attempts + offset + 1, True)
            for case in round_cases
        )
    return tuple(schedule)


def _families(cases: Sequence[CampaignCase]) -> tuple[str, ...]:
    present = {case.family for case in cases}
    ordered = [family for family in FAMILY_ORDER if family in present]
    ordered.extend(sorted(present - set(ordered)))
    return tuple(ordered)


def render_dashboard(
    cases: Sequence[CampaignCase],
    attempts: int,
    results: Sequence[AttemptResult],
    *,
    revision: str,
    seed: int,
    elapsed_seconds: float,
    current: str = "",
    current_seconds: float | None = None,
) -> str:
    roles = ("originator", "callback")
    base_expected = {
        (family, role): sum(
            case.family == family and case.role == role for case in cases
        )
        * attempts
        for family in _families(cases)
        for role in roles
    }
    rows: list[str] = []
    header = (
        f"revision={revision[:12]} seed={seed} elapsed={elapsed_seconds:.0f}s "
        f"retries={sum(result.outcome == OUTCOME_BUSY for result in results)}"
    )
    rows.append(header)
    if current:
        active = "" if current_seconds is None else f" for={current_seconds:.0f}s"
        rows.append(f"current={current}{active}")
    rows.append("F=full R=retrain D=data C=connect N=no-connect E=error")
    rows.append(
        "Modulation   Originator done/exp+x F  R  D  C  N  E | "
        "Callback done/exp+x F  R  D  C  N  E"
    )

    totals = {
        role: {
            "done": 0,
            "expected": 0,
            "extra": 0,
            **{maturity: 0 for maturity in MATURITY_ORDER},
            "error": 0,
        }
        for role in roles
    }

    def role_fields(family: str, role: str) -> str:
        role_results = [
            result
            for result in results
            if result.family == family
            and result.role == role
            and result.counts_as_attempt
        ]
        counts = {
            maturity: sum(result.maturity == maturity for result in role_results)
            for maturity in MATURITY_ORDER
        }
        errors = sum(result.outcome in ERROR_OUTCOMES for result in role_results)
        done = sum(not result.extra for result in role_results)
        extras = sum(result.extra for result in role_results)
        expected = base_expected[(family, role)]
        total = totals[role]
        total["done"] += done
        total["expected"] += expected
        total["extra"] += extras
        total["error"] += errors
        for maturity in MATURITY_ORDER:
            total[maturity] += counts[maturity]
        return (
            f"{done:>3}/{expected:<3}+{extras:<2} "
            f"{counts[MATURITY_FULL]:>2} {counts[MATURITY_RETRAIN]:>2} "
            f"{counts[MATURITY_DATA]:>2} {counts[MATURITY_CONNECT]:>2} "
            f"{counts[MATURITY_NO_CONNECT]:>2} {errors:>2}"
        )

    for family in _families(cases):
        rows.append(
            f"{FAMILY_LABELS.get(family, family):<12} "
            f"{role_fields(family, 'originator')} | "
            f"{role_fields(family, 'callback')}"
        )

    def total_fields(role: str) -> str:
        total = totals[role]
        return (
            f"{total['done']:>3}/{total['expected']:<3}+{total['extra']:<2} "
            f"{total[MATURITY_FULL]:>2} {total[MATURITY_RETRAIN]:>2} "
            f"{total[MATURITY_DATA]:>2} {total[MATURITY_CONNECT]:>2} "
            f"{total[MATURITY_NO_CONNECT]:>2} {total['error']:>2}"
        )

    rows.append(
        f"{'TOTAL':<12} {total_fields('originator')} | "
        f"{total_fields('callback')}"
    )
    return "\n".join(rows)
