#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run the fast modem matrix through the existing loopback SIP path."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
import json
from pathlib import Path
import subprocess
import sys
import time
from typing import Callable, Iterator, Sequence


if __package__:
    from .fast_matrix import (
        FAST_PROFILE_NAMES,
        MEDIA_CODECS,
        EndpointRates,
        MatrixCase,
        TURN_TAKING,
        build_fast_matrix,
        payload_pair,
    )
    from .interop_config import (
        EndpointConfig,
        InteropConfig,
        InteropConfigError,
        InteropPorts,
    )
    from .interop_endpoint import (
        InteropRunError,
        reserve_loopback_port_sets,
    )
    from .live_profiles import (
        PROFILE_GROUPS,
        scenario_commands,
        select_profiles,
    )
    from .live_terminal import ConnectReport, LiveTerminalError
    from .local_interop import (
        InteropResult,
        SKIPPED_EXIT_CODE,
        load_interop_config,
        run_interop,
    )
else:
    from fast_matrix import (
        FAST_PROFILE_NAMES,
        MEDIA_CODECS,
        EndpointRates,
        MatrixCase,
        TURN_TAKING,
        build_fast_matrix,
        payload_pair,
    )
    from interop_config import (
        EndpointConfig,
        InteropConfig,
        InteropConfigError,
        InteropPorts,
    )
    from interop_endpoint import (
        InteropRunError,
        reserve_loopback_port_sets,
    )
    from live_profiles import (
        PROFILE_GROUPS,
        scenario_commands,
        select_profiles,
    )
    from live_terminal import ConnectReport, LiveTerminalError
    from local_interop import (
        InteropResult,
        SKIPPED_EXIT_CODE,
        load_interop_config,
        run_interop,
    )


CaseRunner = Callable[
    [InteropConfig, bool, str | None, tuple[bytes, bytes] | None],
    InteropResult,
]
PortSetReserver = Callable[[int], tuple[InteropPorts, ...]]

ASYNC_BITS_PER_OCTET = 10
FAST_ONLINE_MINIMUM_SECONDS = 1.0
# Digital PCM calls may report CONNECT before their upstream transmitter has
# consumed fresh DTE bytes. Keep the evidence window long enough to observe
# that direction without restoring the ordinary live-call timeout.
FAST_PCM_ONLINE_MINIMUM_SECONDS = 5.0
FAST_ONLINE_MARGIN_SECONDS = 1.0
FAST_JITTER_BUFFER_MS = 60
ATO1_ENDPOINTS = ("candidate", "baseline")
PCM_FAMILIES = frozenset(("v90", "v92"))
PCM_ROLE_BY_CALL = "by-call"
PCM_ROLES = (PCM_ROLE_BY_CALL, "analog", "digital")
MAX_PARALLEL_CASES = 8
INTEROP_EXCEPTIONS = (
    InteropRunError,
    LiveTerminalError,
    OSError,
    subprocess.SubprocessError,
    TimeoutError,
)


def _run_interop(
    config: InteropConfig,
    turn_taking: bool,
    ato1_role: str | None,
    post_ato1_payloads: tuple[bytes, bytes] | None,
) -> InteropResult:
    return run_interop(
        config,
        turn_taking=turn_taking,
        ato1_role=ato1_role,
        post_ato1_payloads=post_ato1_payloads,
        escape_guard_seconds=1.1,
    )


@dataclass(frozen=True)
class FastCaseResult:
    case: MatrixCase
    link_mode: str
    candidate_pcm_role: str | None
    ato1_endpoint: str | None
    outcome: str
    duration_seconds: float
    candidate_rates_verified: bool
    baseline_rates_verified: bool
    ato1_verified: bool | None

    def public_dict(self) -> dict[str, object]:
        return {
            "schema": 1,
            "case": self.case.name,
            "profile": self.case.profile.name,
            "family": self.case.profile.family,
            "media_codec": self.case.media_codec,
            "candidate_originates": self.case.candidate_originates,
            "link_mode": self.link_mode,
            "candidate_pcm_role": self.candidate_pcm_role,
            "ato1_endpoint": self.ato1_endpoint,
            "outcome": self.outcome,
            "duration_seconds": round(self.duration_seconds, 3),
            "candidate_rates_verified": self.candidate_rates_verified,
            "baseline_rates_verified": self.baseline_rates_verified,
            "ato1_verified": self.ato1_verified,
        }


