#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from dataclasses import replace
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.live_campaign import (  # noqa: E402
    AttemptArtifacts,
    _AttemptProgress,
    _LiveTicker,
    _callback_command,
    _display,
    _freeze_with_status,
    _run_schedule,
    run_physical_attempt,
)
from tools.live_campaign_config import (  # noqa: E402
    CampaignConfig,
    CandidateBuildConfig,
)
from tools.live_campaign_profiles import CASES_BY_NAME  # noqa: E402
from tools.live_campaign_report import (  # noqa: E402
    AttemptResult,
    MATURITY_FULL,
    MATURITY_NO_CONNECT,
    MATURITY_REQUEST,
    OUTCOME_BUSY,
    OUTCOME_SETUP_ERROR,
    ScheduledAttempt,
)
from tools.live_terminal import ONLINE_INITIAL_RESPONSE  # noqa: E402
from tools.revision_build import FrozenBinary, RevisionBuild  # noqa: E402


FAKE_MODEM = r"""
import os
import pty
import select
import sys
import tty
from pathlib import Path

mode = sys.argv[1]
barrier = Path(sys.argv[2]) if len(sys.argv) > 2 else None
role = "request" if "SIP_TARGET" in os.environ else "answer"
master, slave = pty.openpty()
tty.setraw(slave)
print(os.ttyname(slave), flush=True)
buffer = bytearray()
online = False
ring_sent = False
callback_nonce = b"0123456789abcdef0123456789abcdef"
connect = (
    b"\r\nModulation: V21\r\nProtocol: LAPM\r\n"
    b"Compression: NONE\r\nTxRate: 300\r\nCONNECT 300\r\n"
)
request_connect = (
    b"\r\nModulation: V22BIS\r\nProtocol: LAPM\r\n"
    b"Compression: NONE\r\nTxRate: 2400\r\nCONNECT 2400\r\n"
)
initial_payload = b"DMODEM-PATTERN-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"
pong = (
    b"\r\nGETTY:PONG:0123456789abcdef0123456789abcdef:"
    b"1783737600123\r\n"
)
while True:
    if (
        mode == "callback"
        and role == "answer"
        and barrier is not None
        and barrier.exists()
        and not ring_sent
    ):
        os.write(master, b"\r\nRING\r\n")
        ring_sent = True
    readable, _, _ = select.select([master], [], [], 0.01)
    if not readable:
        continue
    try:
        data = os.read(master, 4096)
    except OSError:
        break
    if not data:
        break
    buffer.extend(data)
    if online:
        if mode == "callback" and role == "request" and b"DIAL" in buffer:
            buffer.clear()
            os.write(
                master,
                b"\r\nGETTY:CALLBACK:START:" + callback_nonce + b":ready\r\n",
            )
            assert barrier is not None
            barrier.touch()
        elif b"+++" in buffer:
            _, _, remainder = buffer.partition(b"+++")
            buffer = bytearray(remainder)
            online = False
            os.write(master, b"\r\nOK\r\n")
        elif b"PING" in buffer:
            buffer.clear()
            os.write(master, pong)
        elif b"RETRAIN" in buffer:
            buffer.clear()
            os.write(
                master,
                b"GETTY:RETRAIN:START:" + callback_nonce + b"\r\n"
                b"GETTY:RETRAIN:END:" + callback_nonce + b":1834\r\n",
            )
        continue
    while b"\r" in buffer:
        command, _, remainder = buffer.partition(b"\r")
        buffer = bytearray(remainder)
        command = command.strip().upper()
        if not command:
            continue
        if command == b"ATD":
            if mode == "busy":
                os.write(master, b"\r\nBUSY\r\n")
            elif mode == "early-no-carrier":
                os.write(master, b"\r\nNO CARRIER\r\n")
            elif mode == "callback" and role == "request":
                os.write(master, request_connect + initial_payload)
                online = True
            else:
                os.write(master, connect + initial_payload)
                online = True
        elif command == b"ATA" and mode == "callback" and role == "answer":
            os.write(
                master,
                connect
                + b"GETTY:CALLBACK:CONNECT:"
                + callback_nonce
                + b":ready\r\n",
            )
            online = True
        elif command == b"ATO1":
            os.write(master, b"\r\nOK\r\n" + connect)
            online = True
        else:
            os.write(master, b"\r\nOK\r\n")
"""


