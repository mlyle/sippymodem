#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_calls import (  # noqa: E402
    CallResult,
    LiveCallAttempt,
    LiveCallRunError,
    build_child_environment,
    main,
    selected_scenarios,
)
from tools.live_config import (  # noqa: E402
    CallScenario,
    LiveCallConfig,
    ScenarioPlan,
)


class LiveCallRunnerTests(unittest.TestCase):
    def plan(
        self,
        name: str,
        profiles: tuple[str, ...],
        *,
        enabled: bool = True,
        link_modes: tuple[str, ...] = ("lapm",),
        attempts: int = 1,
    ) -> ScenarioPlan:
        return ScenarioPlan(
            name=name,
            enabled=enabled,
            profiles=profiles,
            profile_groups=(),
            link_modes=link_modes,
            attempts=attempts,
            at_commands=(),
        )

    def config(
        self,
        directory: str,
        plans: tuple[ScenarioPlan, ...],
        executable: str = "fake-slmodemd",
    ) -> LiveCallConfig:
        root = Path(directory)
        return LiveCallConfig(
            slmodemd=root / executable,
            artifact_directory=root / "artifacts",
            environment=(
                ("SIP_DOMAIN", "private.example"),
                ("SIP_TARGET", "private-target"),
            ),
            at_commands=(),
            startup_timeout_seconds=1,
            call_timeout_seconds=1,
            online_timeout_seconds=0.25,
            hold_seconds=0,
            inter_scenario_seconds=0,
            plans=plans,
        )

    def scenario(
        self,
        name: str,
        *,
        online_expect: bytes = b"",
        round_trip_probe: str = "",
    ) -> CallScenario:
        return CallScenario(
            name=name,
            plan_name="test-plan",
            profile_name="v21_300",
            family="v21",
            link_mode="lapm",
            attempt=1,
            readiness="runnable",
            prerequisite="",
            at_commands=("ATE0", "ATS95=7"),
            online_expect=online_expect,
            round_trip_probe=round_trip_probe,
        )

    def result(self, scenario: CallScenario, outcome: str) -> CallResult:
        return CallResult(
            scenario_name=scenario.name,
            plan_name=scenario.plan_name,
            profile_name=scenario.profile_name,
            family=scenario.family,
            link_mode=scenario.link_mode,
            attempt=scenario.attempt,
            readiness=scenario.readiness,
            prerequisite=scenario.prerequisite or None,
            outcome=outcome,
            duration_seconds=0.125,
        )

    def test_child_environment_has_one_explicit_private_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = self.config(directory, (self.plan("first", ("v21_300",)),))
            environment = build_child_environment(
                config,
                Path(directory) / "recording",
                {
                    "PATH": "/bin",
                    "SIP_PASSWORD": "inherited-secret",
                    "SIP_TARGET": "inherited-target",
                    "SIP_PORT": "5060",
                    "SIP_RECORD_PREFIX": "inherited-recording",
                },
            )

            self.assertEqual(environment["PATH"], "/bin")
            self.assertNotIn("SIP_PASSWORD", environment)
            self.assertEqual(environment["SIP_TARGET"], "private-target")
            self.assertEqual(environment["SIP_PORT"], "0")
            self.assertEqual(environment["SIP_RTP_PORT"], "0")
            self.assertTrue(environment["SIP_RECORD_PREFIX"].endswith("recording"))

    def test_tracked_example_skips_without_starting_a_call(self) -> None:
        example = Path(__file__).resolve().parents[1] / "tools/live-call.example.json"
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            exit_code = main(("--config", str(example)))

        self.assertEqual(exit_code, 77)
        self.assertIn("no scenario plans are enabled", errors.getvalue())

    def test_plan_selection_expands_enabled_unique_scenarios(self) -> None:
        plans = (
            self.plan("first", ("v21_300",), link_modes=("lapm", "raw")),
            self.plan("second", ("v32_4800",), enabled=False),
            self.plan("repeat", ("v32_9600",), attempts=2),
        )
        with tempfile.TemporaryDirectory() as directory:
            config = self.config(directory, plans)

            self.assertEqual(
                [scenario.name for scenario in selected_scenarios(config, ())],
                [
                    "first.v21_300.lapm.1",
                    "first.v21_300.raw.1",
                    "repeat.v32_9600.lapm.1",
                    "repeat.v32_9600.lapm.2",
                ],
            )
            self.assertEqual(
                [scenario.name for scenario in selected_scenarios(config, ("repeat",))],
                ["repeat.v32_9600.lapm.1", "repeat.v32_9600.lapm.2"],
            )
            for names in (("second",), ("missing",), ("first", "first")):
                with self.subTest(names=names):
                    with self.assertRaises(LiveCallRunError):
                        selected_scenarios(config, names)

    def test_call_result_contains_no_endpoint_or_artifact_fields(self) -> None:
        scenario = self.scenario("named.v21_300.lapm.1")
        result = self.result(scenario, "busy")
        rendered = json.dumps(result.public_dict(), sort_keys=True)

        self.assertEqual(
            set(result.public_dict()),
            {
                "scenario",
                "plan",
                "profile",
                "family",
                "link_mode",
                "attempt",
                "readiness",
                "prerequisite",
                "outcome",
                "duration_seconds",
                "terminal_expected_bytes",
                "terminal_verified_bytes",
                "terminal_transmitted_bytes",
                "round_trip_probe",
                "round_trip_verified",
                "modulation",
                "protocol",
                "compression",
                "receive_bit_rate",
                "transmit_bit_rate",
            },
        )
        self.assertNotIn("target", rendered)
        self.assertNotIn("environment", rendered)
        self.assertNotIn("artifact", rendered)

    def test_fake_modem_call_is_bounded_and_private(self) -> None:
        script_text = """#!/usr/bin/env python3
import os
import pty
import tty

master, slave = pty.openpty()
tty.setraw(slave)
print(os.ttyname(slave), flush=True)
pending = b""
while True:
    pending += os.read(master, 4096)
    while b"\\r" in pending:
        command, pending = pending.split(b"\\r", 1)
        if command == b"ATD":
            os.write(master, b"\\r\\nBUSY\\r\\n")
        else:
            os.write(master, b"\\r\\nOK\\r\\n")
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-slmodemd"
            executable.write_text(script_text, encoding="utf-8")
            executable.chmod(0o700)
            scenario = self.scenario("busy-case.v21_300.lapm.1")
            config = self.config(directory, (self.plan("busy-case", ("v21_300",)),))

            result = LiveCallAttempt(config, scenario).run()

            self.assertEqual(result.outcome, "busy")
            attempts = list(config.artifact_directory.iterdir())
            self.assertEqual(len(attempts), 1)
            attempt = attempts[0]
            self.assertEqual(stat.S_IMODE(attempt.stat().st_mode), 0o700)
            self.assertEqual(
                stat.S_IMODE((attempt / "daemon.log").stat().st_mode), 0o600
            )
            self.assertEqual(
                stat.S_IMODE((attempt / "result.json").stat().st_mode), 0o600
            )
            recorded = json.loads((attempt / "result.json").read_text())
            self.assertEqual(recorded["outcome"], "busy")
            self.assertNotIn("private-target", json.dumps(recorded))
            self.assertNotIn("private.example", json.dumps(recorded))

    def test_fake_modem_preserves_and_verifies_online_data(self) -> None:
        script_text = """#!/usr/bin/env python3
