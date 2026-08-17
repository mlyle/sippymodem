#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
from dataclasses import replace
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.fast_interop import case_config, main, run_case  # noqa: E402
from tools.fast_matrix import (  # noqa: E402
    BASELINE_ORIGINATES,
    CANDIDATE_ORIGINATES,
    build_fast_matrix,
    payload_pair,
)
from tools.interop_config import (  # noqa: E402
    EndpointConfig,
    InteropConfig,
    InteropPorts,
)
from tools.live_terminal import ConnectReport  # noqa: E402
from tools.local_interop import EndpointState, InteropResult  # noqa: E402


def endpoint(name: str) -> EndpointConfig:
    return EndpointConfig(
        executable=Path(f"/{name}"),
        arguments=(name,),
        environment=(("SIP_LOG_LEVEL", "0"),),
        at_commands=("ATZ",),
    )


def base_config(directory: Path, *, enabled: bool = True) -> InteropConfig:
    return InteropConfig(
        enabled=enabled,
        artifact_directory=directory,
        originator=endpoint("candidate"),
        answerer=endpoint("baseline"),
        originator_payload=b"old-originator",
        answerer_payload=b"old-answerer",
        startup_timeout_seconds=1,
        setup_timeout_seconds=1,
        connect_timeout_seconds=1,
        online_timeout_seconds=7,
        cleanup_timeout_seconds=1,
        ports=InteropPorts(),
    )


def passed_result(case) -> InteropResult:
    candidate = EndpointState(
        connected=True,
        exchange_verified=True,
        connect_report=ConnectReport(
            receive_bit_rate=case.candidate_rates.receive,
            transmit_bit_rate=case.candidate_rates.transmit,
        ),
    )
    baseline = EndpointState(
        connected=True,
        exchange_verified=True,
        connect_report=ConnectReport(
            receive_bit_rate=case.baseline_rates.receive,
            transmit_bit_rate=case.baseline_rates.transmit,
        ),
    )
    if not case.candidate_originates:
        candidate, baseline = baseline, candidate
    return InteropResult(
        outcome="passed",
        duration_seconds=0.25,
        originator=candidate,
        answerer=baseline,
        originator_expected_bytes=1,
        answerer_expected_bytes=1,
    )


