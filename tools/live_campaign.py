#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run a revision-frozen campaign against one external modem."""

from __future__ import annotations

import argparse
from contextlib import nullcontext
from dataclasses import asdict, dataclass, field, replace
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import sys
import tempfile
import threading
import time
from typing import Callable, IO, Mapping, Sequence


if __package__:
    from .interop_config import EndpointConfig
    from .interop_endpoint import EndpointProcess, InteropRunError
    from .live_campaign_config import (
        CampaignConfig,
        CampaignConfigError,
        child_environment,
        load_campaign_config,
    )
    from .live_campaign_profiles import (
        CASES_BY_NAME,
        FAMILY_ORDER,
        LINK_RAW,
        ROLES,
        CampaignCase,
        select_cases,
    )
    from .live_campaign_report import (
        AttemptResult,
        MATURITY_CONNECT,
        MATURITY_NO_CONNECT,
        MATURITY_REQUEST,
        OUTCOME_BUSY,
        OUTCOME_COMPLETE,
        OUTCOME_INFRASTRUCTURE_ERROR,
        OUTCOME_SETUP_ERROR,
        OUTCOME_UNAVAILABLE,
        ScheduledAttempt,
        base_schedule,
        extra_schedule,
        render_dashboard,
    )
    from .live_terminal import (
        ConnectReport,
        LiveTerminalError,
        ModemTerminal,
        ONLINE_PING_REQUEST,
        REMOTE_RETRAIN_REQUEST,
    )
    from .revision_build import (
        FrozenBinary,
        RevisionBuild,
        RevisionBuildError,
        freeze_revision_build,
    )
else:
    from interop_config import EndpointConfig
    from interop_endpoint import EndpointProcess, InteropRunError
    from live_campaign_config import (
        CampaignConfig,
        CampaignConfigError,
        child_environment,
        load_campaign_config,
    )
    from live_campaign_profiles import (
        CASES_BY_NAME,
        FAMILY_ORDER,
        LINK_RAW,
        ROLES,
        CampaignCase,
        select_cases,
    )
    from live_campaign_report import (
        AttemptResult,
        MATURITY_CONNECT,
        MATURITY_NO_CONNECT,
        MATURITY_REQUEST,
        OUTCOME_BUSY,
        OUTCOME_COMPLETE,
        OUTCOME_INFRASTRUCTURE_ERROR,
        OUTCOME_SETUP_ERROR,
        OUTCOME_UNAVAILABLE,
        ScheduledAttempt,
        base_schedule,
        extra_schedule,
        render_dashboard,
    )
    from live_terminal import (
        ConnectReport,
        LiveTerminalError,
        ModemTerminal,
        ONLINE_PING_REQUEST,
        REMOTE_RETRAIN_REQUEST,
    )
    from revision_build import (
        FrozenBinary,
        RevisionBuild,
        RevisionBuildError,
        freeze_revision_build,
    )


SKIPPED_EXIT_CODE = 77
CALLBACK_START_RE = re.compile(
    rb"GETTY:CALLBACK:START:(?P<nonce>[0-9a-fA-F]{32}):"
    rb"[^\r\n]*(?:\r\n|\r|\n)"
)
CALLBACK_REQUEST_COMMANDS = (
    "ATZ",
    "ATE0",
    "AT&A7",
    "ATX3",
    "AT+DS=3,1",
    "AT+MS=122,0,2400,2400",
    "ATE0V1Q0",
    "ATS95=7",
)
FILE_MODE = 0o600
DIRECTORY_MODE = 0o700
DASHBOARD_REFRESH_SECONDS = 2.0
RAW_LINK_WAKE = b"\r\r"
FAMILY_CONNECT_MODULATIONS = {
    "bell103": ("Bell 103",),
    "v21": ("V.21",),
    "bell212": ("Bell 212A",),
    "v22": ("V.22",),
    "v22bis": ("V.22bis",),
    "v23": ("V.23",),
    "v32": ("V.32",),
    "v32bis": ("V.32bis",),
    "v34": ("V.34",),
    "v90": ("V.90",),
    "v92": ("V.92",),
}
ANALOGUE_FALLBACK_MODULATIONS = ("V.34", "V.90", "V.92")


class CampaignRunError(RuntimeError):
    """A campaign cannot safely start or retain its evidence."""


class SetupRejected(RuntimeError):
    """A modem rejected a public setup command."""


class _AttemptProgress:
    def __init__(self, case_name: str) -> None:
        self._lock = threading.Lock()
        self._case_name = case_name
        self._stage = "starting"
        self._stage_started = time.monotonic()

    def set(self, stage: str) -> None:
        with self._lock:
            if stage == self._stage:
                return
            self._stage = stage
            self._stage_started = time.monotonic()

    def snapshot(self) -> tuple[str, float]:
        with self._lock:
            label = f"{self._case_name} / {self._stage}"
            seconds = time.monotonic() - self._stage_started
        return label, seconds


class _LiveTicker:
    def __init__(self, refresh: Callable[[], None]) -> None:
        self._refresh = refresh
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="live-campaign-dashboard",
            daemon=True,
        )

    def _run(self) -> None:
        while not self._stop.wait(DASHBOARD_REFRESH_SECONDS):
            try:
                self._refresh()
            except OSError:
                return

    def __enter__(self) -> _LiveTicker:
        self._refresh()
        self._thread.start()
        return self

    def __exit__(self, *_exception: object) -> None:
        self._stop.set()
        self._thread.join()