import os
import pty
import tty

master, slave = pty.openpty()
tty.setraw(slave)
print(os.ttyname(slave), flush=True)
pending = b""
details_enabled = False
online = False
while True:
    pending += os.read(master, 4096)
    if online and b"\\r\\n\\r\\nPING\\r\\n" in pending:
        _before, pending = pending.split(b"\\r\\n\\r\\nPING\\r\\n", 1)
        os.write(
            master,
            b"GETTY:PONG:0123456789abcdef0123456789abcdef:1783737600123\\r\\n",
        )
    if online and b"+++" in pending:
        _before, pending = pending.split(b"+++", 1)
        online = False
        os.write(master, b"\\r\\nOK\\r\\n")
    if online:
        continue
    while b"\\r" in pending:
        command, pending = pending.split(b"\\r", 1)
        if command == b"ATS95=7":
            details_enabled = True
            os.write(master, b"\\r\\nOK\\r\\n")
        elif command == b"ATD" and details_enabled:
            os.write(
                master,
                b"\\r\\nModulation: V32bis\\r\\nProtocol: LAPM\\r\\n"
                b"Compression: V44\\r\\nTxRate: 12000\\r\\n"
                b"CONNECT 14400\\r\\nMODEM-LIVE-OK\\r\\n",
            )
            online = True
        elif command == b"ATD":
            os.write(master, b"\\r\\nERROR\\r\\n")
        else:
            os.write(master, b"\\r\\nOK\\r\\n")
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-slmodemd"
            executable.write_text(script_text, encoding="utf-8")
            executable.chmod(0o700)
            scenario = self.scenario(
                "data-case.v21_300.lapm.1",
                online_expect=b"MODEM-LIVE-OK\r\n",
                round_trip_probe="ping",
            )
            config = self.config(directory, (self.plan("data-case", ("v21_300",)),))

            result = LiveCallAttempt(config, scenario).run()

            self.assertEqual(result.outcome, "connected")
            self.assertEqual(result.terminal_expected_bytes, 15)
            self.assertEqual(result.terminal_verified_bytes, 15)
            self.assertEqual(result.terminal_transmitted_bytes, 10)
            self.assertTrue(result.round_trip_verified)
            self.assertEqual(result.modulation, "V.32bis")
            self.assertEqual(result.protocol, "LAPM")
            self.assertEqual(result.compression, "V.44")
            self.assertEqual(result.receive_bit_rate, 14400)
            self.assertEqual(result.transmit_bit_rate, 12000)
            attempt = next(config.artifact_directory.iterdir())
            received = attempt / "terminal-rx.bin"
            transmitted = attempt / "terminal-tx.bin"
            self.assertEqual(
                received.read_bytes(),
                b"MODEM-LIVE-OK\r\n"
                b"GETTY:PONG:0123456789abcdef0123456789abcdef:"
                b"1783737600123\r\n",
            )
            self.assertEqual(transmitted.read_bytes(), b"\r\n\r\nPING\r\n")
            self.assertEqual(stat.S_IMODE(received.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(transmitted.stat().st_mode), 0o600)

    def test_runner_reports_later_scenario_after_failure(self) -> None:
        document = {
            "slmodemd": "fake-slmodemd",
            "environment": {"SIP_TARGET": "private-target"},
            "plans": [
                {
                    "name": "matrix",
                    "profiles": ["v21_300", "v32_4800"],
                    "enabled": True,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-slmodemd"
            executable.write_text("#!/bin/sh\n", encoding="utf-8")
            executable.chmod(0o700)
            path = Path(directory) / ".live-call.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            path.chmod(0o600)
            scenarios = (
                CallScenario(
                    name="matrix.v21_300.lapm.1",
                    plan_name="matrix",
                    profile_name="v21_300",
                    family="v21",
                    link_mode="lapm",
                    attempt=1,
                    readiness="runnable",
                    prerequisite="",
                    at_commands=(),
                ),
                CallScenario(
                    name="matrix.v32_4800.lapm.1",
                    plan_name="matrix",
                    profile_name="v32_4800",
                    family="v32",
                    link_mode="lapm",
                    attempt=1,
                    readiness="runnable",
                    prerequisite="",
                    at_commands=(),
                ),
            )
            results = (
                self.result(scenarios[0], "busy"),
                self.result(scenarios[1], "connected"),
            )
            output = io.StringIO()
            with (
                mock.patch("tools.live_calls.LiveCallAttempt") as attempt,
                contextlib.redirect_stdout(output),
            ):
                attempt.return_value.run.side_effect = results
                exit_code = main(("--config", str(path)))

            self.assertEqual(exit_code, 1)
            self.assertEqual(attempt.call_count, 2)
            rendered = output.getvalue()
            self.assertIn("matrix.v21_300.lapm.1", rendered)
            self.assertIn("matrix.v32_4800.lapm.1", rendered)
            self.assertNotIn("private-target", rendered)

    def test_prerequisite_profiles_report_without_starting_modem(self) -> None:
        document = {
            "environment": {"SIP_TARGET": "private-target"},
            "plans": [
                {
                    "name": "future",
                    "profiles": ["v34_rx_4800_b2800", "v90_down_28000"],
                    "enabled": True,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".live-call.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            path.chmod(0o600)
            output = io.StringIO()
            with (
                mock.patch("tools.live_calls.LiveCallAttempt") as attempt,
                contextlib.redirect_stdout(output),
            ):
                exit_code = main(("--config", str(path)))

            self.assertEqual(exit_code, 1)
            attempt.assert_not_called()
            records = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual(len(records), 2)
            self.assertTrue(
                all(record["outcome"] == "prerequisite" for record in records)
            )
            self.assertTrue(
                all(record["readiness"] == "prerequisite" for record in records)
            )
            self.assertTrue(all(record["prerequisite"] for record in records))
            self.assertNotIn("private-target", output.getvalue())

    def test_list_mode_is_redacted_and_does_not_start_modem(self) -> None:
        document = {
            "environment": {"SIP_TARGET": "private-target"},
            "plans": [
                {
                    "name": "private-plan",
                    "profile_groups": ["lower"],
                    "link_modes": ["lapm", "raw"],
                    "enabled": True,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".live-call.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            path.chmod(0o600)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = main(("--config", str(path), "--list"))

            self.assertEqual(exit_code, 0)
            self.assertIn("private-plan", output.getvalue())
            self.assertIn('"scenario_count": 12', output.getvalue())
            self.assertNotIn("private-target", output.getvalue())


if __name__ == "__main__":
    unittest.main()
