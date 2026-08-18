#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run one private, opt-in modem-to-modem call over loopback SIP/RTP."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import json
from pathlib import Path
import subprocess
import sys
import time


if __package__:
    from .interop_config import (
        InteropConfig,
        InteropConfigError,
        build_child_environment,
        load_interop_config,
    )
    from .interop_endpoint import (
        EndpointProcess,
        InteropRunError,
        RunArtifacts,
        remaining,
        reserve_loopback_ports,
    )
    from .live_terminal import ConnectReport, LiveTerminalError
else:
    from interop_config import (
        InteropConfig,
        InteropConfigError,
        build_child_environment,
        load_interop_config,
    )
    from interop_endpoint import (
        EndpointProcess,
        InteropRunError,
        RunArtifacts,
        remaining,
        reserve_loopback_ports,
    )
    from live_terminal import ConnectReport, LiveTerminalError


SKIPPED_EXIT_CODE = 77
ONLINE_SETTLE_SECONDS = 0.1
POST_ATO1_SETTLE_SECONDS = 1.0


@dataclass
class EndpointState:
    connected: bool = False
    exchange_verified: bool = False
    transmitted: bytes = b""
    received: bytes = b""
    connect_report: ConnectReport = field(default_factory=ConnectReport)
    log_truncated: bool = False

    def public_dict(self, expected_bytes: int) -> dict[str, object]:
        report = self.connect_report
        return {
            "connected": self.connected,
            "exchange_verified": self.exchange_verified,
            "expected_bytes": expected_bytes,
            "verified_bytes": expected_bytes if self.exchange_verified else 0,
            "transmitted_bytes": len(self.transmitted),
            "received_bytes": len(self.received),
            "modulation": report.modulation,
            "protocol": report.protocol,
            "compression": report.compression,
            "receive_bit_rate": report.receive_bit_rate,
            "transmit_bit_rate": report.transmit_bit_rate,
            "daemon_log_truncated": self.log_truncated,
        }


@dataclass(frozen=True)
class InteropResult:
    outcome: str
    duration_seconds: float
    originator: EndpointState
    answerer: EndpointState
    originator_expected_bytes: int
    answerer_expected_bytes: int
    ato1_requested: bool = False
    ato1_connected: bool = False
    post_ato1_exchange_verified: bool = False

    def public_dict(self) -> dict[str, object]:
        public = {
            "schema": 1,
            "outcome": self.outcome,
            "duration_seconds": round(self.duration_seconds, 3),
            "originator": self.originator.public_dict(
                self.originator_expected_bytes
            ),
            "answerer": self.answerer.public_dict(self.answerer_expected_bytes),
        }
        if self.ato1_requested:
            public["ato1"] = {
                "connected": self.ato1_connected,
                "exchange_verified": self.post_ato1_exchange_verified,
            }
        return public


def _setup_endpoint(endpoint: EndpointProcess, deadline: float) -> None:
    assert endpoint.terminal is not None
    for command in endpoint.config.at_commands:
        if endpoint.terminal.command(command, remaining(deadline)) != "ok":
            raise InteropRunError("endpoint rejected AT setup")


def _connect_endpoints(
    originator: EndpointProcess,
    answerer: EndpointProcess,
    deadline: float,
) -> tuple[str, str]:
    assert originator.terminal is not None
    assert answerer.terminal is not None
    with ThreadPoolExecutor(max_workers=2) as executor:
        origin_future = executor.submit(originator.terminal.dial_until, deadline)
        answer_future = executor.submit(answerer.terminal.answer_until, deadline)
        results: list[str] = []
        for future in (origin_future, answer_future):
            try:
                results.append(future.result())
            except (InteropRunError, LiveTerminalError, OSError, TimeoutError):
                results.append("error")
    return results[0], results[1]


def _exchange_endpoint(
    endpoint: EndpointProcess,
    send: bytes,
    expected: bytes,
    deadline: float,
) -> tuple[bool, bytes]:
    assert endpoint.terminal is not None
    return endpoint.terminal.exchange_online(send, expected, remaining(deadline))


