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


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.local_interop import (  # noqa: E402
    load_interop_config,
    run_interop,
)


FAKE_MODEM = r"""
import os
import pty
import select
import signal
import sys
import time
import tty
from pathlib import Path


role = sys.argv[1]
incoming = bytes.fromhex(sys.argv[2])
setup_count = int(sys.argv[3])
ring_delay = float(sys.argv[4])
announcement = sys.argv[5]
barrier = Path(sys.argv[6])
origin_connect_delay = float(sys.argv[7])
peer_role = "answerer" if role == "originator" else "originator"
signal.signal(signal.SIGTERM, signal.SIG_IGN)

master, slave = pty.openpty()
tty.setraw(slave)
device = os.ttyname(slave)
print(f"PID={os.getpid()}", flush=True)
if announcement == "bare":
    print(device, flush=True)
else:
    print(f"modem TTY is `{device}'", flush=True)

buffer = bytearray()
commands = 0
setup_complete = False
ring_at = None
ring_sent = False
dial_pending = False
origin_connect_at = None
online = False
replied = False
pending_reply = False
connect_response = (
    b"\r\nModulation: V21\r\nProtocol: NONE\r\n"
    b"Compression: NONE\r\nTxRate: 300\r\nCONNECT 300\r\n"
)

while True:
    readable, _, _ = select.select([master], [], [], 0.01)
    now = time.monotonic()
    if (
        role == "answerer"
        and setup_complete
        and not ring_sent
        and (barrier / "call").exists()
    ):
        if ring_at is None:
            ring_at = now + ring_delay
        if now >= ring_at:
            os.write(master, b"\r\nRING\r\n")
            ring_sent = True
    if role == "originator" and dial_pending and (barrier / "answered").exists():
        if origin_connect_at is None:
            origin_connect_at = now + origin_connect_delay
        if now >= origin_connect_at:
            os.write(master, connect_response)
            dial_pending = False
            origin_connect_at = None
            online = True
    if pending_reply and (barrier / peer_role).exists():
        os.write(master, incoming)
        replied = True
        pending_reply = False
    if not readable:
        continue
    data = os.read(master, 4096)
    if not data:
        break
    if online:
        if not replied:
            (barrier / role).touch(exist_ok=True)
            pending_reply = True
        continue
    buffer.extend(data)
    while b"\r" in buffer:
        command, _, remainder = buffer.partition(b"\r")
        buffer = bytearray(remainder)
        if command.upper() == b"ATD" and role == "originator":
            (barrier / "call").touch(exist_ok=True)
            dial_pending = True
        elif command.upper() == b"ATA" and role == "answerer":
            if not ring_sent:
                os.write(master, b"\r\nERROR\r\n")
            else:
                os.write(master, connect_response)
                (barrier / "ata").touch(exist_ok=True)
                (barrier / "answered").touch(exist_ok=True)
                online = True
        else:
            commands += 1
            os.write(master, b"\r\nOK\r\n")
            if role == "answerer" and commands == setup_count:
                setup_complete = True
"""


ORIGINATOR_RAW_COMMANDS = [
    "ATZ",
    "AT\\N0",
    "AT+DS=0,1",
    "AT+MS=21,0,300,300",
]
ANSWERER_RAW_COMMANDS = [
    "ATZ",
    "AT\\N0",
    "AT+DS=0,0",
    "AT+MS=21,0,300,300",
]


