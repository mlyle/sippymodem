#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run explicitly enabled SIP scenarios without publishing private settings."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
from typing import IO, Mapping, Sequence


if __package__:
    from .live_config import (
        CallScenario,
        CONFIG_ENVIRONMENT_KEYS,
        LiveCallConfig,
        LiveCallConfigError,
        ScenarioPlan,
        expand_plan,
        load_live_call_config,
        public_manifest,
    )
    from .live_terminal import (
        ConnectReport,
        LiveTerminalError,
        ModemTerminal,
        ONLINE_PING_REQUEST,
        read_published_device,
    )
else:
    from live_config import (
        CallScenario,
        CONFIG_ENVIRONMENT_KEYS,
        LiveCallConfig,
        LiveCallConfigError,
        ScenarioPlan,
        expand_plan,
        load_live_call_config,
        public_manifest,
    )
    from live_terminal import (
        ConnectReport,
        LiveTerminalError,
        ModemTerminal,
        ONLINE_PING_REQUEST,
        read_published_device,
    )


PRIVATE_RUN_ENVIRONMENT_KEYS = CONFIG_ENVIRONMENT_KEYS | frozenset(
    {"SIP_RECORD_PREFIX"}
)
SKIPPED_EXIT_CODE = 77


class LiveCallRunError(RuntimeError):
    """A live-call prerequisite or private runtime boundary failed."""


@dataclass(frozen=True)
class CallResult:
    scenario_name: str
    plan_name: str
    profile_name: str
    family: str
    link_mode: str
    attempt: int
    readiness: str
    prerequisite: str | None
    outcome: str
    duration_seconds: float
    terminal_expected_bytes: int = 0
    terminal_verified_bytes: int = 0
    terminal_transmitted_bytes: int = 0
    round_trip_probe: str | None = None
    round_trip_verified: bool = False
    modulation: str | None = None
    protocol: str | None = None
    compression: str | None = None
    receive_bit_rate: int | None = None
    transmit_bit_rate: int | None = None

    def public_dict(self) -> dict[str, object]:
        return {
            "scenario": self.scenario_name,
            "plan": self.plan_name,
            "profile": self.profile_name,
            "family": self.family,
            "link_mode": self.link_mode,
            "attempt": self.attempt,
            "readiness": self.readiness,
            "prerequisite": self.prerequisite,
            "outcome": self.outcome,
            "duration_seconds": self.duration_seconds,
            "terminal_expected_bytes": self.terminal_expected_bytes,
            "terminal_verified_bytes": self.terminal_verified_bytes,
            "terminal_transmitted_bytes": self.terminal_transmitted_bytes,
            "round_trip_probe": self.round_trip_probe,
            "round_trip_verified": self.round_trip_verified,
            "modulation": self.modulation,
            "protocol": self.protocol,
            "compression": self.compression,
            "receive_bit_rate": self.receive_bit_rate,
            "transmit_bit_rate": self.transmit_bit_rate,
        }


@dataclass(frozen=True)
class AttemptArtifacts:
    directory: Path
    daemon_log: Path
    result_file: Path
    recording_prefix: Path
    terminal_rx: Path
    terminal_tx: Path

    @classmethod
    def create(cls, root: Path, scenario_name: str) -> AttemptArtifacts:
        private_root = root.absolute()
        private_root.mkdir(mode=0o700, parents=True, exist_ok=True)
        _require_private_directory(private_root)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = Path(
            tempfile.mkdtemp(prefix=f"{stamp}-{scenario_name}-", dir=private_root)
        )
        _require_private_directory(directory)
        return cls(
            directory=directory,
            daemon_log=directory / "daemon.log",
            result_file=directory / "result.json",
            recording_prefix=directory / "media",
            terminal_rx=directory / "terminal-rx.bin",
            terminal_tx=directory / "terminal-tx.bin",
        )

    def write_result(self, result: CallResult) -> None:
        payload = json.dumps(result.public_dict(), sort_keys=True) + "\n"
        with _open_private_text(self.result_file) as stream:
            stream.write(payload)

    def write_terminal_exchange(self, transmitted: bytes, received: bytes) -> None:
        with _open_private_binary(self.terminal_tx) as stream:
            stream.write(transmitted)
        with _open_private_binary(self.terminal_rx) as stream:
            stream.write(received)


