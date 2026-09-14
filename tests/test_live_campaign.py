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
    _case_connect_error,
    _display,
    _freeze_with_status,
    _run_schedule,
    run_campaign,
    run_physical_attempt,
)
from tools.live_campaign_config import (  # noqa: E402
    CampaignConfig,
    CandidateBuildConfig,
)
from tools.live_campaign_profiles import CASES_BY_NAME  # noqa: E402
from tools.live_campaign_report import (  # noqa: E402
    AttemptResult,
    MATURITY_CONNECT,
    MATURITY_DATA,
    MATURITY_FULL,
    MATURITY_NO_CONNECT,
    MATURITY_REQUEST,
    OUTCOME_BUSY,
    OUTCOME_SETUP_ERROR,
    ScheduledAttempt,
)
from tools.live_terminal import ConnectReport, ONLINE_INITIAL_RESPONSE  # noqa: E402
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
callback_requested = False
callback_nonce = b"0123456789abcdef0123456789abcdef"
raw_transport = False
retrained = False

def connect_response():
    if "pcm-retrain" in mode:
        if retrained and mode == "pcm-retrain-unknown":
            return b"\r\nCONNECT 24000\r\n"
        modulation = b"V34" if retrained and mode.startswith("callback") else b"V92"
        rate = b"24000"
    elif mode == "pcm-conventional":
        modulation = b"V92"
        rate = b"24000"
    elif "wrong-profile" in mode:
        modulation = b"V22"
        rate = b"1200"
    elif mode == "wrong-link":
        modulation = b"V22BIS"
        rate = b"2400"
    else:
        modulation = b"V21"
        rate = b"300"
    protocol = b"NONE" if raw_transport or mode == "wrong-link" else b"LAPM"
    upstream = b"Upstream: V34\r\n" if mode == "pcm-conventional" else b""
    tx_rate = b"56000" if mode == "pcm-conventional" else rate
    if "pcm-retrain" in mode:
        upstream = b"Upstream: V34\r\n" if retrained else b"Upstream: PCM\r\n"
        tx_rate = (rate if mode.startswith("callback") else b"52000") if retrained else b"56000"
    return (
        b"\r\nModulation: " + modulation + b"\r\n" + upstream + b"Protocol: " + protocol
        + b"\r\nCompression: NONE\r\nTxRate: " + tx_rate
        + b"\r\nCONNECT " + rate + b"\r\n"
    )
def request_connect_response():
    modulation = b"V21" if mode == "callback-wrong-request" else b"V22BIS"
    rate = b"300" if mode == "callback-wrong-request" else b"2400"
    protocol = b"NONE" if mode == "callback-wrong-request" else b"LAPM"
    return (
        b"\r\nModulation: " + modulation + b"\r\nProtocol: " + protocol
        + b"\r\nCompression: NONE\r\nTxRate: " + rate
        + b"\r\nCONNECT " + rate + b"\r\n"
    )