def _private_directory(path: Path, *, create: bool = True) -> None:
    if create:
        path.mkdir(mode=DIRECTORY_MODE, parents=True, exist_ok=True)
    try:
        attributes = path.lstat()
    except OSError as error:
        raise CampaignRunError("private artifact directory is unavailable") from error
    if (
        not stat.S_ISDIR(attributes.st_mode)
        or stat.S_ISLNK(attributes.st_mode)
        or attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise CampaignRunError(
            "campaign artifacts must be private and owner-owned"
        )


def _private_descriptor(path: Path, *, append: bool = False) -> int:
    flags = os.O_WRONLY | os.O_CREAT
    flags |= os.O_APPEND if append else os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    return os.open(path, flags, FILE_MODE)


def _open_private_text(path: Path, *, append: bool = False) -> IO[str]:
    mode = "a" if append else "w"
    return os.fdopen(
        _private_descriptor(path, append=append), mode, encoding="utf-8"
    )


def _open_private_binary(path: Path) -> IO[bytes]:
    return os.fdopen(_private_descriptor(path), "wb")


def _write_json_once(path: Path, value: Mapping[str, object]) -> None:
    with _open_private_text(path) as stream:
        json.dump(dict(value), stream, indent=2, sort_keys=True)
        stream.write("\n")


def _replace_json(path: Path, value: Mapping[str, object]) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=".summary-", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, FILE_MODE)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            json.dump(dict(value), stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary.exists():
            temporary.unlink()


@dataclass(frozen=True)
class AttemptArtifacts:
    directory: Path
    legs: dict[str, Path]

    @classmethod
    def create(
        cls,
        campaign: Path,
        sequence: int,
        scheduled: ScheduledAttempt,
        physical_try: int,
    ) -> AttemptArtifacts:
        prefix = (
            f"{sequence:04d}-{scheduled.case.name}-"
            f"a{scheduled.ordinal}-p{physical_try}-"
        )
        directory = Path(tempfile.mkdtemp(prefix=prefix, dir=campaign / "attempts"))
        _private_directory(directory, create=False)
        names = (
            ("request", "answer")
            if scheduled.case.role == "callback"
            else ("modem",)
        )
        legs: dict[str, Path] = {}
        for name in names:
            leg = directory / name
            leg.mkdir(mode=DIRECTORY_MODE)
            legs[name] = leg
        return cls(directory, legs)

    def daemon_log(self, leg: str) -> Path:
        return self.legs[leg] / "daemon.log"

    def recording_prefix(self, leg: str) -> Path:
        return self.legs[leg] / "media"

    def write_exchange(self, leg: str, transmitted: bytes, received: bytes) -> None:
        with _open_private_binary(
            self.legs[leg] / "terminal-tx.bin"
        ) as stream:
            stream.write(transmitted)
        with _open_private_binary(
            self.legs[leg] / "terminal-rx.bin"
        ) as stream:
            stream.write(received)

    def write_detail(
        self,
        scheduled: ScheduledAttempt,
        result: AttemptResult,
        setup_trace: Sequence[Mapping[str, str]],
    ) -> None:
        _write_json_once(
            self.directory / "result.json",
            {
                "schema": 1,
                "case": asdict(scheduled.case),
                "result": asdict(result),
                "setup": list(setup_trace),
            },
        )


@dataclass
class CampaignStore:
    directory: Path
    results_file: Path
    summary_file: Path
    _sequence: int = 0

    @classmethod
    def create(
        cls,
        root: Path,
        frozen: FrozenBinary,
        seed: int,
        cases: Sequence[CampaignCase],
        schedule: Sequence[ScheduledAttempt],
        attempts: int,
        failure_extra: int,
        retrain: bool,
    ) -> CampaignStore:
        root = root.absolute()
        _private_directory(root)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = Path(
            tempfile.mkdtemp(
                prefix=f"{stamp}-{frozen.revision[:12]}-",
                dir=root,
            )
        )
        _private_directory(directory, create=False)
        (directory / "attempts").mkdir(mode=DIRECTORY_MODE)
        store = cls(
            directory,
            directory / "results.jsonl",
            directory / "summary.json",
        )
        _write_json_once(
            directory / "manifest.json",
            {
                "schema": 1,
                "started_at": datetime.now(timezone.utc).isoformat(),
                "revision": frozen.revision,
                "binary_sha256": frozen.sha256,
                "seed": seed,
                "attempts_per_case": attempts,
                "failure_extra": failure_extra,
                "retrain": retrain,
                "cases": [asdict(case) for case in cases],
                "base_schedule": [
                    {
                        "case": item.case.name,
                        "ordinal": item.ordinal,
                    }
                    for item in schedule
                ],
            },
        )
        return store

    def new_attempt(
        self, scheduled: ScheduledAttempt, physical_try: int
    ) -> AttemptArtifacts:
        self._sequence += 1
        return AttemptArtifacts.create(
            self.directory, self._sequence, scheduled, physical_try
        )

    def append(self, result: AttemptResult) -> None:
        with _open_private_text(self.results_file, append=True) as stream:
            stream.write(json.dumps(asdict(result), sort_keys=True) + "\n")

    def summary(
        self,
        dashboard: str,
        results: Sequence[AttemptResult],
        *,
        complete: bool,
    ) -> None:
        _replace_json(
            self.summary_file,
            {
                "schema": 1,
                "complete": complete,
                "updated_at": datetime.now(timezone.utc).isoformat(),
                "dashboard": dashboard,
                "physical_results": len(results),
            },
        )


@dataclass
class _AttemptState:
    outcome: str = OUTCOME_COMPLETE
    line_attempted: bool = False
    connected: bool = False
    initial_data: bool = False
    callback_request_attempted: bool = False
    callback_request_maturity: str | None = None
    callback_return_attempted: bool = False
    remote_retrain_nonce: str | None = None
    remote_retrain: bool = False
    retrain_connected: bool = False
    retrain_data: bool = False
    report: ConnectReport = field(default_factory=ConnectReport)
    detail: str = ""
    setup_trace: list[dict[str, str]] = field(default_factory=list)
    captures: dict[str, tuple[bytearray, bytearray]] = field(default_factory=dict)

    def capture(self, leg: str, transmitted: bytes, received: bytes) -> None:
        tx, rx = self.captures.setdefault(leg, (bytearray(), bytearray()))
        tx.extend(transmitted)
        rx.extend(received)


def _endpoint_config(
    frozen: FrozenBinary,
    arguments: Sequence[str],
    commands: Sequence[str],
) -> EndpointConfig:
    return EndpointConfig(
        executable=frozen.executable,
        arguments=tuple(arguments),
        environment=(),
        at_commands=tuple(commands),
    )


def _environment(
    config: CampaignConfig,
    purpose: str,
    recording_prefix: Path,
) -> dict[str, str]:
    values = config.environment(purpose)
    values["SIP_AUTO_ANSWER"] = "0"
    if purpose == "callback-answer":
        values.setdefault("SIP_PORT", "5060")
    else:
        values.setdefault("SIP_PORT", "0")
        values.setdefault("SIP_RTP_PORT", "0")
    return child_environment(values, recording_prefix)


def _setup(
    endpoint: EndpointProcess,
    leg: str,
    timeout_seconds: float,
    trace: list[dict[str, str]],
) -> None:
    assert endpoint.terminal is not None
    deadline = time.monotonic() + timeout_seconds
    for command in endpoint.config.at_commands:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise SetupRejected("setup deadline expired")
        response = endpoint.terminal.command(command, remaining)
        trace.append({"leg": leg, "command": command, "response": response})
        if response != "ok":
            raise SetupRejected(f"{leg} rejected {command}")


def _wake_raw_link(
    terminal: ModemTerminal,
    case: CampaignCase,
    leg: str,
    config: CampaignConfig,
    state: _AttemptState,
    progress: _AttemptProgress,
) -> bool:
    if case.link_mode != LINK_RAW:
        return True
    progress.set("waking raw peer")
    try:
        terminal.send_online(RAW_LINK_WAKE, config.setup_timeout_seconds)
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "raw peer wake-up could not be sent"
        return False
    state.capture(leg, RAW_LINK_WAKE, b"")
    return True


def _connect_rate_error(
    direction: str,
    actual: int | None,
    maximum: int | None,
    minimum: int | None,
) -> str | None:
    if maximum is None:
        return None
    if actual is not None:
        matches = (
            actual == maximum
            if minimum is None
            else minimum <= actual <= maximum
        )
        if matches:
            return None
    actual_text = str(actual) if actual is not None else "unreported"
    expected = str(maximum) if minimum is None else f"{minimum}-{maximum}"
    return f"CONNECT selected {direction} {actual_text} instead of {expected}"


def _connect_error(
    report: ConnectReport,
    modulations: Sequence[str],
    protocol: str,
    receive_rate: int | None,
    transmit_rate: int | None,
    *,
    minimum_receive_rate: int | None = None,
    minimum_transmit_rate: int | None = None,
) -> str | None:
    if report.modulation not in modulations:
        expected = "/".join(modulations)
        actual = report.modulation or "unreported"
        return f"CONNECT selected {actual} instead of {expected}"
    if report.protocol != protocol:
        actual = report.protocol or "unreported"
        return f"CONNECT selected {actual} protocol instead of {protocol}"
    rate_error = _connect_rate_error(
        "RX", report.receive_bit_rate, receive_rate, minimum_receive_rate
    ) or _connect_rate_error(
        "TX", report.transmit_bit_rate, transmit_rate, minimum_transmit_rate
    )
    if rate_error is not None:
        return rate_error
    return None


def _case_connect_error(
    case: CampaignCase, report: ConnectReport
) -> str | None:
    modulations = (
        ANALOGUE_FALLBACK_MODULATIONS
        if case.feature == "analogue-fallback"
        else FAMILY_CONNECT_MODULATIONS[case.family]
    )
    protocol = "none" if case.link_mode == LINK_RAW else "LAPM"
    return _connect_error(
        report,
        modulations,
        protocol,
        case.receive_rate,
        case.transmit_rate,
        minimum_receive_rate=case.minimum_receive_rate,
        minimum_transmit_rate=case.minimum_transmit_rate,
    )


def _exercise_online(
    terminal: ModemTerminal,
    state: _AttemptState,
    leg: str,
    config: CampaignConfig,
    retrain: bool,
    progress: _AttemptProgress,
    *,
    initial_verified: bool | None = None,
) -> None:
    if initial_verified is None:
        progress.set("checking initial data")
        try:
            time.sleep(config.online_settle_seconds)
            verified, received = terminal.probe_initial_data(
                config.online_timeout_seconds
            )
            state.capture(leg, b"", received)
        except (LiveTerminalError, OSError, TimeoutError):
            state.detail = "initial online exchange failed"
            return
        state.initial_data = verified
    else:
        state.initial_data = initial_verified
    if not state.initial_data:
        if not state.detail:
            state.detail = "initial online data was not verified"
        return
    if not retrain:
        return
    remote_deadline = time.monotonic() + config.retrain_timeout_seconds
    progress.set("waiting for remote retrain START")
    try:
        nonce, received = terminal.start_remote_retrain(
            min(
                config.online_timeout_seconds,
                max(0.001, remote_deadline - time.monotonic()),
            )
        )
        state.capture(leg, REMOTE_RETRAIN_REQUEST, received)
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "remote retrain START exchange failed"
        return
    if nonce is None:
        state.detail = "remote retrain START marker was not verified"
        return
    state.remote_retrain_nonce = nonce.decode("ascii")
    progress.set("waiting for remote retrain END")
    remaining = remote_deadline - time.monotonic()
    if remaining <= 0:
        state.detail = "remote retrain exceeded its total deadline"
        return
    try:
        report = terminal.complete_remote_retrain(
            nonce,
            received,
            remaining,
        )
        state.remote_retrain = report.complete
        state.capture(leg, b"", report.continuation)
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "remote retrain END exchange failed"
        return
    if not state.remote_retrain:
        state.detail = "remote retrain END marker was not verified"
        return
    progress.set("waiting for local retrain")
    try:
        state.capture(leg, b"+++ATO1\r", b"")
        report = terminal.return_online(config.retrain_timeout_seconds)
        state.retrain_connected = report.connected
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "retrain did not return online"
        return
    if not state.retrain_connected:
        if report.escape_result != "ok":
            state.detail = (
                "escape sequence returned " + report.escape_result
            )
        elif report.command_result != "ok":
            state.detail = "ATO1 returned " + str(report.command_result)
        else:
            state.detail = "retrain returned " + str(report.retrain_result)
        return
    progress.set("checking post-retrain data")
    try:
        time.sleep(config.online_settle_seconds)
        verified, received = terminal.probe_online_ping(
            config.online_timeout_seconds
        )
        state.capture(leg, ONLINE_PING_REQUEST, received)
        state.retrain_data = verified
        if not verified:
            state.detail = "post-retrain online data was not verified"
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "post-retrain online exchange failed"


def _stop_endpoint(endpoint: EndpointProcess, timeout_seconds: float) -> bool:
    hangup_ok = True
    try:
        if endpoint.terminal is not None:
            hangup_ok = endpoint.terminal.escape_and_hang_up()
    finally:
        stopped = endpoint.stop(timeout_seconds)
    return hangup_ok and stopped


def _outbound_body(
    config: CampaignConfig,
    frozen: FrozenBinary,
    scheduled: ScheduledAttempt,
    artifacts: AttemptArtifacts,
    state: _AttemptState,
    retrain: bool,
    endpoints: list[EndpointProcess],
    progress: _AttemptProgress,
) -> None:
    endpoint = EndpointProcess(
        "originator",
        _endpoint_config(
            frozen,
            config.candidate.arguments,
            scheduled.case.at_commands,
        ),
        _environment(config, "outbound", artifacts.recording_prefix("modem")),
        artifacts.daemon_log("modem"),
    )
    endpoints.append(endpoint)
    progress.set("starting modem")
    endpoint.start(config.startup_timeout_seconds)
    progress.set("configuring modem")
    _setup(
        endpoint,
        "modem",
        config.setup_timeout_seconds,
        state.setup_trace,
    )
    assert endpoint.terminal is not None
    progress.set("waiting for CONNECT")
    state.line_attempted = True
    try:
        response = endpoint.terminal.dial_until(
            time.monotonic() + config.connect_timeout_seconds
        )
    except TimeoutError:
        state.detail = "outbound CONNECT timed out"
        return None
    if response == "busy":
        state.outcome = OUTCOME_BUSY
        state.detail = "external line was busy"
        return None
    if response != "connect":
        state.detail = f"outbound call ended with {response}"
        return None
    state.connected = True
    state.report = endpoint.terminal.connect_report
    state.detail = _case_connect_error(scheduled.case, state.report) or ""
    if state.detail:
        return
    if not _wake_raw_link(
        endpoint.terminal,
        scheduled.case,
        "modem",
        config,
        state,
        progress,
    ):
        return
    _exercise_online(
        endpoint.terminal,
        state,
        "modem",
        config,
        retrain,
        progress,
    )


def _callback_command(case: CampaignCase) -> bytes:
    suffix = b""
    if case.callback_features:
        suffix = b" " + b" ".join(
            feature.encode("ascii") for feature in case.callback_features
        )
    return b"\r\n\r\nDIAL" + suffix + b"\r\n"


def _callback_body(
    config: CampaignConfig,
    frozen: FrozenBinary,
    scheduled: ScheduledAttempt,
    artifacts: AttemptArtifacts,
    state: _AttemptState,
    retrain: bool,
    endpoints: list[EndpointProcess],
    stopped: set[int],
    progress: _AttemptProgress,
) -> None:
    answer_commands = tuple(scheduled.case.at_commands[:-2]) + (
        "ATS0=0",
        *scheduled.case.at_commands[-2:],
    )
    answer = EndpointProcess(
        "answerer",
        _endpoint_config(
            frozen,
            config.candidate.arguments,
            answer_commands,
        ),
        _environment(
            config, "callback-answer", artifacts.recording_prefix("answer")
        ),
        artifacts.daemon_log("answer"),
    )
    request = EndpointProcess(
        "originator",
        _endpoint_config(
            frozen,
            config.candidate.arguments,
            CALLBACK_REQUEST_COMMANDS,
        ),
        _environment(
            config, "callback-request", artifacts.recording_prefix("request")
        ),
        artifacts.daemon_log("request"),
    )
    endpoints.extend((answer, request))
    progress.set("starting callback answerer")
    answer.start(config.startup_timeout_seconds)
    progress.set("configuring callback answerer")
    _setup(
        answer,
        "answer",
        config.setup_timeout_seconds,
        state.setup_trace,
    )
    state.callback_request_attempted = True
    progress.set("starting callback request")
    request.start(config.startup_timeout_seconds)
    progress.set("configuring callback request")
    _setup(
        request,
        "request",
        config.setup_timeout_seconds,
        state.setup_trace,
    )
    assert request.terminal is not None
    progress.set("waiting for request CONNECT")
    state.line_attempted = True
    try:
        response = request.terminal.dial_until(
            time.monotonic() + config.callback_request_timeout_seconds
        )
    except TimeoutError:
        state.callback_request_maturity = MATURITY_NO_CONNECT
        state.detail = "callback request CONNECT timed out"
        return None
    if response == "busy":
        state.outcome = OUTCOME_BUSY
        state.detail = "external line was busy"
        return None
    if response != "connect":
        state.callback_request_maturity = MATURITY_NO_CONNECT
        state.detail = f"callback request ended with {response}"
        return None
    state.callback_request_maturity = MATURITY_CONNECT
    request_error = _connect_error(
        request.terminal.connect_report,
        ("V.22bis",),
        "LAPM",
        2400,
        2400,
    )
    if request_error is not None:
        state.detail = "callback request " + request_error
        return None
    callback_command = _callback_command(scheduled.case)
    progress.set("waiting for callback request greeting")
    try:
        greeting_verified, received = request.terminal.probe_initial_data(
            config.callback_marker_timeout_seconds
        )
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "callback request greeting was not received"
        return None
    state.capture("request", b"", received)
    if not greeting_verified:
        state.detail = "callback request greeting was not verified"
        return None
    progress.set("sending callback request")
    try:
        request.terminal.send_online(
            callback_command,
            config.setup_timeout_seconds,
        )
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "callback request could not be sent"
        return None
    state.capture("request", callback_command, b"")
    progress.set("waiting for callback START")
    try:
        verified, received = request.terminal.exchange_online_pattern(
            b"",
            CALLBACK_START_RE,
            config.callback_marker_timeout_seconds,
        )
    except (LiveTerminalError, OSError, TimeoutError):
        state.detail = "callback START marker was not received"
        return None
    state.capture("request", b"", received)
    match = CALLBACK_START_RE.search(received) if verified else None
    if match is None:
        state.detail = "callback START marker was not verified"
        return None
    nonce = match.group("nonce").lower()
    state.callback_request_maturity = MATURITY_REQUEST
    progress.set("releasing request call")
    if not _stop_endpoint(request, config.cleanup_timeout_seconds):
        state.detail = "callback request hangup or cleanup did not complete"
        return None
    stopped.add(id(request))
    time.sleep(config.callback_release_seconds)
    assert answer.terminal is not None
    callback_deadline = (
        time.monotonic() + config.callback_return_timeout_seconds
    )
    progress.set("waiting for callback RING")
    try:
        response = answer.terminal.wait_ring_until(callback_deadline)
    except TimeoutError:
        state.detail = "returning callback RING timed out"
        return None
    if response != "ring":
        state.detail = f"returning callback ended before RING with {response}"
        return None
    progress.set("answering callback")
    try:
        answer.terminal.start_answer(callback_deadline)
    except TimeoutError:
        state.detail = "callback answer command timed out"
        return None
    state.callback_return_attempted = True
    progress.set("waiting for callback CONNECT")
    try:
        response = answer.terminal.wait_connect_until(callback_deadline)
    except TimeoutError:
        state.detail = "returning callback CONNECT timed out"
        return None
    if response != "connect":
        state.detail = f"returning callback training ended with {response}"
        return None
    state.connected = True
    state.report = answer.terminal.connect_report
    state.detail = _case_connect_error(scheduled.case, state.report) or ""
    if state.detail:
        return
    if not _wake_raw_link(
        answer.terminal,
        scheduled.case,
        "answer",
        config,
        state,
        progress,
    ):
        return
    connect_pattern = re.compile(
        rb"GETTY:CALLBACK:CONNECT:"
        + re.escape(nonce)
        + rb":[^\r\n]*(?:\r\n|\r|\n)"
    )
    progress.set("verifying callback nonce")
    try:
        marker_verified, received = answer.terminal.exchange_online_pattern(
            b"", connect_pattern, config.online_timeout_seconds
        )
        state.capture("answer", b"", received)
    except (LiveTerminalError, OSError, TimeoutError):
        marker_verified = False
        state.detail = "callback CONNECT marker was not received"
    _exercise_online(
        answer.terminal,
        state,
        "answer",
        config,
        retrain,
        progress,
        initial_verified=marker_verified,
    )


def run_physical_attempt(
    config: CampaignConfig,
    frozen: FrozenBinary,
    scheduled: ScheduledAttempt,
    physical_try: int,
    artifacts: AttemptArtifacts,
    retrain: bool,
    progress: _AttemptProgress | None = None,
) -> AttemptResult:
    if progress is None:
        progress = _AttemptProgress(scheduled.case.name)
    state = _AttemptState()
    endpoints: list[EndpointProcess] = []
    stopped: set[int] = set()
    started = time.monotonic()
    try:
        if scheduled.case.role == "originator":
            _outbound_body(
                config,
                frozen,
                scheduled,
                artifacts,
                state,
                retrain,
                endpoints,
                progress,
            )
        else:
            _callback_body(
                config,
                frozen,
                scheduled,
                artifacts,
                state,
                retrain,
                endpoints,
                stopped,
                progress,
            )
    except SetupRejected as error:
        state.outcome = OUTCOME_SETUP_ERROR
        state.detail = str(error)
    except (InteropRunError, LiveTerminalError, OSError):
        state.outcome = OUTCOME_INFRASTRUCTURE_ERROR
        state.detail = "modem process or terminal infrastructure failed"
    finally:
        progress.set("cleaning up")
        cleanup_ok = True
        for endpoint in reversed(endpoints):
            if id(endpoint) in stopped:
                continue
            cleanup_ok = (
                _stop_endpoint(endpoint, config.cleanup_timeout_seconds) and cleanup_ok
            )
        if not cleanup_ok:
            if state.detail:
                state.detail += "; "
            state.detail += "modem hangup or cleanup did not complete"

    result = AttemptResult(
        case_name=scheduled.case.name,
        family=scheduled.case.family,
        role=scheduled.case.role,
        ordinal=scheduled.ordinal,
        extra=scheduled.extra,
        physical_try=physical_try,
        outcome=state.outcome,
        link_mode=scheduled.case.link_mode,
        line_attempted=state.line_attempted,
        connected=state.connected,
        initial_data=state.initial_data,
        callback_request_attempted=state.callback_request_attempted,
        callback_request_maturity=state.callback_request_maturity,
        callback_return_attempted=state.callback_return_attempted,
        remote_retrain_nonce=state.remote_retrain_nonce,
        remote_retrain=state.remote_retrain,
        retrain_connected=state.retrain_connected,
        retrain_data=state.retrain_data,
        retrain_requested=retrain,
        duration_seconds=round(time.monotonic() - started, 3),
        modulation=state.report.modulation,
        protocol=state.report.protocol,
        compression=state.report.compression,
        receive_rate=state.report.receive_bit_rate,
        transmit_rate=state.report.transmit_bit_rate,
        detail=state.detail,
        artifact=artifacts.directory.name,
    )
    for leg in artifacts.legs:
        tx, rx = state.captures.get(leg, (bytearray(), bytearray()))
        artifacts.write_exchange(leg, bytes(tx), bytes(rx))
    artifacts.write_detail(scheduled, result, state.setup_trace)
    return result


def _selected(arguments: argparse.Namespace) -> tuple[CampaignCase, ...]:
    unknown = set(arguments.case) - set(CASES_BY_NAME)
    if unknown:
        raise CampaignRunError(f"unknown case: {sorted(unknown)[0]}")
    suite = "extended" if arguments.all else arguments.suite
    roles = list(arguments.role)
    if arguments.callbacks:
        roles.append("callback")
    if arguments.originators:
        roles.append("originator")
    selected = select_cases(
        suite,
        names=arguments.case,
        families=arguments.modulation,
        roles=roles,
        rates=arguments.rate,
        symbol_rates=arguments.baud,
        features=arguments.feature,
    )
    if not selected:
        raise CampaignRunError("campaign selectors matched no cases")
    return selected


def _list_cases(cases: Sequence[CampaignCase]) -> None:
    print("case modulation role line-role rx tx baud feature")
    for case in cases:
        print(
            f"{case.name} {case.family} {case.role} {case.line_role} "
            f"{case.receive_rate or '-'} {case.transmit_rate or '-'} "
            f"{case.symbol_rate or '-'} {case.feature}"
        )


def _display(
    cases: Sequence[CampaignCase],
    attempts: int,
    results: Sequence[AttemptResult],
    *,
    revision: str,
    seed: int,
    started: float,
    current: str,
    current_seconds: float | None = None,
    live: bool,
) -> str:
    arguments = dict(
        revision=revision,
        seed=seed,
        elapsed_seconds=time.monotonic() - started,
        current=current,
        current_seconds=current_seconds,
    )
    dashboard = render_dashboard(
        cases,
        attempts,
        results,
        **arguments,
    )
    terminal_lines = shutil.get_terminal_size(fallback=(80, 24)).lines
    recent_limit = max(0, terminal_lines - len(dashboard.splitlines()) - 3)
    if recent_limit > 0:
        dashboard = render_dashboard(
            cases,
            attempts,
            results,
            recent_limit=recent_limit,
            **arguments,
        )
    if live:
        sys.stdout.write("\x1b[2J\x1b[H" + dashboard + "\n")
        sys.stdout.flush()
    return dashboard


def _freeze_with_status(
    build: RevisionBuild,
    cache: Path,
    *,
    live_display: bool,
) -> FrozenBinary:
    progress = _AttemptProgress("revision build")
    requested = build.revision
    if len(requested) > 48:
        requested = requested[:45] + "..."

    def refresh() -> None:
        current, seconds = progress.snapshot()
        dashboard = "\n".join(
            (
                "Live modem campaign",
                f"requested revision: {requested}",
                f"current: {current} for={seconds:.0f}s",
                "First use may compile the pinned SIP stack; exact-revision "
                "builds are cached.",
            )
        )
        sys.stdout.write("\x1b[2J\x1b[H" + dashboard + "\n")
        sys.stdout.flush()

    def report(stage: str) -> None:
        progress.set(stage)
        if not live_display:
            print(f"revision build: {stage}", flush=True)

    if live_display:
        sys.stdout.write("\x1b[?25l")
        sys.stdout.flush()
    try:
        ticker = _LiveTicker(refresh) if live_display else nullcontext()
        with ticker:
            return freeze_revision_build(build, cache, progress=report)
    finally:
        if live_display:
            sys.stdout.write("\x1b[?25h\n")
            sys.stdout.flush()


def _run_schedule(
    schedule: Sequence[ScheduledAttempt],
    config: CampaignConfig,
    frozen: FrozenBinary,
    store: CampaignStore,
    cases: Sequence[CampaignCase],
    attempts: int,
    seed: int,
    retrain: bool,
    results: list[AttemptResult],
    started: float,
    live_display: bool,
    *,
    initial_cooldown: bool = False,
) -> None:
    first_physical_attempt = True
    for scheduled in schedule:
        physical_try = 1
        while True:
            progress = _AttemptProgress(scheduled.case.name)

            def refresh() -> None:
                current, seconds = progress.snapshot()
                _display(
                    cases,
                    attempts,
                    results,
                    revision=frozen.revision,
                    seed=seed,
                    started=started,
                    current=current,
                    current_seconds=seconds,
                    live=True,
                )

            def wait_with_status(seconds: float, stage: str) -> None:
                if seconds <= 0:
                    return
                progress.set(stage)
                ticker = _LiveTicker(refresh) if live_display else nullcontext()
                with ticker:
                    time.sleep(seconds)

            if first_physical_attempt and initial_cooldown:
                wait_with_status(
                    config.inter_call_seconds,
                    "initial inter-call cooldown",
                )
            first_physical_attempt = False
            artifacts = store.new_attempt(scheduled, physical_try)
            ticker = _LiveTicker(refresh) if live_display else nullcontext()
            with ticker:
                result = run_physical_attempt(
                    config,
                    frozen,
                    scheduled,
                    physical_try,
                    artifacts,
                    retrain,
                    progress,
                )
            results.append(result)
            store.append(result)
            if not live_display:
                if result.callback_request_only:
                    request_status = (
                        result.callback_request_maturity or result.outcome
                    )
                    status = f"request-{request_status}"
                else:
                    status = result.maturity or result.outcome
                print(
                    f"{scheduled.case.name} attempt={scheduled.ordinal} "
                    f"physical={physical_try} status={status} "
                    f"duration={result.duration_seconds:.1f}s",
                    flush=True,
                )
            progress.set("attempt complete")
            current, seconds = progress.snapshot()
            dashboard = _display(
                cases,
                attempts,
                results,
                revision=frozen.revision,
                seed=seed,
                started=started,
                current=current,
                current_seconds=seconds,
                live=live_display,
            )
            store.summary(dashboard, results, complete=False)
            if result.outcome != OUTCOME_BUSY:
                if result.line_attempted:
                    wait_with_status(
                        config.inter_call_seconds,
                        "inter-call cooldown",
                    )
                break
            if physical_try > config.busy_retry_limit:
                unavailable = replace(
                    result,
                    outcome=OUTCOME_UNAVAILABLE,
                    detail="BUSY retry limit exhausted",
                )
                results.append(unavailable)
                store.append(unavailable)
                wait_with_status(
                    config.inter_call_seconds,
                    "inter-call cooldown",
                )
                break
            physical_try += 1
            wait_with_status(config.busy_retry_seconds, "BUSY retry cooldown")


def run_campaign(
    config: CampaignConfig,
    frozen: FrozenBinary,
    cases: Sequence[CampaignCase],
    *,
    attempts: int,
    failure_extra: int,
    seed: int,
    randomize: bool,
    retrain: bool,
    live_display: bool,
) -> tuple[CampaignStore, list[AttemptResult]]:
    if live_display:
        sys.stdout.write("\x1b[?25l")
        sys.stdout.flush()
    try:
        schedule = base_schedule(cases, attempts, seed, randomize=randomize)
        store = CampaignStore.create(
            config.artifact_directory,
            frozen,
            seed,
            cases,
            schedule,
            attempts,
            failure_extra,
            retrain,
        )
        results: list[AttemptResult] = []
        started = time.monotonic()
        try:
            _run_schedule(
                schedule,
                config,
                frozen,
                store,
                cases,
                attempts,
                seed,
                retrain,
                results,
                started,
                live_display,
                initial_cooldown=True,
            )
            extras = extra_schedule(
                cases, results, attempts, failure_extra, seed
            )
            _run_schedule(
                extras,
                config,
                frozen,
                store,
                cases,
                attempts,
                seed,
                retrain,
                results,
                started,
                live_display,
            )
        except KeyboardInterrupt:
            dashboard = _display(
                cases,
                attempts,
                results,
                revision=frozen.revision,
                seed=seed,
                started=started,
                current="interrupted",
                live=live_display,
            )
            store.summary(dashboard, results, complete=False)
            raise
        dashboard = _display(
            cases,
            attempts,
            results,
            revision=frozen.revision,
            seed=seed,
            started=started,
            current="complete",
            live=live_display,
        )
        if not live_display:
            print(dashboard)
        store.summary(dashboard, results, complete=True)
        return store, results
    finally:
        if live_display:
            sys.stdout.write("\x1b[?25h\n")
            sys.stdout.flush()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "With an enabled owner-only .live-campaign.json, the default "
            "command runs the three-attempt standard suite."
        ),
    )
    parser.add_argument("--config", type=Path, default=Path(".live-campaign.json"))
    parser.add_argument(
        "--suite", choices=("smoke", "standard", "extended"), default="standard"
    )
    parser.add_argument(
        "--all",
        "--whole-enchilada",
        action="store_true",
        help="select the extended suite",
    )
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument(
        "--modulation", choices=FAMILY_ORDER, action="append", default=[]
    )
    parser.add_argument("--role", choices=ROLES, action="append", default=[])
    parser.add_argument("--callbacks", action="store_true")
    parser.add_argument("--originators", action="store_true")
    parser.add_argument("--rate", "--datarate", type=int, action="append", default=[])
    parser.add_argument("--baud", type=int, action="append", default=[])
    parser.add_argument("--feature", action="append", default=[])
    parser.add_argument("--attempts", type=int, default=3)
    parser.add_argument("--failure-extra", type=int, default=1)
    parser.add_argument("--seed", type=int)
    parser.add_argument(
        "--ordered",
        action="store_true",
        help="preserve profile order instead of balancing completion",
    )
    parser.add_argument("--no-retrain", action="store_true")
    parser.add_argument("--revision")
    parser.add_argument("--list", action="store_true", help="list selected cases only")
    parser.add_argument(
        "--prepare", action="store_true", help="freeze the binary but make no calls"
    )
    parser.add_argument("--plain", action="store_true", help="do not repaint a TTY")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        cases = _selected(arguments)
        if not 1 <= arguments.attempts <= 20:
            raise CampaignRunError("attempts must be between 1 and 20")
        if not 0 <= arguments.failure_extra <= 5:
            raise CampaignRunError("failure-extra must be between 0 and 5")
        if arguments.list:
            _list_cases(cases)
            print(
                f"cases={len(cases)} base_attempts="
                f"{len(cases) * arguments.attempts} maximum_failure_extras="
                f"{len(cases) * arguments.failure_extra}"
            )
            return 0
        if not arguments.config.exists():
            print("live campaign skipped: private configuration is absent")
            return SKIPPED_EXIT_CODE
        config = load_campaign_config(arguments.config)
        if not config.enabled:
            print("live campaign skipped: private configuration is disabled")
            return SKIPPED_EXIT_CODE
        candidate = config.candidate
        if arguments.revision is not None:
            candidate = replace(candidate, revision=arguments.revision)
        live_display = sys.stdout.isatty() and not arguments.plain
        frozen = _freeze_with_status(
            RevisionBuild(
                label="candidate",
                repository=candidate.repository,
                revision=candidate.revision,
                command=candidate.command,
                executable=candidate.executable,
            ),
            config.build_cache,
            live_display=live_display,
        )
        seed = arguments.seed
        if seed is None:
            seed = secrets.randbits(63)
        maximum = len(cases) * (
            arguments.attempts + arguments.failure_extra
        )
        print(
            f"revision={frozen.revision} binary={frozen.sha256} "
            f"cases={len(cases)} base={len(cases) * arguments.attempts} "
            f"maximum_logical={maximum} seed={seed}",
            flush=True,
        )
        if arguments.prepare:
            print(f"frozen_build={frozen.directory.name}")
            return 0
        store, _results = run_campaign(
            config,
            frozen,
            cases,
            attempts=arguments.attempts,
            failure_extra=arguments.failure_extra,
            seed=seed,
            randomize=not arguments.ordered,
            retrain=not arguments.no_retrain,
            live_display=live_display,
        )
        print(f"campaign={store.directory.name}")
        return 0
    except KeyboardInterrupt:
        print("live campaign interrupted", file=sys.stderr)
        return 130
    except (
        CampaignConfigError,
        CampaignRunError,
        RevisionBuildError,
        ValueError,
    ) as error:
        print(f"live campaign error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