def _require_private_directory(path: Path) -> None:
    try:
        attributes = path.lstat()
    except OSError as error:
        raise LiveCallRunError("private artifact directory is unavailable") from error
    if (
        not stat.S_ISDIR(attributes.st_mode)
        or stat.S_ISLNK(attributes.st_mode)
        or attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise LiveCallRunError("artifact directory must be private and owner-owned")


def _open_private_text(path: Path) -> IO[str]:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(path, flags, 0o600)
    return os.fdopen(descriptor, "w", encoding="utf-8")


def _open_private_binary(path: Path) -> IO[bytes]:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(path, flags, 0o600)
    return os.fdopen(descriptor, "wb")


def build_child_environment(
    config: LiveCallConfig,
    recording_prefix: Path,
    base_environment: Mapping[str, str] | None = None,
) -> dict[str, str]:
    environment = dict(os.environ if base_environment is None else base_environment)
    for key in PRIVATE_RUN_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    environment.update(config.environment_dict())
    environment.setdefault("SIP_PORT", "0")
    environment.setdefault("SIP_RTP_PORT", "0")
    environment["SIP_RECORD_PREFIX"] = str(recording_prefix.absolute())
    return environment


def selected_scenarios(
    config: LiveCallConfig, requested_names: Sequence[str]
) -> tuple[CallScenario, ...]:
    enabled = {plan.name: plan for plan in config.plans if plan.enabled}
    if not requested_names:
        plans: tuple[ScenarioPlan, ...] = tuple(
            plan for plan in config.plans if plan.enabled
        )
    elif len(requested_names) != len(set(requested_names)):
        raise LiveCallRunError("a scenario plan was selected more than once")
    else:
        if any(name not in enabled for name in requested_names):
            raise LiveCallRunError(
                "a selected scenario plan is unavailable or disabled"
            )
        plans = tuple(enabled[name] for name in requested_names)
    return tuple(
        scenario for plan in plans for scenario in expand_plan(config, plan)
    )


class LiveCallAttempt:
    """Own all process, terminal, and artifact state for one scenario."""

    def __init__(self, config: LiveCallConfig, scenario: CallScenario) -> None:
        if scenario.readiness != "runnable":
            raise LiveCallRunError("scenario has an unmet public prerequisite")
        self._config = config
        self._scenario = scenario
        self._artifacts = AttemptArtifacts.create(
            config.artifact_directory, scenario.name
        )
        self._process: subprocess.Popen[bytes] | None = None
        self._terminal: ModemTerminal | None = None
        self._daemon_log: IO[str] | None = None

    def run(self) -> CallResult:
        started = time.monotonic()
        outcome = "infrastructure-error"
        expected_bytes = len(self._scenario.online_expect)
        verified_bytes = 0
        transmitted = b""
        round_trip_verified = False
        connect_report = ConnectReport()
        stage = "startup"
        try:
            self._start()
            stage = "setup"
            assert self._terminal is not None
            for command in self._scenario.at_commands:
                if (
                    self._terminal.command(
                        command, self._config.startup_timeout_seconds
                    )
                    != "ok"
                ):
                    outcome = "setup-error"
                    break
            else:
                stage = "call"
                response = self._terminal.command(
                    "ATD",
                    self._config.call_timeout_seconds,
                )
                if response == "connect":
                    connect_report = self._terminal.connect_report
                    if self._scenario.online_expect:
                        stage = "data"
                        data_verified, received = self._terminal.exchange_online(
                            self._scenario.online_send,
                            self._scenario.online_expect,
                            self._config.online_timeout_seconds,
                        )
                        transmitted = self._scenario.online_send
                        if data_verified:
                            verified_bytes = expected_bytes
                        if (
                            data_verified
                            and self._scenario.round_trip_probe == "ping"
                        ):
                            stage = "round-trip"
                            round_trip_verified, probe_received = (
                                self._terminal.probe_online_ping(
                                    self._config.online_timeout_seconds
                                )
                            )
                            transmitted += ONLINE_PING_REQUEST
                            received += probe_received
                        self._artifacts.write_terminal_exchange(
                            transmitted, received
                        )
                        if not data_verified:
                            outcome = "data-missing"
                        elif (
                            self._scenario.round_trip_probe
                            and not round_trip_verified
                        ):
                            outcome = "round-trip-missing"
                        else:
                            outcome = "connected"
                    else:
                        outcome = "connected"
                    time.sleep(self._config.hold_seconds)
                    self._terminal.escape_and_hang_up()
                elif response == "ok":
                    outcome = "unexpected-ok"
                else:
                    outcome = response
        except TimeoutError:
            outcome = f"{stage}-timeout"
        except (
            LiveCallRunError,
            LiveTerminalError,
            OSError,
            subprocess.SubprocessError,
        ):
            outcome = f"{stage}-failed"
        finally:
            self._close()
        result = CallResult(
            scenario_name=self._scenario.name,
            plan_name=self._scenario.plan_name,
            profile_name=self._scenario.profile_name,
            family=self._scenario.family,
            link_mode=self._scenario.link_mode,
            attempt=self._scenario.attempt,
            readiness=self._scenario.readiness,
            prerequisite=self._scenario.prerequisite or None,
            outcome=outcome,
            duration_seconds=round(time.monotonic() - started, 3),
            terminal_expected_bytes=expected_bytes,
            terminal_verified_bytes=verified_bytes,
            terminal_transmitted_bytes=len(transmitted),
            round_trip_probe=self._scenario.round_trip_probe or None,
            round_trip_verified=round_trip_verified,
            modulation=connect_report.modulation,
            protocol=connect_report.protocol,
            compression=connect_report.compression,
            receive_bit_rate=connect_report.receive_bit_rate,
            transmit_bit_rate=connect_report.transmit_bit_rate,
        )
        self._artifacts.write_result(result)
        return result

    def _start(self) -> None:
        executable = self._config.slmodemd.absolute()
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise LiveCallRunError("modem executable is unavailable")
        self._daemon_log = _open_private_text(self._artifacts.daemon_log)
        environment = build_child_environment(
            self._config,
            self._artifacts.recording_prefix,
        )
        self._process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=self._daemon_log,
            env=environment,
            close_fds=True,
            start_new_session=True,
        )
        if self._process.stdout is None:
            raise LiveTerminalError("modem did not publish a terminal")
        device_path = read_published_device(
            self._process.stdout, self._config.startup_timeout_seconds
        )
        self._terminal = ModemTerminal.open(device_path)

    def _close(self) -> None:
        if self._terminal is not None:
            try:
                self._terminal.close()
            except OSError:
                pass
            self._terminal = None
        if self._process is not None:
            process = self._process
            self._process = None
            try:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass
                else:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
            if process.stdout is not None:
                process.stdout.close()
        if self._daemon_log is not None:
            self._daemon_log.close()
            self._daemon_log = None


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run private, explicitly enabled modem live-call plans."
    )
    parser.add_argument("--config", default=".live-call.json")
    parser.add_argument(
        "--list", action="store_true", help="print only redacted plan metadata"
    )
    parser.add_argument(
        "--plan",
        action="append",
        default=[],
        metavar="NAME",
        help="run one enabled named plan; may be repeated",
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    options = _argument_parser().parse_args(arguments)
    try:
        config = load_live_call_config(options.config)
    except LiveCallConfigError:
        print("live-call configuration is unavailable or rejected", file=sys.stderr)
        return SKIPPED_EXIT_CODE
    if options.list:
        print(json.dumps(public_manifest(config), indent=2, sort_keys=True))
        return 0
    try:
        scenarios = selected_scenarios(config, options.plan)
    except LiveCallRunError as error:
        print(f"live-call: {error}", file=sys.stderr)
        return 2
    if not scenarios:
        print("live-call: no scenario plans are enabled", file=sys.stderr)
        return SKIPPED_EXIT_CODE
    if (
        any(scenario.readiness == "runnable" for scenario in scenarios)
        and not config.slmodemd.absolute().is_file()
    ):
        print("live-call: modem executable is unavailable", file=sys.stderr)
        return SKIPPED_EXIT_CODE
    results = []
    ran_call = False
    try:
        for scenario in scenarios:
            if scenario.readiness != "runnable":
                result = CallResult(
                    scenario_name=scenario.name,
                    plan_name=scenario.plan_name,
                    profile_name=scenario.profile_name,
                    family=scenario.family,
                    link_mode=scenario.link_mode,
                    attempt=scenario.attempt,
                    readiness=scenario.readiness,
                    prerequisite=scenario.prerequisite or None,
                    outcome="prerequisite",
                    duration_seconds=0.0,
                    terminal_expected_bytes=len(scenario.online_expect),
                )
            else:
                if ran_call and config.inter_scenario_seconds:
                    time.sleep(config.inter_scenario_seconds)
                try:
                    result = LiveCallAttempt(config, scenario).run()
                except (
                    LiveCallRunError,
                    LiveTerminalError,
                    OSError,
                    subprocess.SubprocessError,
                ):
                    result = CallResult(
                        scenario_name=scenario.name,
                        plan_name=scenario.plan_name,
                        profile_name=scenario.profile_name,
                        family=scenario.family,
                        link_mode=scenario.link_mode,
                        attempt=scenario.attempt,
                        readiness=scenario.readiness,
                        prerequisite=None,
                        outcome="artifact-failed",
                        duration_seconds=0.0,
                        terminal_expected_bytes=len(scenario.online_expect),
                    )
                ran_call = True
            results.append(result)
            print(json.dumps(result.public_dict(), sort_keys=True))
    except KeyboardInterrupt:
        print("live-call: interrupted", file=sys.stderr)
        return 130
    return 0 if all(result.outcome == "connected" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