class LocalInteropTests(unittest.TestCase):
    def write_config(
        self,
        directory: Path,
        executable: Path,
        fake_script: Path,
        *,
        enabled: bool = True,
        answer_delay: float = 0.2,
        origin_connect_delay: float = 0.2,
        connect_timeout: float = 2,
    ) -> Path:
        barrier = directory / "barrier"
        barrier.mkdir(exist_ok=True)
        payload = {
            "enabled": enabled,
            "artifact_directory": str(directory / "artifacts"),
            "originator": {
                "executable": str(executable),
                "arguments": [
                    str(fake_script),
                    "originator",
                    b"ANSWER-DATA".hex(),
                    str(len(ORIGINATOR_RAW_COMMANDS)),
                    "0",
                    "bare",
                    str(barrier),
                    str(origin_connect_delay),
                ],
                "environment": {"SIP_CODEC": "PCMU"},
                "at_commands": ORIGINATOR_RAW_COMMANDS,
            },
            "answerer": {
                "executable": str(executable),
                "arguments": [
                    str(fake_script),
                    "answerer",
                    b"ORIGIN-DATA".hex(),
                    str(len(ANSWERER_RAW_COMMANDS)),
                    str(answer_delay),
                    "labeled",
                    str(barrier),
                    str(origin_connect_delay),
                ],
                "environment": {"SIP_LOG_LEVEL": "0"},
                "at_commands": ANSWERER_RAW_COMMANDS,
            },
            "originator_payload_hex": b"ORIGIN-DATA".hex(),
            "answerer_payload_hex": b"ANSWER-DATA".hex(),
            "startup_timeout_seconds": 2,
            "setup_timeout_seconds": 2,
            "connect_timeout_seconds": connect_timeout,
            "online_timeout_seconds": 2,
            "cleanup_timeout_seconds": 0.1,
            "ports": {
                "originator_sip": 41001,
                "answerer_sip": 41002,
                "originator_rtp": 41003,
                "answerer_rtp": 41004,
            },
        }
        config_path = directory / "interop.json"
        config_path.write_text(json.dumps(payload), encoding="utf-8")
        config_path.chmod(0o600)
        return config_path

    def test_fake_processes_ring_answer_and_exchange_over_real_ptys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fake_script = directory / "fake.py"
            fake_script.write_text(
                textwrap.dedent(FAKE_MODEM).lstrip(), encoding="utf-8"
            )
            config = load_interop_config(
                self.write_config(directory, Path(sys.executable), fake_script)
            )
            result = run_interop(config)
            public = result.public_dict()

            self.assertEqual(public["outcome"], "passed")
            self.assertTrue(public["originator"]["connected"])
            self.assertTrue(public["answerer"]["connected"])
            self.assertTrue(public["originator"]["exchange_verified"])
            self.assertTrue(public["answerer"]["exchange_verified"])
            self.assertEqual(public["originator"]["modulation"], "V.21")
            self.assertEqual(public["answerer"]["receive_bit_rate"], 300)
            self.assertTrue((directory / "barrier" / "ata").exists())

            serialized = json.dumps(public, sort_keys=True)
            self.assertNotIn(str(directory), serialized)
            self.assertNotIn(str(sys.executable), serialized)
            self.assertNotIn(b"ORIGIN-DATA".hex(), serialized)
            self.assertNotIn(b"ANSWER-DATA".hex(), serialized)

            run_directories = list((directory / "artifacts").iterdir())
            self.assertEqual(len(run_directories), 1)
            run_directory = run_directories[0]
            self.assertEqual(
                stat.S_IMODE(run_directory.stat().st_mode), 0o700
            )
            self.assertEqual(
                (run_directory / "originator" / "terminal-tx.bin").read_bytes(),
                b"ORIGIN-DATA",
            )
            self.assertIn(
                b"ANSWER-DATA",
                (run_directory / "originator" / "terminal-rx.bin").read_bytes(),
            )
            self.assertEqual(
                (run_directory / "answerer" / "terminal-tx.bin").read_bytes(),
                b"ANSWER-DATA",
            )
            self.assertIn(
                b"ORIGIN-DATA",
                (run_directory / "answerer" / "terminal-rx.bin").read_bytes(),
            )
            private_result = json.loads(
                (run_directory / "result.json").read_text(encoding="utf-8")
            )
            self.assertEqual(private_result, public)
            for artifact in run_directory.rglob("*"):
                if artifact.is_file():
                    self.assertEqual(stat.S_IMODE(artifact.stat().st_mode), 0o600)
            for role in ("originator", "answerer"):
                log = (run_directory / role / "daemon.log").read_text(
                    encoding="utf-8"
                )
                pid_line = next(
                    line for line in log.splitlines() if line.startswith("PID=")
                )
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(pid_line.removeprefix("PID=")), 0)

    def test_global_connect_deadline_preserves_the_first_endpoint_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fake_script = directory / "fake.py"
            fake_script.write_text(FAKE_MODEM, encoding="utf-8")
            config = load_interop_config(
                self.write_config(
                    directory,
                    Path(sys.executable),
                    fake_script,
                    answer_delay=0,
                    origin_connect_delay=1.5,
                    connect_timeout=1,
                )
            )
            result = run_interop(config).public_dict()

            self.assertEqual(result["outcome"], "connect-error")
            self.assertFalse(result["originator"]["connected"])
            self.assertTrue(result["answerer"]["connected"])
            self.assertLess(result["duration_seconds"], 1.5)


if __name__ == "__main__":
    unittest.main()