def _exchange_endpoints(
    originator: EndpointProcess,
    answerer: EndpointProcess,
    originator_payload: bytes,
    answerer_payload: bytes,
    deadline: float,
    turn_taking: bool,
) -> tuple[tuple[bool, bytes], tuple[bool, bytes]]:
    if turn_taking:
        assert originator.terminal is not None
        assert answerer.terminal is not None
        try:
            originator.terminal.send_online(
                originator_payload, remaining(deadline)
            )
            answer_exchange = answerer.terminal.exchange_online(
                b"", originator_payload, remaining(deadline)
            )
            if not answer_exchange[0]:
                return (False, b""), answer_exchange
            answerer.terminal.send_online(answerer_payload, remaining(deadline))
            origin_exchange = originator.terminal.exchange_online(
                b"", answerer_payload, remaining(deadline)
            )
            return origin_exchange, answer_exchange
        except (InteropRunError, LiveTerminalError, OSError, TimeoutError):
            return (False, b""), (False, b"")

    with ThreadPoolExecutor(max_workers=2) as executor:
        origin_future = executor.submit(
            _exchange_endpoint,
            originator,
            originator_payload,
            answerer_payload,
            deadline,
        )
        answer_future = executor.submit(
            _exchange_endpoint,
            answerer,
            answerer_payload,
            originator_payload,
            deadline,
        )
        results: list[tuple[bool, bytes]] = []
        for future in (origin_future, answer_future):
            try:
                results.append(future.result())
            except (InteropRunError, LiveTerminalError, OSError, TimeoutError):
                results.append((False, b""))
    return results[0], results[1]


def _stop_endpoints(
    endpoints: tuple[EndpointProcess, EndpointProcess],
    timeout_seconds: float,
) -> tuple[bool, bool]:
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(endpoint.stop, timeout_seconds)
            for endpoint in endpoints
        ]
        results: list[bool] = []
        for future in futures:
            try:
                results.append(future.result())
            except (OSError, subprocess.SubprocessError):
                results.append(False)
    return results[0], results[1]