class FastInteropTests(unittest.TestCase):
    def test_case_config_uses_sip_clock_and_swaps_call_roles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = base_config(Path(temporary))
            cases = {
                case.originator_order: case
                for case in build_fast_matrix(("v23_1200_75",))
                if case.media_codec == "PCMU"
            }
            candidate_call = case_config(base, cases[CANDIDATE_ORIGINATES])
            baseline_call = case_config(base, cases[BASELINE_ORIGINATES])

            self.assertEqual(
                candidate_call.originator.executable, Path("/candidate")
            )
            self.assertEqual(
                baseline_call.originator.executable, Path("/baseline")
            )
            self.assertEqual(
                sorted(
                    (
                        candidate_call.online_timeout_seconds,
                        baseline_call.online_timeout_seconds,
                    )
                ),
                [5.266666666666667, 5.4],
            )
            for config in (candidate_call, baseline_call):
                for configured_endpoint in (config.originator, config.answerer):
                    environment = configured_endpoint.environment_dict()
                    self.assertEqual(environment["SIP_CODEC"], "PCMU")
                    self.assertEqual(environment["SIP_REMOTE_CLOCK"], "1")
                    self.assertEqual(environment["SIP_JB_MS"], "60")
                    self.assertIn(
                        "AT+MS=23,0,300,1200",
                        configured_endpoint.at_commands,
                    )
                    self.assertIn("AT\\N0", configured_endpoint.at_commands)

            payloads = payload_pair(cases[CANDIDATE_ORIGINATES], "initial")
            self.assertEqual(candidate_call.originator_payload, payloads.candidate)
            self.assertEqual(candidate_call.answerer_payload, payloads.baseline)
            payloads = payload_pair(cases[BASELINE_ORIGINATES], "initial")
            self.assertEqual(baseline_call.originator_payload, payloads.baseline)
            self.assertEqual(baseline_call.answerer_payload, payloads.candidate)

    def test_run_case_requires_data_and_both_reported_rates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = base_config(Path(temporary))
            case = build_fast_matrix(("v21_300",))[0]
            passed = run_case(
                base,
                case,
                runner=lambda _config, _turn_taking, _role, _payloads: (
                    passed_result(case)
                ),
            )
            self.assertEqual(passed.outcome, "passed")
            self.assertTrue(passed.candidate_rates_verified)
            self.assertTrue(passed.baseline_rates_verified)

            mismatch = passed_result(case)
            mismatch.answerer.connect_report = ConnectReport(
                receive_bit_rate=1200,
                transmit_bit_rate=1200,
            )
            failed = run_case(
                base,
                case,
                runner=lambda _config, _turn_taking, _role, _payloads: mismatch,
            )
            self.assertEqual(failed.outcome, "baseline-rate-mismatch")
            self.assertFalse(failed.baseline_rates_verified)

            data_error = passed_result(case)
            data_error = replace(data_error, outcome="data-error")
            failed = run_case(
                base,
                case,
                runner=lambda _config, _turn_taking, _role, _payloads: data_error,
            )
            self.assertEqual(failed.outcome, "data-error")

    def test_baudot_uses_turn_taking_exchange(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = base_config(Path(temporary))
            case = build_fast_matrix(("baudot_45",))[0]
            observed: list[bool] = []

            def runner(
                _config: InteropConfig,
                turn_taking: bool,
                _role: str | None,
                _payloads: tuple[bytes, bytes] | None,
            ) -> InteropResult:
                observed.append(turn_taking)
                return passed_result(case)

            self.assertEqual(
                run_case(base, case, runner=runner).outcome, "passed"
            )
            self.assertEqual(observed, [True])

    def test_ato1_targets_the_candidate_and_uses_fresh_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = base_config(Path(temporary))
            cases = {
                case.originator_order: case
                for case in build_fast_matrix(("v21_300",))
                if case.media_codec == "PCMU"
            }
            observed = []

            def runner(
                _config: InteropConfig,
                _turn_taking: bool,
                role: str | None,
                payloads: tuple[bytes, bytes] | None,
            ) -> InteropResult:
                observed.append((role, payloads))
                case = cases[
                    CANDIDATE_ORIGINATES
                    if role == "originator"
                    else BASELINE_ORIGINATES
                ]
                return replace(
                    passed_result(case),
                    ato1_requested=True,
                    ato1_connected=True,
                    post_ato1_exchange_verified=True,
                )

            for order in (CANDIDATE_ORIGINATES, BASELINE_ORIGINATES):
                result = run_case(base, cases[order], ato1=True, runner=runner)
                self.assertEqual(result.outcome, "passed")
                self.assertTrue(result.ato1_verified)

            self.assertEqual(
                [item[0] for item in observed], ["originator", "answerer"]
            )
            for order, (_, payloads) in zip(
                (CANDIDATE_ORIGINATES, BASELINE_ORIGINATES), observed
            ):
                assert payloads is not None
                initial = payload_pair(cases[order], "initial")
                self.assertNotIn(initial.candidate, payloads)
                self.assertNotIn(initial.baseline, payloads)

    def test_disabled_example_skips_without_exposing_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "matrix.json"
            payload = {
                "enabled": False,
                "artifact_directory": "artifacts",
                "originator": {
                    "executable": "/private/candidate",
                    "arguments": [],
                    "environment": {},
                    "at_commands": ["ATZ"],
                },
                "answerer": {
                    "executable": "/private/baseline",
                    "arguments": [],
                    "environment": {},
                    "at_commands": ["ATZ"],
                },
                "originator_payload_hex": "41",
                "answerer_payload_hex": "42",
            }
            path.write_text(json.dumps(payload), encoding="utf-8")
            self.assertEqual(main(["--config", str(path)]), 77)


if __name__ == "__main__":
    unittest.main()
