#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run the fast modem matrix through the existing loopback SIP path."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import json
from pathlib import Path
import subprocess
import sys
from typing import Callable, Sequence


if __package__:
    from .fast_matrix import (
        FAST_PROFILE_NAMES,
        EndpointRates,
        MatrixCase,
        TURN_TAKING,
        build_fast_matrix,
        payload_pair,
    )
    from .interop_config import EndpointConfig, InteropConfig, InteropConfigError
    from .interop_endpoint import InteropRunError
    from .live_profiles import scenario_commands
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
        EndpointRates,
        MatrixCase,
        TURN_TAKING,
        build_fast_matrix,
        payload_pair,
    )
    from interop_config import EndpointConfig, InteropConfig, InteropConfigError
    from interop_endpoint import InteropRunError
    from live_profiles import scenario_commands
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

ASYNC_BITS_PER_OCTET = 10
FAST_ONLINE_MINIMUM_SECONDS = 1.0
# Digital PCM calls may report CONNECT before their upstream transmitter has
# consumed fresh DTE bytes. Keep the evidence window long enough to observe
# that direction without restoring the ordinary live-call timeout.
FAST_PCM_ONLINE_MINIMUM_SECONDS = 5.0
FAST_ONLINE_MARGIN_SECONDS = 1.0
FAST_JITTER_BUFFER_MS = 60


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
            "outcome": self.outcome,
            "duration_seconds": round(self.duration_seconds, 3),
            "candidate_rates_verified": self.candidate_rates_verified,
            "baseline_rates_verified": self.baseline_rates_verified,
            "ato1_verified": self.ato1_verified,
        }


def _matrix_endpoint(
    endpoint: EndpointConfig, case: MatrixCase
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
    return replace(
        endpoint,
        environment=tuple(sorted(environment.items())),
        at_commands=scenario_commands(case.profile, "raw", (), ()),
    )


def _fast_online_timeout(
    base: InteropConfig,
    case: MatrixCase,
    originator_payload: bytes,
    answerer_payload: bytes,
) -> float:
    if case.candidate_originates:
        originator_rates = case.candidate_rates
        answerer_rates = case.baseline_rates
    else:
        originator_rates = case.baseline_rates
        answerer_rates = case.candidate_rates
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
        if case.profile.family in {"v90", "v92"}
        else FAST_ONLINE_MINIMUM_SECONDS
    )
    fast_timeout = max(
        minimum_seconds,
        wire_time + FAST_ONLINE_MARGIN_SECONDS,
    )
    return min(base.online_timeout_seconds, fast_timeout)


def case_config(base: InteropConfig, case: MatrixCase) -> InteropConfig:
    """Bind one public matrix case to a private pair of executables."""

    candidate = _matrix_endpoint(base.originator, case)
    baseline = _matrix_endpoint(base.answerer, case)
    payloads = payload_pair(case, "initial")
    if case.candidate_originates:
        originator, answerer = candidate, baseline
        originator_payload = payloads.candidate
        answerer_payload = payloads.baseline
    else:
        originator, answerer = baseline, candidate
        originator_payload = payloads.baseline
        answerer_payload = payloads.candidate
    return replace(
        base,
        artifact_directory=base.artifact_directory / case.name,
        originator=originator,
        answerer=answerer,
        originator_payload=originator_payload,
        answerer_payload=answerer_payload,
        online_timeout_seconds=_fast_online_timeout(
            base,
            case,
            originator_payload,
            answerer_payload,
        ),
    )


def _rates_match(result: InteropResult, case: MatrixCase) -> tuple[bool, bool]:
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

    return (
        matches(candidate, case.candidate_rates),
        matches(baseline, case.baseline_rates),
    )


def run_case(
    base: InteropConfig,
    case: MatrixCase,
    ato1: bool = False,
    runner: CaseRunner = _run_interop,
) -> FastCaseResult:
    configured = case_config(base, case)
    ato1_role = None
    post_ato1_payloads = None
    if ato1:
        ato1_role = "originator" if case.candidate_originates else "answerer"
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
    candidate_rates, baseline_rates = _rates_match(result, case)
    outcome = result.outcome
    if outcome == "passed" and not candidate_rates:
        outcome = "candidate-rate-mismatch"
    if outcome == "passed" and not baseline_rates:
        outcome = "baseline-rate-mismatch"
    return FastCaseResult(
        case=case,
        outcome=outcome,
        duration_seconds=result.duration_seconds,
        candidate_rates_verified=candidate_rates,
        baseline_rates_verified=baseline_rates,
        ato1_verified=(
            result.ato1_connected and result.post_ato1_exchange_verified
            if ato1
            else None
        ),
    )


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
    parser.add_argument("--stop-on-failure", action="store_true")
    parser.add_argument(
        "--ato1",
        action="store_true",
        help="return the candidate online with ATO1 and repeat the exchange",
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    options = _parser().parse_args(arguments)
    try:
        config = load_interop_config(options.config)
        profiles = tuple(options.profile) or FAST_PROFILE_NAMES
        cases = build_fast_matrix(profiles)
    except (InteropConfigError, KeyError, ValueError):
        print(json.dumps({"schema": 1, "outcome": "configuration-error"}))
        return 2
    if not config.enabled:
        print(json.dumps({"schema": 1, "outcome": "skipped"}))
        return SKIPPED_EXIT_CODE

    failed = False
    try:
        for case in cases:
            result = run_case(config, case, ato1=options.ato1)
            print(json.dumps(result.public_dict(), sort_keys=True), flush=True)
            if result.outcome != "passed":
                failed = True
                if options.stop_on_failure:
                    break
    except (
        InteropRunError,
        LiveTerminalError,
        OSError,
        subprocess.SubprocessError,
        TimeoutError,
    ):
        print(json.dumps({"schema": 1, "outcome": "infrastructure-error"}))
        return 1
    except KeyboardInterrupt:
        return 130
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
