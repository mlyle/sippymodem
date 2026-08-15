#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Schedule and summarize bounded live-campaign attempts."""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction
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
    line_attempted: bool = False
    connected: bool = False
    initial_data: bool = False
    callback_request_maturity: str | None = None
    remote_retrain_nonce: str | None = None
    remote_retrain: bool = False
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
            and self.remote_retrain
            and self.retrain_connected
            and self.retrain_data
        ):
            return MATURITY_FULL
        if (
            self.initial_data
            and self.retrain_requested
            and self.remote_retrain
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
    if not randomize:
        return tuple(
            ScheduledAttempt(case, ordinal, False)
            for ordinal in range(1, attempts + 1)
            for case in cases
        )

    generator = random.Random(seed)
    groups: dict[tuple[str, str], list[CampaignCase]] = {}
    for case in cases:
        groups.setdefault((case.family, case.role), []).append(case)

    group_done = {group: 0 for group in groups}
    group_expected = {
        group: len(group_cases) * attempts
        for group, group_cases in groups.items()
    }
    case_done = {case.name: 0 for case in cases}
    schedule: list[ScheduledAttempt] = []
    while len(schedule) < len(cases) * attempts:
        available_groups = [
            group
            for group in groups
            if group_done[group] < group_expected[group]
        ]
        minimum = min(
            Fraction(group_done[group], group_expected[group])
            for group in available_groups
        )
        least_complete_groups = [
            group
            for group in available_groups
            if Fraction(group_done[group], group_expected[group]) == minimum
        ]
        group = generator.choice(least_complete_groups)
        minimum_case_count = min(
            case_done[case.name]
            for case in groups[group]
            if case_done[case.name] < attempts
        )
        least_complete_cases = [
            case
            for case in groups[group]
            if case_done[case.name] == minimum_case_count
        ]
        case = generator.choice(least_complete_cases)
        case_done[case.name] += 1
        group_done[group] += 1
        schedule.append(ScheduledAttempt(case, case_done[case.name], False))
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
        if not result.extra
        and result.counts_as_attempt
        and result.outcome not in ERROR_OUTCOMES
        and not result.successful()
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


def _families(
    cases: Sequence[CampaignCase],
    results: Sequence[AttemptResult] = (),
) -> tuple[str, ...]:
    present = {case.family for case in cases}
    if any(result.callback_request_maturity is not None for result in results):
        present.add("v22bis")
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
    recent_limit: int = 0,
) -> str:
    roles = ("originator", "callback")
    families = _families(cases, results)
    base_expected = {
        (family, role): sum(
            case.family == family and case.role == role for case in cases
        )
        * attempts
        for family in families
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
        "+ includes failure extras and V.22bis callback-request observations"
    )
    role_columns = "done/exp+x   F  R  D  C  N  E"
    role_width = len(role_columns)
    rows.append(
        f"{'':13}{'Originator':^{role_width}} | "
        f"{'Callback':^{role_width}}"
    )
    rows.append(
        f"{'Modulation':<12} {role_columns} | {role_columns}"
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
        request_observations = (
            [
                result.callback_request_maturity
                for result in results
                if result.callback_request_maturity is not None
            ]
            if family == "v22bis" and role == "originator"
            else []
        )
        for maturity in request_observations:
            counts[maturity] += 1
        errors = sum(result.outcome in ERROR_OUTCOMES for result in role_results)
        done = sum(not result.extra for result in role_results)
        extras = sum(result.extra for result in role_results) + len(
            request_observations
        )
        expected = base_expected[(family, role)]
        total = totals[role]
        total["done"] += done
        total["expected"] += expected
        total["extra"] += extras
        total["error"] += errors
        for maturity in MATURITY_ORDER:
            total[maturity] += counts[maturity]
        return (
            f" {done:>3}/{expected:<3}+{extras:<2} "
            f"{counts[MATURITY_FULL]:>2} {counts[MATURITY_RETRAIN]:>2} "
            f"{counts[MATURITY_DATA]:>2} {counts[MATURITY_CONNECT]:>2} "
            f"{counts[MATURITY_NO_CONNECT]:>2} {errors:>2}"
        )

    for family in families:
        rows.append(
            f"{FAMILY_LABELS.get(family, family):<12} "
            f"{role_fields(family, 'originator')} | "
            f"{role_fields(family, 'callback')}"
        )

    def total_fields(role: str) -> str:
        total = totals[role]
        return (
            f" {total['done']:>3}/{total['expected']:<3}+{total['extra']:<2} "
            f"{total[MATURITY_FULL]:>2} {total[MATURITY_RETRAIN]:>2} "
            f"{total[MATURITY_DATA]:>2} {total[MATURITY_CONNECT]:>2} "
            f"{total[MATURITY_NO_CONNECT]:>2} {total['error']:>2}"
        )

    rows.append(
        f"{'TOTAL':<12} {total_fields('originator')} | "
        f"{total_fields('callback')}"
    )
    if recent_limit > 0 and results:
        cases_by_name = {case.name: case for case in cases}
        rows.extend(
            (
                "Recent calls (newest first): o=originator c=callback l=LAPM r=raw",
                "Q=request C=connect D=initial R=remote L=local P=post",
            )
        )
        first = max(0, len(results) - recent_limit)
        for index in range(len(results) - 1, first - 1, -1):
            result = results[index]
            case = cases_by_name.get(result.case_name)
            family = FAMILY_LABELS.get(result.family, result.family)[:7]
            role = "o" if result.role == "originator" else "c"
            mode = "l" if case is not None and case.link_mode == "lapm" else "r"
            receive_rate = result.receive_rate
            transmit_rate = result.transmit_rate
            if case is not None:
                receive_rate = receive_rate or case.receive_rate
                transmit_rate = transmit_rate or case.transmit_rate
                symbol_rate = case.symbol_rate
            else:
                symbol_rate = None
            rates = f"{receive_rate or '-'}/{transmit_rate or '-'}@{symbol_rate or '-'}"
            status = result.maturity or {
                OUTCOME_INFRASTRUCTURE_ERROR: "infra-error",
                OUTCOME_UNAVAILABLE: "unavailable",
            }.get(result.outcome, result.outcome)
            sequence = f"{index + 1:03d}{'+' if result.extra else ' '}"
            rows.append(
                f"{sequence} {role} {family:<7} {rates:<17} {mode} "
                f"p{result.physical_try:<2} "
                f"Q{int(result.callback_request_maturity is not None)} "
                f"C{int(result.connected)} D{int(result.initial_data)} "
                f"R{int(result.remote_retrain)} "
                f"L{int(result.retrain_connected)} "
                f"P{int(result.retrain_data)} "
                f"{result.duration_seconds:>4.0f}s {status}"
            )
    return "\n".join(rows)