def run_interop(
    config: InteropConfig,
    *,
    turn_taking: bool = False,
    ato1_role: str | None = None,
    post_ato1_payloads: tuple[bytes, bytes] | None = None,
    escape_guard_seconds: float = 1.1,
) -> InteropResult:
    """Run the configured pair and retain private evidence for every outcome."""

    if not config.enabled:
        raise InteropRunError("interop configuration is disabled")
    if (ato1_role is None) != (post_ato1_payloads is None):
        raise InteropRunError("ATO1 role and payloads must be supplied together")
    if ato1_role not in {None, "originator", "answerer"}:
        raise InteropRunError("invalid ATO1 endpoint role")
    if post_ato1_payloads is not None and (
        not all(post_ato1_payloads)
        or post_ato1_payloads[0] in post_ato1_payloads[1]
        or post_ato1_payloads[1] in post_ato1_payloads[0]
    ):
        raise InteropRunError("ATO1 payloads must be distinct and non-containing")
    ports = config.ports
    if not any(ports.values()):
        ports = reserve_loopback_ports()
    artifacts = RunArtifacts.create(config.artifact_directory)
    originator_state = EndpointState()
    answerer_state = EndpointState()
    originator = EndpointProcess(
        "originator",
        config.originator,
        build_child_environment(
            config.originator,
            "originator",
            ports,
            artifacts.recording_prefix("originator"),
        ),
        artifacts.daemon_log("originator"),
    )
    answerer = EndpointProcess(
        "answerer",
        config.answerer,
        build_child_environment(
            config.answerer,
            "answerer",
            ports,
            artifacts.recording_prefix("answerer"),
        ),
        artifacts.daemon_log("answerer"),
    )
    started = time.monotonic()
    stage = "startup"
    outcome = "infrastructure-error"
    ato1_connected = False
    post_ato1_exchange_verified = False
    try:
        answerer.start(config.startup_timeout_seconds)
        originator.start(config.startup_timeout_seconds)

        stage = "setup"
        setup_deadline = time.monotonic() + config.setup_timeout_seconds
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = (
                executor.submit(_setup_endpoint, answerer, setup_deadline),
                executor.submit(_setup_endpoint, originator, setup_deadline),
            )
            for future in futures:
                future.result()

        stage = "connect"
        connect_deadline = time.monotonic() + config.connect_timeout_seconds
        originator_result, answerer_result = _connect_endpoints(
            originator, answerer, connect_deadline
        )
        originator_state.connected = originator_result == "connect"
        answerer_state.connected = answerer_result == "connect"
        assert originator.terminal is not None
        assert answerer.terminal is not None
        if originator_state.connected:
            originator_state.connect_report = originator.terminal.connect_report
        if answerer_state.connected:
            answerer_state.connect_report = answerer.terminal.connect_report
        if not originator_state.connected or not answerer_state.connected:
            outcome = "connect-error"
        else:
            stage = "data"
            # Some peers report CONNECT before their media datapump can safely
            # accept the first terminal character.  RTP continues to clock
            # during this short, bounded settling interval.
            time.sleep(ONLINE_SETTLE_SECONDS)
            originator_state.transmitted = config.originator_payload
            answerer_state.transmitted = config.answerer_payload
            data_deadline = time.monotonic() + config.online_timeout_seconds
            origin_exchange, answer_exchange = _exchange_endpoints(
                originator,
                answerer,
                config.originator_payload,
                config.answerer_payload,
                data_deadline,
                turn_taking,
            )
            originator_state.exchange_verified, originator_state.received = (
                origin_exchange
            )
            answerer_state.exchange_verified, answerer_state.received = (
                answer_exchange
            )
            outcome = (
                "passed"
                if originator_state.exchange_verified
                and answerer_state.exchange_verified
                else "data-error"
            )
            if outcome == "passed" and ato1_role is not None:
                stage = "ato1"
                ato1_endpoint = (
                    originator if ato1_role == "originator" else answerer
                )
                assert ato1_endpoint.terminal is not None
                ato1_connected = ato1_endpoint.terminal.return_online(
                    config.connect_timeout_seconds,
                    escape_guard_seconds,
                ).connected
                if not ato1_connected:
                    outcome = "ato1-error"
                else:
                    assert post_ato1_payloads is not None
                    stage = "post-ato1-data"
                    # Retrain CONNECT can precede decoder reacquisition and
                    # error-control retransmission at the peer. Keep this
                    # boundary distinct from initial-link settling.
                    time.sleep(POST_ATO1_SETTLE_SECONDS)
                    data_deadline = (
                        time.monotonic() + config.online_timeout_seconds
                    )
                    origin_exchange, answer_exchange = _exchange_endpoints(
                        originator,
                        answerer,
                        post_ato1_payloads[0],
                        post_ato1_payloads[1],
                        data_deadline,
                        turn_taking,
                    )
                    originator_state.transmitted += post_ato1_payloads[0]
                    answerer_state.transmitted += post_ato1_payloads[1]
                    originator_state.received += origin_exchange[1]
                    answerer_state.received += answer_exchange[1]
                    post_ato1_exchange_verified = (
                        origin_exchange[0] and answer_exchange[0]
                    )
                    originator_state.exchange_verified &= origin_exchange[0]
                    answerer_state.exchange_verified &= answer_exchange[0]
                    outcome = (
                        "passed"
                        if post_ato1_exchange_verified
                        else "post-ato1-data-error"
                    )
    except (InteropRunError, LiveTerminalError, OSError, TimeoutError):
        outcome = f"{stage}-error"
    finally:
        cleanup_results = _stop_endpoints(
            (originator, answerer), config.cleanup_timeout_seconds
        )
        if not all(cleanup_results):
            outcome = "cleanup-error"
        originator_state.log_truncated = originator.log_truncated
        answerer_state.log_truncated = answerer.log_truncated

    originator_expected_bytes = len(config.answerer_payload)
    answerer_expected_bytes = len(config.originator_payload)
    if post_ato1_payloads is not None:
        originator_expected_bytes += len(post_ato1_payloads[1])
        answerer_expected_bytes += len(post_ato1_payloads[0])
    result = InteropResult(
        outcome=outcome,
        duration_seconds=time.monotonic() - started,
        originator=originator_state,
        answerer=answerer_state,
        originator_expected_bytes=originator_expected_bytes,
        answerer_expected_bytes=answerer_expected_bytes,
        ato1_requested=ato1_role is not None,
        ato1_connected=ato1_connected,
        post_ato1_exchange_verified=post_ato1_exchange_verified,
    )
    artifacts.write_exchange(
        "originator", originator_state.transmitted, originator_state.received
    )
    artifacts.write_exchange(
        "answerer", answerer_state.transmitted, answerer_state.received
    )
    artifacts.write_result(result.public_dict())
    return result


def _public_status(outcome: str) -> str:
    return json.dumps({"schema": 1, "outcome": outcome}, sort_keys=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run one opt-in modem-pair call over loopback.",
        epilog=(
            "Copy tools/local-interop.example.json into an ignored private "
            "directory, edit both endpoint definitions, chmod it 600, and "
            "enable it before running."
        ),
    )
    parser.add_argument("--config", type=Path, required=True)
    arguments = parser.parse_args(argv)
    if not arguments.config.exists():
        print(_public_status("skipped"))
        return SKIPPED_EXIT_CODE
    try:
        config = load_interop_config(arguments.config)
    except InteropConfigError:
        print(_public_status("configuration-error"))
        return 2
    if not config.enabled:
        print(_public_status("skipped"))
        return SKIPPED_EXIT_CODE
    try:
        result = run_interop(config)
    except (
        InteropRunError,
        LiveTerminalError,
        OSError,
        subprocess.SubprocessError,
        TimeoutError,
    ):
        print(_public_status("infrastructure-error"))
        return 1
    print(json.dumps(result.public_dict(), sort_keys=True))
    return 0 if result.outcome == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
