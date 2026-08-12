#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.interop_config import EndpointConfig  # noqa: E402
from tools.interop_endpoint import (  # noqa: E402
    EndpointProcess,
    InteropRunError,
    RunArtifacts,
    published_device,
    reserve_loopback_ports,
)


FAKE_ENDPOINT = r"""
import os
import pty
import signal
import sys
import time

signal.signal(signal.SIGTERM, signal.SIG_IGN)
master, slave = pty.openpty()
device = os.ttyname(slave)
print(f"PID={os.getpid()}", flush=True)
if sys.argv[1] == "bare":
    print(device, flush=True)
else:
    print(f"modem TTY is `{device}'", flush=True)
while True:
    time.sleep(0.05)
"""


class InteropEndpointTests(unittest.TestCase):
    def test_artifacts_are_private_and_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "evidence"
            artifacts = RunArtifacts.create(root)
            artifacts.write_exchange("originator", b"sent", b"received")
            artifacts.write_result({"schema": 1, "outcome": "passed"})

            self.assertEqual(
                (artifacts.endpoint_directories["originator"] / "terminal-tx.bin")
                .read_bytes(),
                b"sent",
            )
            self.assertEqual(
                json.loads(artifacts.result_file.read_text(encoding="utf-8")),
                {"schema": 1, "outcome": "passed"},
            )
            for directory in (root, artifacts.directory, *artifacts.endpoint_directories.values()):
                self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
            for path in artifacts.directory.rglob("*"):
                if path.is_file():
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError):
                artifacts.write_result({"schema": 1})

    def test_artifact_root_rejects_public_permissions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "public"
            root.mkdir(mode=0o755)
            with self.assertRaisesRegex(InteropRunError, "must be private"):
                RunArtifacts.create(root)

    def test_device_announcements_and_port_selection_are_bounded(self) -> None:
        self.assertEqual(published_device(b"/dev/pts/12\r"), "/dev/pts/12")
        self.assertEqual(
            published_device(b"modem TTY is `/dev/pts/34'\r"),
            "/dev/pts/34",
        )
        self.assertIsNone(published_device(b"/dev/pts/12 trailing"))
        reservations = []

        class FakeReservation:
            def __init__(self, *_arguments: object) -> None:
                self.closed = False
                self.port = 43001 + len(reservations)
                self.bound = None
                self.assert_earlier_choices_are_held()
                reservations.append(self)

            def assert_earlier_choices_are_held(self) -> None:
                if any(item.closed for item in reservations):
                    raise AssertionError("an earlier port was released too soon")

            def bind(self, address: tuple[str, int]) -> None:
                self.bound = address

            def getsockname(self) -> tuple[str, int]:
                return ("127.0.0.1", self.port)

            def close(self) -> None:
                self.closed = True

        with mock.patch(
            "tools.interop_endpoint.socket.socket", side_effect=FakeReservation
        ):
            ports = reserve_loopback_ports().values()
        self.assertEqual(len(ports), 4)
        self.assertEqual(len(set(ports)), 4)
        self.assertTrue(all(1024 <= port <= 65535 for port in ports))
        self.assertTrue(all(item.bound == ("127.0.0.1", 0) for item in reservations))
        self.assertTrue(all(item.closed for item in reservations))

    def test_process_owns_capture_terminal_and_forced_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            script = directory / "fake_endpoint.py"
            script.write_text(
                textwrap.dedent(FAKE_ENDPOINT).lstrip(), encoding="utf-8"
            )
            artifacts = RunArtifacts.create(directory / "evidence")
            for index, announcement in enumerate(("bare", "labeled")):
                config = EndpointConfig(
                    executable=Path(sys.executable),
                    arguments=(str(script), announcement),
                    environment=(),
                    at_commands=("ATZ",),
                )
                endpoint = EndpointProcess(
                    "originator" if index == 0 else "answerer",
                    config,
                    os.environ,
                    artifacts.daemon_log(
                        "originator" if index == 0 else "answerer"
                    ),
                )
                endpoint.start(2)
                self.assertIsNotNone(endpoint.terminal)
                assert endpoint.process is not None
                pid = endpoint.process.pid
                self.assertTrue(endpoint.stop(0.1))
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)


if __name__ == "__main__":
    unittest.main()
