#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Schedule and summarize bounded live-campaign attempts."""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction
import random
from typing import Iterable, Sequence


if __package__:
    from .live_campaign_profiles import (
        CampaignCase,
        FAMILY_CONNECT_MODULATIONS,
        FAMILY_ORDER,
        LINK_LAPM,
        LINK_MODES,
        LINK_RAW,
        ROLE_CALLBACK,
        ROLE_ORIGINATOR,
    )
else:
    from live_campaign_profiles import (
        CampaignCase,
        FAMILY_CONNECT_MODULATIONS,
        FAMILY_ORDER,
        LINK_LAPM,
        LINK_MODES,
        LINK_RAW,
        ROLE_CALLBACK,
        ROLE_ORIGINATOR,
    )


MATURITY_FULL = "full"
MATURITY_RETRAIN = "retrain"
MATURITY_DATA = "data"
MATURITY_CONNECT = "connect"
MATURITY_NO_CONNECT = "no-connect"
MATURITY_REQUEST = "request"
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
    link_mode: str | None = None
    line_attempted: bool = False
    connected: bool = False
    initial_data: bool = False
    callback_request_attempted: bool = False
    callback_request_maturity: str | None = None
    callback_request_modulation: str | None = None
    callback_request_protocol: str | None = None
    callback_request_receive_rate: int | None = None
    callback_request_transmit_rate: int | None = None
    callback_request_profile_error: str = ""
    callback_return_attempted: bool = False
    remote_retrain_nonce: str | None = None
    remote_retrain: bool = False
    retrain_connected: bool = False
    retrain_data: bool = False
    retrain_requested: bool = True
    duration_seconds: float = 0.0
    modulation: str | None = None
    protocol: str | None = None
    compression: str | None = None
    receive_rate: int | None = None
    transmit_rate: int | None = None
    profile_error: str = ""
    detail: str = ""
    artifact: str = ""
    upstream_mode: str | None = None
    # The initial profile above owns scoring. These independent observations
    # describe the CONNECT after local ATO1, not the remote retrain interval.
    retrain_modulation: str | None = None
    retrain_protocol: str | None = None
    retrain_compression: str | None = None
    retrain_receive_rate: int | None = None
    retrain_transmit_rate: int | None = None
    retrain_upstream_mode: str | None = None
    startup: str | None = None
    retrain_startup: str | None = None

    @property
    def profile_mismatch(self) -> bool:
        # Older recordings kept the mismatch in detail and stopped at CONNECT.
        return bool(self.profile_error) or self.detail.startswith("CONNECT selected ")

    @property
    def callback_request_profile_mismatch(self) -> bool:
        return bool(self.callback_request_profile_error) or self.detail.startswith(
            "callback request CONNECT selected "
        )

    @property
    def maturity(self) -> str | None:
        if self.outcome != OUTCOME_COMPLETE or self.callback_request_only:
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
        return self.outcome != OUTCOME_BUSY and not self.callback_request_only

    @property
    def callback_request_only(self) -> bool:
        return (
            self.role == "callback"
            and self.callback_request_attempted
            and not self.callback_return_attempted
        )

    def successful(self) -> bool:
        expected = MATURITY_FULL if self.retrain_requested else MATURITY_DATA
        return self.maturity == expected and not self.profile_mismatch


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
        least_complete_roles = {group[1] for group in least_complete_groups}
        if least_complete_roles == {ROLE_ORIGINATOR, ROLE_CALLBACK}:
            role = (
                ROLE_CALLBACK
                if generator.randrange(4) == 0
                else ROLE_ORIGINATOR
            )
            least_complete_groups = [
                group for group in least_complete_groups if group[1] == role
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


def _negotiated_group(
    modulation: str | None,
    protocol: str | None,
    requested: tuple[str, str],
    mismatch: bool,
) -> tuple[str, str]:
    # Legacy results without a mismatch may omit their validated profile.
    # Never credit a known mismatch to a guessed modulation or link protocol.
    family = next(
        (family for family, names in FAMILY_CONNECT_MODULATIONS.items()
         if modulation in names), None
    )
    return (
        family or (requested[0] if modulation is None and not mismatch else "?"),
        {"LAPM": LINK_LAPM, "none": LINK_RAW}.get(
            protocol, requested[1] if protocol is None and not mismatch else "?"
        ),
    )


def _retrain_profile_line(result: AttemptResult) -> str | None:
    fields = ("modulation", "protocol", "compression", "receive_rate",
              "transmit_rate", "upstream_mode")
    initial = tuple(getattr(result, name) for name in fields)
    retrained = tuple(getattr(result, "retrain_" + name) for name in fields)
    if (not result.retrain_connected or not any(value is not None for value in retrained)
            or retrained == initial):
        return None
    mode = {"LAPM": "l", "none": "r"}.get(result.retrain_protocol, "?")
    line = (
        f"     after ATO1: {(result.retrain_modulation or '?')[:7]} "
        f"{result.retrain_receive_rate or '-'}/{result.retrain_transmit_rate or '-'} {mode}"
    )
    upstream = result.retrain_upstream_mode
    if upstream in {"PCM", "V.34"} or result.upstream_mode in {"PCM", "V.34"}:
        line += f" up={upstream if upstream in {'PCM', 'V.34'} else '?'}"
    if result.retrain_compression != result.compression:
        line += f" compression={result.retrain_compression or '-'}"
    return line


def render_dashboard(
    cases: Sequence[CampaignCase],
    attempts: int,
    results: Sequence[AttemptResult],
    *,
    revision: str,
    campaign_id: str,
    elapsed_seconds: float,
    current: str = "",
    current_seconds: float | None = None,
    recent_limit: int = 0,
) -> str:
    roles = ("originator", "callback")
    cases_by_name = {case.name: case for case in cases}
    family_modes: dict[str, set[str]] = {}
    for case in cases:
        family_modes.setdefault(case.family, set()).add(case.link_mode)

    def result_link_mode(result: AttemptResult) -> str:
        if result.link_mode in LINK_MODES:
            return result.link_mode
        case = cases_by_name.get(result.case_name)
        if case is not None:
            return case.link_mode
        modes = family_modes.get(result.family, set())
        return next(iter(modes)) if len(modes) == 1 else LINK_RAW

    # Progress belongs to the scheduled profile. Each physical leg's result
    # belongs to the negotiated profile, independently of that progress.
    calls = []
    requests = []
    request_group = ("v22bis", LINK_LAPM)
    present = {(case.family, case.link_mode) for case in cases}
    for result in results:
        if result.counts_as_attempt:
            requested = (result.family, result_link_mode(result))
            actual = (
                _negotiated_group(
                    result.modulation, result.protocol, requested, result.profile_mismatch
                )
                if result.connected and result.maturity is not None
                else requested
            )
            calls.append((result, requested, actual))
            present.update((requested, actual))
        if result.callback_request_attempted:
            present.add(request_group)
            if result.outcome == OUTCOME_BUSY:
                continue
            actual = (
                _negotiated_group(
                    result.callback_request_modulation,
                    result.callback_request_protocol,
                    request_group, result.callback_request_profile_mismatch,
                )
                if result.callback_request_maturity in {
                    MATURITY_CONNECT, MATURITY_REQUEST
                }
                else request_group
            )
            requests.append((result, actual))
            present.add(actual)
    groups = [
        (family, link_mode) for family in FAMILY_ORDER for link_mode in LINK_MODES
        if (family, link_mode) in present
    ]
    groups.extend(sorted(present - set(groups)))

    base_expected = {
        (family, link_mode, role): sum(
            case.family == family
            and case.link_mode == link_mode
            and case.role == role
            for case in cases
        )
        * attempts
        for family, link_mode in groups
        for role in roles
    }
    rows: list[str] = []
    header = (
        f"revision={revision[:12]} campaign={campaign_id} elapsed={elapsed_seconds:.0f}s "
        f"retries={sum(result.outcome == OUTCOME_BUSY for result in results)}"
    )
    rows.append(header)
    if current:
        active = "" if current_seconds is None else f" for={current_seconds:.0f}s"
        rows.append(f"current={current}{active}")
    rows.append(
        "F=full R=retrain D=data C=connect Q=request N=no-connect E=error"
    )
    rows.append("r=raw l=LAPM; + includes failure extras and request legs")
    rows.append(
        "progress=requested; F/R/D/C/Q=negotiated; mismatch=missed requested profile"
    )
    role_columns = "done/exp+x   F  R  D  C  Q  N  E"
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
            MATURITY_REQUEST: 0,
            "error": 0,
        }
        for role in roles
    }

    def role_fields(family: str, link_mode: str, role: str) -> tuple[str, str]:
        role_results = [
            result
            for result, requested, _ in calls
            if requested == (family, link_mode)
            and result.role == role
        ]
        observed_results = [
            result for result, _, actual in calls
            if actual == (family, link_mode) and result.role == role
        ]
        counts = {
            maturity: sum(result.maturity == maturity for result in observed_results)
            for maturity in MATURITY_ORDER
        }
        mismatches = {
            maturity: sum(
                result.maturity == maturity and result.profile_mismatch
                for result in role_results
            )
            for maturity in MATURITY_ORDER[:4]
        }
        requested_requests = (
            [result for result, _ in requests]
            if (family, link_mode) == request_group and role == ROLE_ORIGINATOR
            else []
        )
        request_results = [
            result for result, actual in requests
            if actual == (family, link_mode) and role == ROLE_ORIGINATOR
        ]
        request_applicable = (
            ((family, link_mode) == request_group or bool(request_results))
            and role == ROLE_ORIGINATOR
        )
        request_observations = [
            result.callback_request_maturity
            for result in request_results
            if result.callback_request_maturity is not None
        ]
        for maturity in request_observations:
            if maturity in counts:
                counts[maturity] += 1
        mismatches[MATURITY_REQUEST] = 0
        for result in requested_requests:
            if (
                result.callback_request_profile_mismatch
                and result.callback_request_maturity in mismatches
            ):
                mismatches[result.callback_request_maturity] += 1
        request_count = sum(
            maturity == MATURITY_REQUEST for maturity in request_observations
        )
        errors = sum(result.outcome in ERROR_OUTCOMES for result in role_results)
        errors += sum(
            result.callback_request_only
            and result.callback_request_maturity is None
            and result.outcome in ERROR_OUTCOMES
            for result in requested_requests
        )
        done = sum(not result.extra for result in role_results)
        extras = sum(result.extra for result in role_results) + len(
            requested_requests
        )
        expected = base_expected[(family, link_mode, role)]
        total = totals[role]
        total["done"] += done
        total["expected"] += expected
        total["extra"] += extras
        total["error"] += errors
        total[MATURITY_REQUEST] += request_count
        for maturity in MATURITY_ORDER:
            total[maturity] += counts[maturity]
        request_cell = str(request_count) if request_applicable else ""
        fields = (
            f" {done:>3}/{expected:<3}+{extras:<2} "
            f"{counts[MATURITY_FULL]:>2} {counts[MATURITY_RETRAIN]:>2} "
            f"{counts[MATURITY_DATA]:>2} {counts[MATURITY_CONNECT]:>2} "
            f"{request_cell:>2} "
            f"{counts[MATURITY_NO_CONNECT]:>2} {errors:>2}"
        )
        mismatch_request_cell = str(mismatches[MATURITY_REQUEST]) if request_applicable else ""
        mismatch_fields = " " * 12 + " ".join(
            f"{mismatches[maturity]:>2}" for maturity in MATURITY_ORDER[:4]
        ) + f" {mismatch_request_cell:>2}" + " " * 6
        return fields, mismatch_fields

    for family, link_mode in groups:
        mode_label = {LINK_LAPM: "l", LINK_RAW: "r"}.get(link_mode, "?")
        label = f"{FAMILY_LABELS.get(family, family)}/{mode_label}"
        originator, originator_mismatches = role_fields(
            family, link_mode, ROLE_ORIGINATOR
        )
        callback, callback_mismatches = role_fields(
            family, link_mode, ROLE_CALLBACK
        )
        rows.append(
            f"{label:<12} {originator} | {callback}"
        )
        rows.append(
            f"{'  mismatch':<12} {originator_mismatches} | {callback_mismatches}"
        )

    def total_fields(role: str) -> str:
        total = totals[role]
        request_cell = (
            str(total[MATURITY_REQUEST])
            if role == ROLE_ORIGINATOR and ("v22bis", LINK_LAPM) in groups
            else ""
        )
        return (
            f" {total['done']:>3}/{total['expected']:<3}+{total['extra']:<2} "
            f"{total[MATURITY_FULL]:>2} {total[MATURITY_RETRAIN]:>2} "
            f"{total[MATURITY_DATA]:>2} {total[MATURITY_CONNECT]:>2} "
            f"{request_cell:>2} "
            f"{total[MATURITY_NO_CONNECT]:>2} {total['error']:>2}"
        )

    rows.append(
        f"{'TOTAL':<12} {total_fields('originator')} | "
        f"{total_fields('callback')}"
    )
    if recent_limit > 0 and results:
        rows.extend(
            (
                "Recent calls (newest first): o=originator c=callback q=request l=LAPM r=raw",
                "Q=request C=connect D=initial R=remote L=local P=post",
            )
        )
        history = (
            (index, request)
            for index in range(len(results) - 1, -1, -1)
            for request in (False, True)
            if (results[index].callback_request_attempted if request
                else not results[index].callback_request_only)
        )
        for index, request in history:
            result = results[index]
            case = cases_by_name.get(result.case_name)
            connected = result.connected
            initial_data = result.initial_data
            profile_mismatch = (
                result.callback_request_profile_mismatch if request
                else result.profile_mismatch
            )
            retrain_line = None if request else _retrain_profile_line(result)
            # Keep the initial, requested and retrained profiles together within the
            # available terminal rows; never leave an orphaned annotation.
            history_rows = 1 + int(profile_mismatch) + int(retrain_line is not None)
            if history_rows > recent_limit:
                break
            recent_limit -= history_rows
            modulation = (
                result.modulation if result.connected else None
            ) or ("?" if result.connected and profile_mismatch else
                  FAMILY_LABELS.get(result.family, result.family))
            role = "o" if result.role == "originator" else "c"
            if result.connected:
                mode = {"LAPM": "l", "none": "r"}.get(
                    result.protocol, "?"
                )
                receive_rate = result.receive_rate
                transmit_rate = result.transmit_rate
            else:
                mode = (
                    "l" if result_link_mode(result) == LINK_LAPM else "r"
                )
                receive_rate = case.receive_rate if case is not None else None
                transmit_rate = case.transmit_rate if case is not None else None
            if case is not None:
                symbol_rate = case.symbol_rate
            else:
                symbol_rate = None
            if request:
                role = "q"
                connected = result.callback_request_maturity in {
                    MATURITY_CONNECT, MATURITY_REQUEST
                }
                initial_data = result.callback_request_maturity == MATURITY_REQUEST
                # A successful legacy request validated this exact profile.
                # A failed legacy CONNECT may not have saved its actual one.
                known_profile = not connected or (initial_data and not profile_mismatch)
                modulation = result.callback_request_modulation or (
                    "V.22bis" if known_profile else "?"
                )
                mode = {"LAPM": "l", "none": "r"}.get(
                    result.callback_request_protocol,
                    "l" if known_profile else "?",
                )
                receive_rate = result.callback_request_receive_rate or (
                    2400 if known_profile else None
                )
                transmit_rate = result.callback_request_transmit_rate or (
                    2400 if known_profile else None
                )
                symbol_rate = 600
            if connected:
                # CONNECT reports do not include a measured symbol rate.
                symbol_rate = None
            rates = f"{receive_rate or '-'}/{transmit_rate or '-'}@{symbol_rate or '-'}"
            if request:
                request_status = (
                    result.callback_request_maturity or result.outcome
                )
                status = (
                    MATURITY_REQUEST if request_status == MATURITY_REQUEST
                    else f"request-{request_status}"
                )
            else:
                status = result.maturity or {
                    OUTCOME_INFRASTRUCTURE_ERROR: "infra-error",
                    OUTCOME_UNAVAILABLE: "unavailable",
                }.get(result.outcome, result.outcome)
            if profile_mismatch:
                status = f"{request_status if request else status}/mismatch"
            if not request and connected and result.upstream_mode in {"V.34", "PCM"}:
                status += f" up={result.upstream_mode}"
            sequence = f"{index + 1:03d}{'+' if result.extra else ' '}"
            request_cell = (
                str(int(result.callback_request_maturity == MATURITY_REQUEST))
                if request else " "
            )
            duration = "   -s" if request else f"{result.duration_seconds:>4.0f}s"
            rows.append(
                f"{sequence} {role} {modulation[:7]:<7} {rates:<17} {mode} "
                f"p{result.physical_try:<2} "
                f"Q{request_cell} "
                f"C{int(connected)} D{int(initial_data)} "
                f"R{int(not request and result.remote_retrain)} "
                f"L{int(not request and result.retrain_connected)} "
                f"P{int(not request and result.retrain_data)} "
                f"{duration} {status}"
            )
            if profile_mismatch:
                if request:
                    tried = "V.22bis 2400/2400@600 l (callback request)"
                elif case is None:
                    tried = "profile unavailable"
                else:
                    def rate_range(minimum: int | None, maximum: int | None) -> str:
                        return (
                            f"{minimum}-{maximum}"
                            if minimum is not None and minimum != maximum
                            else str(maximum or "-")
                        )

                    tried = (
                        f"{FAMILY_LABELS.get(case.family, case.family)} "
                        f"{rate_range(case.minimum_receive_rate, case.receive_rate)}/"
                        f"{rate_range(case.minimum_transmit_rate, case.transmit_rate)}"
                        f"@{case.symbol_rate or '-'} "
                        f"{'l' if case.link_mode == LINK_LAPM else 'r'}"
                    )
                    if case.feature == "analogue-fallback":
                        tried += " (analogue fallback)"
                rows.append(f"     tried: {tried}")
            if retrain_line is not None:
                rows.append(retrain_line)
    return "\n".join(rows)