initial_payload = b"DMODEM-PATTERN-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"
pong = (
    b"\r\nGETTY:PONG:0123456789abcdef0123456789abcdef:"
    b"1783737600123\r\n"
)
while True:
    if (
        mode.startswith("callback")
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
        if mode in ("raw-wakeup", "wrong-link") and b"\r\r" in buffer:
            buffer.clear()
            os.write(master, initial_payload)
        elif mode.startswith("callback") and role == "request" and b"DIAL" in buffer:
            buffer.clear()
            os.write(
                master,
                b"\r\nGETTY:CALLBACK:START:" + callback_nonce + b":ready\r\n",
            )
            callback_requested = True
        elif b"+++" in buffer:
            _, _, remainder = buffer.partition(b"+++")
            buffer = bytearray(remainder)
            online = False
            os.write(master, b"\r\nOK\r\n")
        elif b"PING" in buffer:
            buffer.clear()
            if not (retrained and mode == "pcm-retrain-no-data"):
                os.write(master, pong)
        elif b"RETRAIN" in buffer:
            buffer.clear()
            if mode in ("online-prefix-before-retrain-end", "wrong-profile-no-end"):
                os.write(
                    master,
                    b"GETTY:RETRAIN:START:"
                    + callback_nonce
                    + b"\r\n+++",
                )
            else:
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
            if mode == "connect-timeout":
                continue
            elif mode == "busy":
                os.write(master, b"\r\nBUSY\r\n")
            elif mode == "early-no-carrier":
                os.write(master, b"\r\nNO CARRIER\r\n")
            elif mode.startswith("callback") and role == "request":
                os.write(master, request_connect_response() + initial_payload)
                online = True
            elif mode in ("raw-wakeup", "wrong-link"):
                os.write(master, connect_response())
                online = True
            else:
                os.write(master, connect_response() + initial_payload)
                online = True
        elif command == b"ATA" and mode.startswith("callback") and role == "answer":
            os.write(
                master,
                connect_response()
                + b"GETTY:CALLBACK:CONNECT:"
                + callback_nonce
                + b":ready\r\n",
            )
            online = True
        elif command == b"ATO1":
            retrained = True
            if mode == "pcm-retrain-failed":
                os.write(master, b"\r\nOK\r\nNO CARRIER\r\n")
            else:
                os.write(master, b"\r\nOK\r\n" + connect_response())
                online = True
        elif command == b"ATH0":
            print("ATH0", flush=True)
            if callback_requested:
                assert barrier is not None
                barrier.touch()
            os.write(master, b"\r\nOK\r\n")
        else:
            if command == b"ATZ":
                raw_transport = False
            elif command in (b"AT\\N0", b"AT+DS=0,0"):
                raw_transport = True
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
                campaign_id="test-campaign",
                started=time.monotonic(),
                current="v21 / waiting for CONNECT",
                current_seconds=4.0,
                live=True,
            )
        self.assertTrue(output.getvalue().startswith("\x1b[2J\x1b[H"))
        self.assertIn("campaign=test-campaign", output.getvalue().splitlines()[0])
        self.assertNotIn("seed=", output.getvalue())
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
                campaign_id="test-campaign",
                started=time.monotonic(),
                current="complete",
                live=False,
            )
        recent = [line for line in dashboard.splitlines() if line[:3].isdigit()]
        self.assertEqual(len(recent), 1)
        self.assertLessEqual(len(dashboard.splitlines()), 13)
        self.assertTrue(recent[0].startswith("005"))

    def test_display_keeps_mismatch_profiles_together_within_terminal(self) -> None:
        case = CASES_BY_NAME["v21-300-originator"]
        result = AttemptResult(
            case_name=case.name, family=case.family, role=case.role,
            ordinal=1, extra=False, physical_try=1, outcome="complete",
            connected=True, modulation="Bell 103", protocol="none",
            receive_rate=300, transmit_rate=300,
            detail="CONNECT selected Bell 103 instead of V.21",
        )
        for terminal_rows, expected_calls in ((16, 0), (17, 1), (18, 1)):
            with self.subTest(terminal_rows=terminal_rows), mock.patch(
                "tools.live_campaign.shutil.get_terminal_size",
                return_value=os.terminal_size((80, terminal_rows)),
            ):
                dashboard = _display(
                    (case,), 1, (result,), revision="a" * 40,
                    campaign_id="test-campaign",
                    started=time.monotonic(), current="complete", live=False,
                )
            lines = dashboard.splitlines()
            self.assertLessEqual(len(lines), terminal_rows - 1)
            self.assertEqual(
                sum(line[:3].isdigit() for line in lines), expected_calls
            )
            self.assertEqual(
                sum(line.startswith("     tried:") for line in lines),
                expected_calls,
            )

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
                    True,
                    results,
                    time.monotonic(),
                    False,
                )
            self.assertEqual(sleep.call_args_list, [mock.call(10)] * 9)
            self.assertEqual(sum(r.outcome == OUTCOME_BUSY for r in results), 10)
            self.assertEqual(results[-1].outcome, "unavailable")

    def test_campaign_headers_identify_saved_evidence_and_keep_seed_in_manifest(self) -> None:
        for interrupted in (False, True):
            with (
                self.subTest(interrupted=interrupted),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                case = CASES_BY_NAME["v21-300-originator"]
                frozen = FrozenBinary(
                    root, Path(sys.executable), "a" * 40, "b" * 64, False
                )
                config = self.config(root, root / "unused.py", "pass")
                output = io.StringIO()
                result = AttemptResult(
                    case_name=case.name, family=case.family, role=case.role,
                    ordinal=1, extra=False, physical_try=1, outcome="complete",
                    connected=True,
                )
                with (
                    mock.patch("tools.live_campaign.sys.stdout", output),
                    mock.patch(
                        "tools.live_campaign.run_physical_attempt",
                        return_value=result,
                        side_effect=KeyboardInterrupt if interrupted else None,
                    ),
                ):
                    arguments = dict(
                        attempts=1, failure_extra=0, seed=12345,
                        randomize=True, retrain=True, live_display=True,
                    )
                    if interrupted:
                        with self.assertRaises(KeyboardInterrupt):
                            run_campaign(config, frozen, (case,), **arguments)
                    else:
                        run_campaign(config, frozen, (case,), **arguments)
                campaign = next(config.artifact_directory.iterdir())
                manifest = json.loads((campaign / "manifest.json").read_text())
                summary = json.loads((campaign / "summary.json").read_text())
                expected = f"revision={'a' * 12} campaign={campaign.name} "
                self.assertTrue(summary["dashboard"].startswith(expected))
                self.assertEqual(summary["complete"], not interrupted)
                state = "interrupted" if interrupted else "complete"
                self.assertIn(f"current={state}", summary["dashboard"])
                headers = [
                    line.split("revision=", 1)[1]
                    for line in output.getvalue().splitlines()
                    if "revision=" in line
                ]
                self.assertTrue(headers)
                self.assertTrue(all(
                    f"campaign={campaign.name} " in line for line in headers
                ))
                self.assertNotIn("seed=", output.getvalue())
                self.assertEqual(manifest["seed"], 12345)

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
            self.assertEqual(result.protocol, "none")
            self.assertEqual(result.compression, "none")
            self.assertEqual(result.receive_rate, 300)
            self.assertEqual(result.transmit_rate, 300)
            transmitted = (artifacts.legs["modem"] / "terminal-tx.bin").read_bytes()
            self.assertNotIn(b"0123456789", transmitted)
            self.assertEqual(transmitted.count(b"RETRAIN"), 1)
            self.assertEqual(transmitted.count(b"PING"), 1)
            self.assertIn(b"ATH0\n", artifacts.daemon_log("modem").read_bytes())
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

    def test_cleanup_hangs_up_even_when_the_call_body_does_not_return(self) -> None:
        for failure in (None, OSError, KeyboardInterrupt):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                case = CASES_BY_NAME["v21-300-originator"]
                endpoint = mock.Mock()
                endpoint.terminal.escape_and_hang_up.return_value = True
                endpoint.stop.return_value = True

                def body(
                    _config, _frozen, _scheduled, _artifacts, state,
                    _retrain, endpoints, _progress,
                ):
                    endpoints.append(endpoint)
                    state.detail = "outbound CONNECT timed out"
                    if failure is not None:
                        raise failure

                scheduled = ScheduledAttempt(case, 1)
                with mock.patch("tools.live_campaign._outbound_body", side_effect=body):
                    arguments = (
                        self.config(root, root / "unused.py", "pass"),
                        mock.sentinel.frozen,
                        scheduled,
                        1,
                        self.artifacts(root, scheduled),
                        False,
                    )
                    if failure is KeyboardInterrupt:
                        with self.assertRaises(KeyboardInterrupt):
                            run_physical_attempt(*arguments)
                    else:
                        run_physical_attempt(*arguments)
                self.assertEqual(endpoint.mock_calls, [
                    mock.call.terminal.escape_and_hang_up(),
                    mock.call.stop(1),
                ])

    def test_failed_hangup_still_stops_all_endpoints_and_preserves_detail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = CASES_BY_NAME["v21-300-originator"]
            scheduled = ScheduledAttempt(case, 1)
            endpoints = [mock.Mock(), mock.Mock()]
            for endpoint in endpoints:
                endpoint.terminal.escape_and_hang_up.return_value = False
                endpoint.stop.return_value = True

            def body(
                _config, _frozen, _scheduled, _artifacts, state,
                _retrain, owned_endpoints, _progress,
            ):
                owned_endpoints.extend(endpoints)
                state.detail = "outbound CONNECT timed out"

            with mock.patch("tools.live_campaign._outbound_body", side_effect=body):
                result = run_physical_attempt(
                    self.config(root, root / "unused.py", "pass"),
                    mock.sentinel.frozen,
                    scheduled,
                    1,
                    self.artifacts(root, scheduled),
                    False,
                )
            self.assertEqual(
                result.detail,
                "outbound CONNECT timed out; modem hangup or cleanup did not complete",
            )
            for endpoint in endpoints:
                self.assertEqual(endpoint.mock_calls, [
                    mock.call.terminal.escape_and_hang_up(),
                    mock.call.stop(1),
                ])

    def test_connect_timeout_still_sends_ath0(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            scheduled = ScheduledAttempt(CASES_BY_NAME["v21-300-originator"], 1)
            artifacts = self.artifacts(root, scheduled)
            result = run_physical_attempt(
                replace(
                    self.config(root, script, "connect-timeout"),
                    connect_timeout_seconds=0.05,
                ),
                FrozenBinary(root, Path(sys.executable), "a" * 40, "b" * 64, False),
                scheduled,
                1,
                artifacts,
                False,
            )
            self.assertEqual(result.detail, "outbound CONNECT timed out")
            self.assertIn(b"ATH0\n", artifacts.daemon_log("modem").read_bytes())

    def test_connect_profile_requires_requested_mode_rates_and_protocol(
        self,
    ) -> None:
        raw_v21 = CASES_BY_NAME["v21-300-originator"]
        self.assertIsNone(
            _case_connect_error(
                raw_v21,
                ConnectReport("V.21", "none", "none", 300, 300),
            )
        )
        self.assertIn(
            "V.22",
            _case_connect_error(
                raw_v21,
                ConnectReport("V.22", "none", "none", 300, 300),
            ),
        )
        self.assertIn(
            "protocol",
            _case_connect_error(
                raw_v21,
                ConnectReport("V.21", "LAPM", "none", 300, 300),
            ),
        )
        self.assertIn(
            "RX",
            _case_connect_error(
                raw_v21,
                ConnectReport("V.21", "none", "none", 1200, 300),
            ),
        )
        self.assertIn(
            "TX",
            _case_connect_error(
                raw_v21,
                ConnectReport("V.21", "none", "none", 300, 1200),
            ),
        )
        fallback = CASES_BY_NAME["v90-analogue-fallback-raw-originator"]
        self.assertIsNone(
            _case_connect_error(
                fallback,
                ConnectReport("V.34", "none", "none", 33600, 33600),
            )
        )

        raw_v34 = CASES_BY_NAME["v34-26400-b3200-raw-originator"]
        lapm_v34 = CASES_BY_NAME["v34-26400-b3200-lapm-originator"]
        self.assertIsNone(
            _case_connect_error(
                raw_v34,
                ConnectReport("V.34", "none", "none", 26400, 24000),
            ),
        )
        self.assertIsNone(
            _case_connect_error(
                lapm_v34,
                ConnectReport("V.34", "LAPM", "none", 26400, 24000),
            )
        )
        self.assertIn(
            "RX",
            _case_connect_error(
                lapm_v34,
                ConnectReport("V.34", "LAPM", "none", 2400, 24000),
            ),
        )
        self.assertIn(
            "TX",
            _case_connect_error(
                lapm_v34,
                ConnectReport("V.34", "LAPM", "none", 24000, 28800),
            ),
        )

        raw_v90 = CASES_BY_NAME[
            "v90-d56000-u31200-b3429-raw-originator"
        ]
        self.assertIsNone(
            _case_connect_error(
                raw_v90,
                ConnectReport("V.90", "none", "none", 26400, 50667),
            )
        )
        self.assertIn(
            "RX",
            _case_connect_error(
                raw_v90,
                ConnectReport("V.90", "none", "none", 2400, 50667),
            ),
        )
        self.assertIn(
            "TX",
            _case_connect_error(
                raw_v90,
                ConnectReport("V.90", "none", "none", 26400, 26400),
            ),
        )

    def test_pcm_profile_requires_upstream_evidence_not_just_rate(self) -> None:
        for role in ("originator", "callback"):
            for mode, protocol in (("raw", "none"), ("lapm", "LAPM")):
                case = CASES_BY_NAME[f"v92-pcmup-24000-{mode}-{role}"]
                for upstream in (None, "V.34", "PCM"):
                    with self.subTest(role=role, mode=mode, upstream=upstream):
                        report = ConnectReport("V.92", protocol, "none", 24000,
                                               56000, upstream_mode=upstream)
                        error = _case_connect_error(case, report)
                        if upstream == "PCM":
                            self.assertIsNone(error)
                        else:
                            self.assertIn("upstream instead of PCM", error)

    def test_mismatched_calls_continue_data_and_both_retrains(self) -> None:
        for mode, name in (
            ("wrong-profile", "v21-300-originator"),
            ("callback-wrong-profile", "v21-300-callback"),
            ("wrong-link", "v22bis-2400-lapm-originator"),
            ("wrong-profile-no-end", "v21-300-originator"),
            ("wrong-profile-no-retrain", "v21-300-originator"),
            ("pcm-conventional", "v92-pcmup-24000-raw-originator"),
        ):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                script = root / "fake.py"
                script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
                scheduled = ScheduledAttempt(CASES_BY_NAME[name], 1)
                artifacts = self.artifacts(root, scheduled)
                config = self.config(root, script, mode, root / "callback-ready")
                if mode.endswith("no-end"):
                    config = replace(config, retrain_timeout_seconds=0.1)
                with mock.patch("tools.live_terminal.time.sleep"):
                    result = run_physical_attempt(
                        config,
                        FrozenBinary(root, Path(sys.executable), "a" * 40, "b" * 64, False),
                        scheduled, 1, artifacts, not mode.endswith("no-retrain"),
                    )
                self.assertTrue(result.profile_mismatch)
                self.assertFalse(result.successful())
                self.assertTrue(result.initial_data)
                self.assertFalse(result.callback_request_profile_mismatch)
                detail = json.loads((artifacts.directory / "result.json").read_text())
                self.assertEqual(detail["result"]["profile_error"], result.profile_error)
                if mode == "pcm-conventional":
                    self.assertEqual(result.upstream_mode, "V.34")
                    self.assertEqual(detail["result"]["upstream_mode"], "V.34")
                leg = "answer" if mode.startswith("callback") else "modem"
                tx = (artifacts.legs[leg] / "terminal-tx.bin").read_bytes()
                if mode.endswith("no-retrain"):
                    self.assertEqual(result.maturity, MATURITY_DATA)
                    self.assertNotIn(b"RETRAIN", tx)
                    self.assertNotIn(b"ATO1", tx)
                elif mode.endswith("no-end"):
                    self.assertIn(b"RETRAIN", tx)
                    self.assertEqual(result.maturity, MATURITY_DATA)
                    self.assertIn("remote retrain END", result.detail)
                    self.assertNotIn(b"ATO1", tx)
                else:
                    self.assertIn(b"RETRAIN", tx)
                    self.assertEqual(result.maturity, MATURITY_FULL)
                    self.assertEqual(result.detail, "")
                    self.assertIn(b"ATO1", tx)
                    self.assertIn(b"PING", tx)

    def test_retrain_profile_is_saved_separately_before_the_data_probe(self) -> None:
        for mode in ("pcm-retrain-fallback", "callback-pcm-retrain-fallback",
                     "pcm-retrain-no-data", "pcm-retrain-unknown", "pcm-retrain-failed"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                script = root / "fake.py"
                script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
                role = "callback" if mode.startswith("callback") else "originator"
                scheduled = ScheduledAttempt(CASES_BY_NAME[f"v92-pcmup-24000-raw-{role}"], 1)
                artifacts = self.artifacts(root, scheduled)
                config = self.config(root, script, mode, root / "callback-ready")
                if mode.endswith("no-data"):
                    config = replace(config, online_timeout_seconds=0.1)
                with mock.patch("tools.live_terminal.time.sleep"):
                    result = run_physical_attempt(
                        config,
                        FrozenBinary(root, Path(sys.executable), "a" * 40, "b" * 64, False),
                        scheduled, 1, artifacts, True,
                    )
                initial = ("V.92", "none", "none", 24000, 56000, "PCM")
                fields = ("modulation", "protocol", "compression", "receive_rate",
                          "transmit_rate", "upstream_mode")
                self.assertEqual(tuple(getattr(result, name) for name in fields), initial)
                self.assertFalse(result.profile_mismatch)
                self.assertTrue(result.initial_data)
                self.assertTrue(result.remote_retrain)
                if mode.endswith("failed"):
                    expected = (None,) * len(fields)
                    self.assertFalse(result.retrain_connected)
                elif mode.endswith("unknown"):
                    expected = (None, None, None, 24000, None, None)
                elif role == "callback":
                    expected = ("V.34", "none", "none", 24000, 24000, "V.34")
                else:
                    expected = ("V.92", "none", "none", 24000, 52000, "V.34")
                saved = json.loads((artifacts.directory / "result.json").read_text())["result"]
                for name, value in zip(fields, expected):
                    self.assertEqual(getattr(result, "retrain_" + name), value)
                    self.assertEqual(saved["retrain_" + name], value)
                self.assertEqual(tuple(saved[name] for name in fields), initial)
                self.assertEqual(result.retrain_data, not mode.endswith(("no-data", "failed")))

    def test_online_bytes_before_retrain_end_are_ordinary_payload(
        self,
    ) -> None:
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
            config = replace(
                self.config(root, script, "online-prefix-before-retrain-end"),
                retrain_timeout_seconds=0.1,
            )
            result = run_physical_attempt(
                config,
                frozen,
                scheduled,
                1,
                artifacts,
                True,
            )

            self.assertEqual(result.outcome, "complete")
            self.assertTrue(result.connected)
            self.assertTrue(result.initial_data)
            self.assertFalse(result.remote_retrain)
            self.assertEqual(result.maturity, MATURITY_DATA)
            self.assertIn("END marker", result.detail)

    def test_raw_outbound_wakes_peer_before_initial_data(self) -> None:
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
            result = run_physical_attempt(
                self.config(root, script, "raw-wakeup"),
                frozen,
                scheduled,
                1,
                artifacts,
                False,
            )

            self.assertEqual(result.maturity, MATURITY_DATA)
            self.assertTrue(result.initial_data)
            transmitted = (
                artifacts.legs["modem"] / "terminal-tx.bin"
            ).read_bytes()
            self.assertIn(b"\r\r", transmitted)

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
            self.assertEqual(result.callback_request_modulation, "V.22bis")
            self.assertEqual(result.callback_request_protocol, "LAPM")
            self.assertEqual(result.callback_request_receive_rate, 2400)
            self.assertEqual(result.callback_request_transmit_rate, 2400)
            self.assertTrue(result.callback_return_attempted)
            self.assertEqual(
                result.remote_retrain_nonce,
                "0123456789abcdef0123456789abcdef",
            )
            self.assertTrue(barrier.exists())
            for leg in ("request", "answer"):
                self.assertIn(b"ATH0\n", artifacts.daemon_log(leg).read_bytes())
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

    def test_callback_request_mismatch_does_not_stop_returning_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "fake.py"
            script.write_text(textwrap.dedent(FAKE_MODEM), encoding="utf-8")
            barrier = root / "callback-ready"
            case = CASES_BY_NAME["v21-300-raw-callback"]
            scheduled = ScheduledAttempt(case, 1)
            artifacts = self.artifacts(root, scheduled)
            result = run_physical_attempt(
                self.config(root, script, "callback-wrong-request", barrier),
                FrozenBinary(
                    root,
                    Path(sys.executable),
                    "a" * 40,
                    "b" * 64,
                    False,
                ),
                scheduled,
                1,
                artifacts,
                False,
            )

            self.assertEqual(result.callback_request_maturity, MATURITY_REQUEST)
            self.assertEqual(result.callback_request_modulation, "V.21")
            self.assertEqual(result.callback_request_protocol, "none")
            self.assertEqual(result.callback_request_receive_rate, 300)
            self.assertEqual(result.callback_request_transmit_rate, 300)
            self.assertTrue(result.callback_return_attempted)
            self.assertFalse(result.callback_request_only)
            self.assertTrue(result.callback_request_profile_mismatch)
            self.assertFalse(result.profile_mismatch)
            self.assertIn("CONNECT selected V.21", result.callback_request_profile_error)
            self.assertEqual(result.maturity, MATURITY_DATA)
            self.assertTrue(barrier.exists())
            tx = (artifacts.legs["request"] / "terminal-tx.bin").read_bytes()
            self.assertTrue(tx.startswith(b"\r\r" + _callback_command(case)))

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
