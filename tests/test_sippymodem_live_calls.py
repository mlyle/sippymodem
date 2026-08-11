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


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.sippymodem_live_calls import (  # noqa: E402
    CallResult,
    LiveCallAttempt,
    LiveCallRunError,
    build_child_environment,
    main,
    selected_plans,
)
from tools.sippymodem_live_config import (  # noqa: E402
    DialPlan,
    LiveCallConfig,
)


class LiveCallRunnerTests(unittest.TestCase):
    def config(
        self,
        directory: str,
        plans: tuple[DialPlan, ...],
        executable: str = "fake-slmodemd",
    ) -> LiveCallConfig:
        root = Path(directory)
        return LiveCallConfig(
            slmodemd=root / executable,
            artifact_directory=root / "artifacts",
            environment=(("SIP_DOMAIN", "private.example"),),
            at_commands=("ATZ",),
            startup_timeout_seconds=1,
            call_timeout_seconds=1,
            online_timeout_seconds=0.25,
            hold_seconds=0,
            plans=plans,
        )

    def plans(self) -> tuple[DialPlan, ...]:
        return (
            DialPlan("first", "one", "", True, ()),
            DialPlan("second", "two", "", False, ()),
            DialPlan("target", "", "private-target", True, ()),
        )

    def test_child_environment_has_one_explicit_private_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = self.config(directory, self.plans())
            environment = build_child_environment(
                config,
                self.plans()[2],
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

            without_target = build_child_environment(
                config,
                self.plans()[0],
                Path(directory) / "other-recording",
                {"SIP_TARGET": "inherited-target"},
            )
            self.assertNotIn("SIP_TARGET", without_target)

    def test_plan_selection_requires_enabled_unique_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = self.config(directory, self.plans())

            self.assertEqual(
                [plan.name for plan in selected_plans(config, ())],
                ["first", "target"],
            )
            self.assertEqual(
                [plan.name for plan in selected_plans(config, ("target",))],
                ["target"],
            )
            for names in (("second",), ("missing",), ("first", "first")):
                with self.subTest(names=names):
                    with self.assertRaises(LiveCallRunError):
                        selected_plans(config, names)

    def test_call_result_contains_no_endpoint_fields(self) -> None:
        result = CallResult("named", "address", "busy", 0.125)
        rendered = json.dumps(result.public_dict(), sort_keys=True)

        self.assertEqual(
            set(result.public_dict()),
            {
                "plan",
                "form",
                "outcome",
                "duration_seconds",
                "terminal_expected_bytes",
                "terminal_verified_bytes",
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
        if command.startswith(b"ATD"):
            os.write(master, b"\\r\\nBUSY\\r\\n")
        else:
            os.write(master, b"\\r\\nOK\\r\\n")
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-slmodemd"
            executable.write_text(script_text, encoding="utf-8")
            executable.chmod(0o700)
            plans = (DialPlan("busy-case", "peer", "", True, ()),)
            config = self.config(directory, plans)

            result = LiveCallAttempt(config, plans[0]).run()

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
            self.assertNotIn("peer", json.dumps(recorded))
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
while True:
    pending += os.read(master, 4096)
    if b"+++" in pending:
        _before, pending = pending.split(b"+++", 1)
        os.write(master, b"\\r\\nOK\\r\\n")
    while b"\\r" in pending:
        command, pending = pending.split(b"\\r", 1)
        if command.startswith(b"ATD"):
            os.write(master, b"\\r\\nCONNECT 300\\r\\nSIPPY-LIVE-OK\\r\\n")
        else:
            os.write(master, b"\\r\\nOK\\r\\n")
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-slmodemd"
            executable.write_text(script_text, encoding="utf-8")
            executable.chmod(0o700)
            plans = (
                DialPlan(
                    "data-case",
                    "peer",
                    "",
                    True,
                    (),
                    b"",
                    b"SIPPY-LIVE-OK\r\n",
                ),
            )
            config = self.config(directory, plans)

            result = LiveCallAttempt(config, plans[0]).run()

            self.assertEqual(result.outcome, "connected")
            self.assertEqual(result.terminal_expected_bytes, 15)
            self.assertEqual(result.terminal_verified_bytes, 15)
            attempt = next(config.artifact_directory.iterdir())
            received = attempt / "terminal-rx.bin"
            transmitted = attempt / "terminal-tx.bin"
            self.assertEqual(received.read_bytes(), b"SIPPY-LIVE-OK\r\n")
            self.assertEqual(transmitted.read_bytes(), b"")
            self.assertEqual(stat.S_IMODE(received.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(transmitted.stat().st_mode), 0o600)

    def test_list_mode_is_redacted_and_does_not_start_modem(self) -> None:
        document = {
            "environment": {"SIP_DOMAIN": "private.example"},
            "plans": [
                {
                    "name": "private-plan",
                    "dial": "peer@private.example",
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
            self.assertIn("address", output.getvalue())
            self.assertNotIn("private.example", output.getvalue())
            self.assertNotIn("peer@", output.getvalue())


if __name__ == "__main__":
    unittest.main()