def _matrix_endpoint(
    endpoint: EndpointConfig,
    case: MatrixCase,
    *,
    pcm_digital: bool,
    link_mode: str,
) -> EndpointConfig:
    environment = endpoint.environment_dict()
    environment.update(
        {
            "SIP_CODEC": case.media_codec,
            "SIP_FRAME_MS": "10",
            # Keep the normal three-frame reserve: a shallow fast-test buffer
            # can conceal an arrived frame when the two local processes are
            # scheduled unevenly, turning host jitter into a modem failure.
            "SIP_JB_MS": str(FAST_JITTER_BUFFER_MS),
            "SIP_REMOTE_CLOCK": "1",
        }
    )
    role_commands = (
        (f"AT+V90D={int(pcm_digital)}",)
        if case.profile.family in PCM_FAMILIES
        else ()
    )
    return replace(
        endpoint,
        environment=tuple(sorted(environment.items())),
        at_commands=scenario_commands(
            case.profile, link_mode, (), role_commands
        ),
    )


def _fast_online_timeout(
    base: InteropConfig,
    case: MatrixCase,
    candidate_rates: EndpointRates,
    baseline_rates: EndpointRates,
    originator_payload: bytes,
    answerer_payload: bytes,
) -> float:
    if case.candidate_originates:
        originator_rates = candidate_rates
        answerer_rates = baseline_rates
    else:
        originator_rates = baseline_rates
        answerer_rates = candidate_rates
    direction_times = (
        len(originator_payload)
        * ASYNC_BITS_PER_OCTET
        / originator_rates.transmit,
        len(answerer_payload)
        * ASYNC_BITS_PER_OCTET
        / answerer_rates.transmit,
    )
    wire_time = (
        sum(direction_times)
        if case.exchange == TURN_TAKING
        else max(direction_times)
    )
    minimum_seconds = (
        FAST_PCM_ONLINE_MINIMUM_SECONDS
        if case.profile.family in PCM_FAMILIES
        else FAST_ONLINE_MINIMUM_SECONDS
    )
    fast_timeout = max(
        minimum_seconds,
        wire_time + FAST_ONLINE_MARGIN_SECONDS,
    )
    return min(base.online_timeout_seconds, fast_timeout)


def _pcm_roles(
    case: MatrixCase, candidate_pcm_role: str
) -> tuple[str | None, str | None]:
    if candidate_pcm_role not in PCM_ROLES:
        raise ValueError("invalid candidate PCM role")
    if case.profile.family not in PCM_FAMILIES:
        return None, None
    selected = candidate_pcm_role
    if selected == PCM_ROLE_BY_CALL:
        selected = "analog" if case.candidate_originates else "digital"
    baseline = "analog" if selected == "digital" else "digital"
    return selected, baseline


def _selected_rates(
    case: MatrixCase, candidate_pcm_role: str | None
) -> tuple[EndpointRates, EndpointRates]:
    if candidate_pcm_role is None:
        return case.candidate_rates, case.baseline_rates
    receive_rate = case.profile.receive_rate
    transmit_rate = case.profile.transmit_rate
    if receive_rate is None or transmit_rate is None:
        raise ValueError("PCM profiles need exact directional rates")
    analog_rates = EndpointRates(receive_rate, transmit_rate)
    digital_rates = EndpointRates(transmit_rate, receive_rate)
    if candidate_pcm_role == "digital":
        return digital_rates, analog_rates
    return analog_rates, digital_rates


def case_config(
    base: InteropConfig,
    case: MatrixCase,
    link_mode: str = "raw",
    candidate_pcm_role: str = PCM_ROLE_BY_CALL,
) -> InteropConfig:
    """Bind one public matrix case to a private pair of executables."""

    candidate_role, baseline_role = _pcm_roles(case, candidate_pcm_role)
    candidate_rates, baseline_rates = _selected_rates(case, candidate_role)
    candidate = _matrix_endpoint(
        base.originator,
        case,
        pcm_digital=candidate_role == "digital",
        link_mode=link_mode,
    )
    baseline = _matrix_endpoint(
        base.answerer,
        case,
        pcm_digital=baseline_role == "digital",
        link_mode=link_mode,
    )
    payloads = payload_pair(case, "initial")
    if case.candidate_originates:
        originator, answerer = candidate, baseline
        originator_payload = payloads.candidate
        answerer_payload = payloads.baseline
    else:
        originator, answerer = baseline, candidate
        originator_payload = payloads.baseline
        answerer_payload = payloads.candidate
    artifact_name = (
        case.name if link_mode == "raw" else f"{case.name}-{link_mode}"
    )
    if candidate_pcm_role != PCM_ROLE_BY_CALL and candidate_role is not None:
        artifact_name = f"{artifact_name}-candidate-{candidate_role}"
    return replace(
        base,
        artifact_directory=base.artifact_directory / artifact_name,
        originator=originator,
        answerer=answerer,
        originator_payload=originator_payload,
        answerer_payload=answerer_payload,
        online_timeout_seconds=_fast_online_timeout(
            base,
            case,
            candidate_rates,
            baseline_rates,
            originator_payload,
            answerer_payload,
        ),
    )