class LiveCampaignTests(unittest.TestCase):
    def test_live_display_clears_the_terminal_before_repainting(self) -> None:
        output = io.StringIO()
        with mock.patch("tools.live_campaign.sys.stdout", output):
            _display(
                (CASES_BY_NAME["v21-300-originator"],),
                1,
                (),
                revision="a" * 40,
                seed=1,
                started=time.monotonic(),
                current="v21 / waiting for CONNECT",
                current_seconds=4.0,
                live=True,
            )
        self.assertTrue(output.getvalue().startswith("\x1b[2J\x1b[H"))
        self.assertIn("for=4s", output.getvalue())

    def test_display_fills_only_spare_terminal_rows_with_recent_calls(self) -> None:
        case = CASES_BY_NAME["v21-300-originator"]
        results = tuple(
            AttemptResult(
                case_name=case.name,
                family=case.family,
                role=case.role,
                ordinal=index,
                extra=False,
                physical_try=1,
                outcome="complete",
                connected=True,
                duration_seconds=float(index),
            )
            for index in range(1, 6)
        )
        with mock.patch(
            "tools.live_campaign.shutil.get_terminal_size",
            return_value=os.terminal_size((80, 14)),
        ):
            dashboard = _display(
                (case,),
                5,
                results,
                revision="a" * 40,
                seed=1,
                started=time.monotonic(),
                current="complete",
                live=False,
            )
        recent = [line for line in dashboard.splitlines() if line[:3].isdigit()]
        self.assertEqual(len(recent), 3)
        self.assertLessEqual(len(dashboard.splitlines()), 13)
        self.assertTrue(recent[0].startswith("005"))

    def test_live_ticker_refreshes_until_its_context_exits(self) -> None:
        refreshes: list[float] = []
        with mock.patch("tools.live_campaign.DASHBOARD_REFRESH_SECONDS", 0.01):
            with _LiveTicker(lambda: refreshes.append(time.monotonic())):
                deadline = time.monotonic() + 0.2
                while len(refreshes) < 3 and time.monotonic() < deadline:
                    time.sleep(0.005)
        self.assertGreaterEqual(len(refreshes), 3)
        completed = len(refreshes)
        time.sleep(0.025)
        self.assertEqual(len(refreshes), completed)

    def test_revision_build_status_repaints_while_compiling(self) -> None:
        output = io.StringIO()
        frozen = FrozenBinary(
            Path("/private/cache"),
            Path("/private/cache/slmodemd"),
            "a" * 40,
            "b" * 64,
            False,
        )

        def freeze(_build: object, _cache: object, progress: object) -> FrozenBinary:
            assert callable(progress)
            progress("compiling revision-frozen executable")
            time.sleep(0.03)
            return frozen

        build = RevisionBuild(
            "candidate", Path("."), "HEAD", ("make",), Path("src/slmodemd")
        )
        with (
            mock.patch("tools.live_campaign.sys.stdout", output),
            mock.patch("tools.live_campaign.DASHBOARD_REFRESH_SECONDS", 0.005),
            mock.patch("tools.live_campaign.freeze_revision_build", side_effect=freeze),
        ):
            self.assertIs(
                _freeze_with_status(build, Path("/private/cache"), live_display=True),
                frozen,
            )
        display = output.getvalue()
        self.assertIn("\x1b[2J\x1b[H", display)
        self.assertIn("compiling revision-frozen executable", display)
        self.assertIn("for=", display)
        self.assertTrue(display.endswith("\x1b[?25h\n"))

    def test_attempt_progress_restarts_the_active_stage_clock(self) -> None:
        with mock.patch(
            "tools.live_campaign.time.monotonic",
            side_effect=(10.0, 11.0, 14.5),
        ):
            progress = _AttemptProgress("v34-case")
            progress.set("waiting for CONNECT")
            label, seconds = progress.snapshot()
        self.assertEqual(label, "v34-case / waiting for CONNECT")
        self.assertEqual(seconds, 3.5)

    def test_local_setup_error_does_not_consume_line_cooldown(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "unused.py"
            config = replace(
                self.config(root, script, "pass"),
                inter_call_seconds=90,
            )
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            result = AttemptResult(
                case_name=case.name,
                family=case.family,
                role=case.role,
                ordinal=1,
                extra=False,
                physical_try=1,
                outcome=OUTCOME_SETUP_ERROR,
            )
            store = mock.Mock()
            store.new_attempt.return_value = mock.sentinel.artifacts
            results: list[AttemptResult] = []
            with (
                mock.patch(
                    "tools.live_campaign.run_physical_attempt",
                    return_value=result,
                ),
                mock.patch("tools.live_campaign.time.sleep") as sleep,
                mock.patch("tools.live_campaign.sys.stdout", io.StringIO()),
            ):
                _run_schedule(
                    (scheduled,),
                    config,
                    frozen,
                    store,
                    (case,),
                    1,
                    7,
                    True,
                    results,
                    time.monotonic(),
                    False,
                )
            sleep.assert_not_called()
            self.assertEqual(results, [result])

    def test_busy_retries_every_ten_seconds_for_ninety_seconds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "unused.py"
            config = replace(
                self.config(root, script, "pass"),
                busy_retry_seconds=10,
                busy_retry_limit=9,
            )
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )

            def busy_result(
                _config: object,
                _frozen: object,
                item: ScheduledAttempt,
                physical_try: int,
                _artifacts: object,
                _retrain: bool,
                _progress: object,
            ) -> AttemptResult:
                return AttemptResult(
                    case_name=item.case.name,
                    family=item.case.family,
                    role=item.case.role,
                    ordinal=item.ordinal,
                    extra=item.extra,
                    physical_try=physical_try,
                    outcome=OUTCOME_BUSY,
                    line_attempted=True,
                )

            store = mock.Mock()
            store.new_attempt.return_value = mock.sentinel.artifacts
            results: list[AttemptResult] = []
            with (
                mock.patch(
                    "tools.live_campaign.run_physical_attempt",
                    side_effect=busy_result,
                ),
                mock.patch("tools.live_campaign.time.sleep") as sleep,
                mock.patch("tools.live_campaign.sys.stdout", io.StringIO()),
            ):
                _run_schedule(
                    (scheduled,),
                    config,
                    frozen,
                    store,
                    (case,),
                    1,
                    7,
                    True,
                    results,
                    time.monotonic(),
                    False,
                )
            self.assertEqual(sleep.call_args_list, [mock.call(10)] * 9)
            self.assertEqual(sum(r.outcome == OUTCOME_BUSY for r in results), 10)
            self.assertEqual(results[-1].outcome, "unavailable")

    def config(
        self,
        root: Path,
        script: Path,
        mode: str,
        barrier: Path | None = None,
    ) -> CampaignConfig:
        build = CandidateBuildConfig(
            repository=root,
            revision="HEAD",
            command=("true",),
            executable=Path("modem"),
            arguments=(str(script), mode, *((str(barrier),) if barrier else ())),
        )
        target = (("SIP_TARGET", "sip:private@example.invalid"),)
        return CampaignConfig(
            enabled=True,
            artifact_directory=root / "campaigns",
            build_cache=root / "builds",
            candidate=build,
            outbound_environment=target,
            callback_request_environment=target,
            callback_answer_environment=(),
            startup_timeout_seconds=2,
            setup_timeout_seconds=2,
            connect_timeout_seconds=2,
            callback_request_timeout_seconds=2,
            callback_marker_timeout_seconds=2,
            callback_return_timeout_seconds=2,
            callback_release_seconds=0,
            online_timeout_seconds=2,
            online_settle_seconds=0,
            retrain_timeout_seconds=2,
            cleanup_timeout_seconds=1,
            inter_call_seconds=0,
            busy_retry_seconds=0,
            busy_retry_limit=0,
        )

    def artifacts(
        self, root: Path, scheduled: ScheduledAttempt
    ) -> AttemptArtifacts:
        campaign = root / "run"
        campaign.mkdir(mode=0o700)
        (campaign / "attempts").mkdir(mode=0o700)
        return AttemptArtifacts.create(campaign, 1, scheduled, 1)

    def test_outbound_real_pty_reaches_retrain_data_bucket(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            artifacts = self.artifacts(root, scheduled)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            progress = _AttemptProgress(case.name)
            with mock.patch("tools.live_terminal.time.sleep"):
                with mock.patch.object(
                    progress, "set", wraps=progress.set
                ) as set_stage:
                    result = run_physical_attempt(
                        self.config(root, script, "pass"),
                        frozen,
                        scheduled,
                        1,
                        artifacts,
                        True,
                        progress,
                    )

            self.assertEqual(result.maturity, MATURITY_FULL)
            self.assertEqual(result.modulation, "V.21")
            self.assertEqual(result.receive_rate, 300)
            self.assertEqual(result.transmit_rate, 300)
            transmitted = (artifacts.legs["modem"] / "terminal-tx.bin").read_bytes()
            self.assertNotIn(b"0123456789", transmitted)
            self.assertEqual(transmitted.count(b"RETRAIN"), 1)
            self.assertEqual(transmitted.count(b"PING"), 1)
            stages = [call.args[0] for call in set_stage.call_args_list]
            self.assertIn("waiting for remote retrain END", stages)
            self.assertFalse(any("nonce=" in stage for stage in stages))
            detail = json.loads(
                (artifacts.directory / "result.json").read_text(encoding="utf-8")
            )
            self.assertEqual(detail["result"]["outcome"], "complete")
            for path in artifacts.directory.rglob("*"):
                if path.is_file():
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_busy_is_not_a_maturity_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            result = run_physical_attempt(
                self.config(root, script, "busy"),
                frozen,
                scheduled,
                1,
                self.artifacts(root, scheduled),
                True,
            )
            self.assertEqual(result.outcome, OUTCOME_BUSY)
            self.assertTrue(result.line_attempted)
            self.assertIsNone(result.maturity)
            self.assertFalse(result.counts_as_attempt)

    def test_no_carrier_is_reported_as_a_failed_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            result = run_physical_attempt(
                self.config(root, script, "early-no-carrier"),
                frozen,
                scheduled,
                1,
                self.artifacts(root, scheduled),
                True,
            )
            self.assertEqual(result.outcome, "complete")
            self.assertTrue(result.line_attempted)
            self.assertEqual(result.maturity, "no-connect")
            self.assertIn("no-carrier", result.detail)
            self.assertTrue(result.counts_as_attempt)

    def test_callback_request_features_are_case_owned(self) -> None:
        plain = _callback_command(CASES_BY_NAME["v21-300-callback"])
        v22bis = _callback_command(
            CASES_BY_NAME["v22bis-2400-lapm-callback"]
        )
        pcm = _callback_command(CASES_BY_NAME["v92-pcmup-36000-callback"])
        self.assertEqual(plain, b"\r\n\r\nDIAL\r\n")
        self.assertEqual(v22bis, b"\r\n\r\nDIAL V42\r\n")
        self.assertEqual(pcm, b"\r\n\r\nDIAL V42 V44 PCM\r\n")

    def test_callback_real_ptys_bind_nonce_and_retrain(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            barrier = root / "callback-ready"
            case = CASES_BY_NAME["v21-300-callback"]
            scheduled = ScheduledAttempt(case, 1)
            artifacts = self.artifacts(root, scheduled)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            progress = _AttemptProgress(case.name)
            with (
                mock.patch("tools.live_terminal.time.sleep"),
                mock.patch.object(
                    progress, "set", wraps=progress.set
                ) as set_stage,
            ):
                result = run_physical_attempt(
                    self.config(root, script, "callback", barrier),
                    frozen,
                    scheduled,
                    1,
                    artifacts,
                    True,
                    progress,
                )
            self.assertEqual(result.maturity, MATURITY_FULL)
            self.assertTrue(result.callback_request_attempted)
            self.assertEqual(
                result.callback_request_maturity,
                MATURITY_REQUEST,
            )
            self.assertTrue(result.callback_return_attempted)
            self.assertEqual(
                result.remote_retrain_nonce,
                "0123456789abcdef0123456789abcdef",
            )
            self.assertTrue(barrier.exists())
            request_rx = (
                artifacts.legs["request"] / "terminal-rx.bin"
            ).read_bytes()
            self.assertIn(ONLINE_INITIAL_RESPONSE, request_rx)
            self.assertIn(
                b"GETTY:CALLBACK:START:",
                request_rx,
            )
            self.assertIn(
                b"GETTY:CALLBACK:CONNECT:",
                (artifacts.legs["answer"] / "terminal-rx.bin").read_bytes(),
            )
            stages = [call.args[0] for call in set_stage.call_args_list]
            ring = stages.index("waiting for callback RING")
            answering = stages.index("answering callback")
            training = stages.index("waiting for callback CONNECT")
            self.assertLess(ring, answering)
            self.assertLess(answering, training)

    def test_callback_request_failure_does_not_score_requested_modulation(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            case = CASES_BY_NAME["v21-300-raw-callback"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            result = run_physical_attempt(
                self.config(root, script, "early-no-carrier"),
                frozen,
                scheduled,
                1,
                self.artifacts(root, scheduled),
                True,
            )
            self.assertEqual(
                result.callback_request_maturity,
                MATURITY_NO_CONNECT,
            )
            self.assertTrue(result.callback_request_attempted)
            self.assertFalse(result.callback_return_attempted)
            self.assertTrue(result.callback_request_only)
            self.assertIsNone(result.maturity)
            self.assertFalse(result.counts_as_attempt)

    def test_callback_answer_write_failure_remains_request_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            barrier = root / "callback-ready"
            case = CASES_BY_NAME["v21-300-raw-callback"]
            scheduled = ScheduledAttempt(case, 1)
            frozen = FrozenBinary(
                root,
                Path(sys.executable),
                "a" * 40,
                "b" * 64,
                False,
            )
            with mock.patch(
                "tools.interop_endpoint.InteropTerminal.start_answer",
                side_effect=TimeoutError,
            ):
                result = run_physical_attempt(
                    self.config(root, script, "callback", barrier),
                    frozen,
                    scheduled,
                    1,
                    self.artifacts(root, scheduled),
                    True,
                )
            self.assertEqual(
                result.callback_request_maturity,
                MATURITY_REQUEST,
            )
            self.assertFalse(result.callback_return_attempted)
            self.assertTrue(result.callback_request_only)
            self.assertIsNone(result.maturity)
            self.assertFalse(result.counts_as_attempt)


if __name__ == "__main__":
    unittest.main()