def _rates_match(
    result: InteropResult,
    case: MatrixCase,
    candidate_pcm_role: str | None,
) -> tuple[bool, bool]:
    if case.candidate_originates:
        candidate = result.originator.connect_report
        baseline = result.answerer.connect_report
    else:
        candidate = result.answerer.connect_report
        baseline = result.originator.connect_report

    def matches(report: ConnectReport, rates: EndpointRates) -> bool:
        return (
            report.receive_bit_rate == rates.receive
            and report.transmit_bit_rate == rates.transmit
        )

    candidate_rates, baseline_rates = _selected_rates(
        case, candidate_pcm_role
    )
    return matches(candidate, candidate_rates), matches(baseline, baseline_rates)


def run_case(
    base: InteropConfig,
    case: MatrixCase,
    ato1_endpoint: str | None = None,
    link_mode: str = "raw",
    candidate_pcm_role: str = PCM_ROLE_BY_CALL,
    runner: CaseRunner = _run_interop,
) -> FastCaseResult:
    if ato1_endpoint not in (None, *ATO1_ENDPOINTS):
        raise ValueError("invalid ATO1 endpoint")
    selected_pcm_role, _ = _pcm_roles(case, candidate_pcm_role)
    configured = case_config(
        base, case, link_mode, candidate_pcm_role
    )
    ato1_role = None
    post_ato1_payloads = None
    if ato1_endpoint is not None:
        endpoint_originates = (
            case.candidate_originates
            if ato1_endpoint == "candidate"
            else not case.candidate_originates
        )
        ato1_role = "originator" if endpoint_originates else "answerer"
        payloads = payload_pair(case, "ato1")
        post_ato1_payloads = (
            (payloads.candidate, payloads.baseline)
            if case.candidate_originates
            else (payloads.baseline, payloads.candidate)
        )
    result = runner(
        configured,
        case.exchange == TURN_TAKING,
        ato1_role,
        post_ato1_payloads,
    )
    candidate_rates, baseline_rates = _rates_match(
        result, case, selected_pcm_role
    )
    outcome = result.outcome
    if outcome == "passed" and not candidate_rates:
        outcome = "candidate-rate-mismatch"
    if outcome == "passed" and not baseline_rates:
        outcome = "baseline-rate-mismatch"
    return FastCaseResult(
        case=case,
        link_mode=link_mode,
        candidate_pcm_role=selected_pcm_role,
        ato1_endpoint=ato1_endpoint,
        outcome=outcome,
        duration_seconds=result.duration_seconds,
        candidate_rates_verified=candidate_rates,
        baseline_rates_verified=baseline_rates,
        ato1_verified=(
            result.ato1_connected and result.post_ato1_exchange_verified
            if ato1_endpoint is not None
            else None
        ),
    )


def _infrastructure_result(
    case: MatrixCase,
    ato1_endpoint: str | None,
    link_mode: str,
    candidate_pcm_role: str,
    duration_seconds: float,
) -> FastCaseResult:
    selected_pcm_role, _ = _pcm_roles(case, candidate_pcm_role)
    return FastCaseResult(
        case=case,
        link_mode=link_mode,
        candidate_pcm_role=selected_pcm_role,
        ato1_endpoint=ato1_endpoint,
        outcome="infrastructure-error",
        duration_seconds=duration_seconds,
        candidate_rates_verified=False,
        baseline_rates_verified=False,
        ato1_verified=False if ato1_endpoint is not None else None,
    )


def _run_case_safely(
    base: InteropConfig,
    case: MatrixCase,
    ato1_endpoint: str | None,
    link_mode: str,
    candidate_pcm_role: str,
    runner: CaseRunner,
) -> FastCaseResult:
    started = time.monotonic()
    try:
        return run_case(
            base,
            case,
            ato1_endpoint=ato1_endpoint,
            link_mode=link_mode,
            candidate_pcm_role=candidate_pcm_role,
            runner=runner,
        )
    except INTEROP_EXCEPTIONS:
        return _infrastructure_result(
            case,
            ato1_endpoint,
            link_mode,
            candidate_pcm_role,
            time.monotonic() - started,
        )


def run_case_waves(
    base: InteropConfig,
    cases: Sequence[MatrixCase],
    *,
    jobs: int = 1,
    stop_on_failure: bool = False,
    ato1_endpoint: str | None = None,
    link_mode: str = "raw",
    candidate_pcm_role: str = PCM_ROLE_BY_CALL,
    runner: CaseRunner = _run_interop,
    port_set_reserver: PortSetReserver = reserve_loopback_port_sets,
) -> Iterator[tuple[FastCaseResult, ...]]:
    """Run bounded waves and yield each wave in matrix order."""

    if not 1 <= jobs <= MAX_PARALLEL_CASES:
        raise ValueError("parallel case count is out of range")
    for offset in range(0, len(cases), jobs):
        wave = tuple(cases[offset : offset + jobs])
        bases = (base,) * len(wave)
        if jobs > 1:
            port_sets = port_set_reserver(len(wave))
            numbers = tuple(
                port for ports in port_sets for port in ports.values()
            )
            if (
                len(port_sets) != len(wave)
                or not all(numbers)
                or len(set(numbers)) != len(numbers)
            ):
                raise InteropRunError("parallel loopback ports are not disjoint")
            bases = tuple(
                replace(base, ports=ports) for ports in port_sets
            )

        if len(wave) == 1:
            results = (
                _run_case_safely(
                    bases[0],
                    wave[0],
                    ato1_endpoint,
                    link_mode,
                    candidate_pcm_role,
                    runner,
                ),
            )
        else:
            with ThreadPoolExecutor(max_workers=len(wave)) as executor:
                futures = tuple(
                    executor.submit(
                        _run_case_safely,
                        configured,
                        case,
                        ato1_endpoint,
                        link_mode,
                        candidate_pcm_role,
                        runner,
                    )
                    for configured, case in zip(bases, wave)
                )
                results = tuple(future.result() for future in futures)

        yield results
        if any(
            result.outcome == "infrastructure-error"
            or (stop_on_failure and result.outcome != "passed")
            for result in results
        ):
            break


def _parallelism(value: str) -> int:
    try:
        jobs = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("jobs must be an integer") from error
    if not 1 <= jobs <= MAX_PARALLEL_CASES:
        raise argparse.ArgumentTypeError(
            f"jobs must be between 1 and {MAX_PARALLEL_CASES}"
        )
    return jobs


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run a private modem pair through the fast loopback SIP matrix. "
            "The config originator is the candidate and the answerer is the "
            "comparison endpoint; the runner exercises both call roles."
        )
    )
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument(
        "--profile",
        action="append",
        default=[],
        help="run one public profile; may be repeated",
    )
    parser.add_argument(
        "--group",
        action="append",
        choices=tuple(PROFILE_GROUPS),
        default=[],
        help="run one public profile group; may be repeated",
    )
    parser.add_argument("--stop-on-failure", action="store_true")
    parser.add_argument(
        "--jobs",
        type=_parallelism,
        default=1,
        help=(
            "run this many isolated calls at once; parallel waves use "
            "dynamically selected ports (default: 1, maximum: 8)"
        ),
    )
    parser.add_argument(
        "--media-codec",
        action="append",
        choices=MEDIA_CODECS,
        default=[],
        help="run only this G.711 law; may be repeated",
    )
    parser.add_argument(
        "--link-mode",
        choices=("raw", "lapm"),
        default="raw",
        help="select raw or LAPM host framing (default: raw)",
    )
    parser.add_argument(
        "--ato1",
        nargs="?",
        choices=ATO1_ENDPOINTS,
        const="candidate",
        help=(
            "return the selected endpoint online with ATO1 and repeat the "
            "exchange (default endpoint: candidate)"
        ),
    )
    parser.add_argument(
        "--candidate-pcm-role",
        choices=PCM_ROLES,
        default=PCM_ROLE_BY_CALL,
        help=(
            "select the candidate's V.90/V.92 line role independently of "
            "call direction (default: analogue originator, digital answerer)"
        ),
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    options = _parser().parse_args(arguments)
    try:
        config = load_interop_config(options.config)
        selected = select_profiles(
            tuple(options.profile), tuple(options.group)
        )
        profiles = tuple(profile.name for profile in selected)
        profiles = profiles or FAST_PROFILE_NAMES
        cases = build_fast_matrix(profiles)
        if options.media_codec:
            selected_codecs = frozenset(options.media_codec)
            cases = tuple(
                case for case in cases if case.media_codec in selected_codecs
            )
    except (InteropConfigError, KeyError, ValueError):
        print(json.dumps({"schema": 1, "outcome": "configuration-error"}))
        return 2
    if not config.enabled:
        print(json.dumps({"schema": 1, "outcome": "skipped"}))
        return SKIPPED_EXIT_CODE

    failed = False
    try:
        for wave in run_case_waves(
            config,
            cases,
            jobs=options.jobs,
            stop_on_failure=options.stop_on_failure,
            ato1_endpoint=options.ato1,
            link_mode=options.link_mode,
            candidate_pcm_role=options.candidate_pcm_role,
        ):
            print(
                "\n".join(
                    json.dumps(result.public_dict(), sort_keys=True)
                    for result in wave
                ),
                flush=True,
            )
            if any(result.outcome != "passed" for result in wave):
                failed = True
    except INTEROP_EXCEPTIONS:
        print(json.dumps({"schema": 1, "outcome": "infrastructure-error"}))
        return 1
    except KeyboardInterrupt:
        return 130
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
