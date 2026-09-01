#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from pathlib import Path
import sys
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_peer_serial import (  # noqa: E402
    escape_to_command_mode,
    write_all,
)


class BufferedPort:
    """Model a flow-controlled serial output queue and a slow modem."""

    def __init__(
        self,
        maximum_write: int | None = None,
        *,
        hardware_flow: bool = True,
    ) -> None:
        self.events: list[object] = []
        self.maximum_write = maximum_write
        self.pending = bytearray()
        self.received = bytearray()
        self._rtscts = hardware_flow

    @property
    def rtscts(self) -> bool:
        return self._rtscts

    @rtscts.setter
    def rtscts(self, enabled: bool) -> None:
        self.events.append(("flow", enabled, bytes(self.pending)))
        self._rtscts = enabled
        if not enabled:
            self._deliver()

    def flush(self) -> None:
        self.events.append(("flush", self.rtscts, bytes(self.pending)))
        self._deliver()

    def write(self, data: memoryview) -> int:
        count = len(data)
        if self.maximum_write is not None:
            count = min(count, self.maximum_write)
        self.pending.extend(data[:count])
        return count

    def _deliver(self) -> None:
        self.received.extend(self.pending)
        self.pending.clear()


class LivePeerSerialTests(unittest.TestCase):
    def test_drains_slow_online_marker_before_escape_guard(self) -> None:
        port = BufferedPort()
        marker = (
            b"GETTY:RETRAIN:START:"
            b"0123456789abcdef0123456789abcdef\r\n"
        )
        self.assertEqual(len(marker), 54)
        write_all(port, marker)

        guards: list[tuple[float, bytes, bytes]] = []

        def record_guard(seconds: float) -> None:
            guards.append((seconds, bytes(port.pending), bytes(port.received)))

        with mock.patch(
            "tools.live_peer_serial.time.sleep", side_effect=record_guard
        ):
            escape_to_command_mode(port, 1.1)

        self.assertEqual(
            port.events,
            [
                ("flush", True, marker),
                ("flow", False, b""),
                ("flush", False, b"+++"),
            ],
        )
        self.assertEqual(
            guards,
            [
                (1.1, b"", marker),
                (1.1, b"", marker + b"+++"),
            ],
        )
        self.assertEqual(port.received, marker + b"+++")
        self.assertFalse(port.rtscts)

    def test_write_all_handles_partial_serial_writes(self) -> None:
        port = BufferedPort(maximum_write=2)

        write_all(port, b"abcdefg")
        port.flush()

        self.assertEqual(port.received, b"abcdefg")

    def test_rejects_invalid_escape_guard(self) -> None:
        port = BufferedPort()
        for guard in (0.0, -1.0, float("inf"), float("nan"), True):
            with self.subTest(guard=guard):
                with self.assertRaises(ValueError):
                    escape_to_command_mode(port, guard)
        self.assertEqual(port.events, [])

    def test_requires_hardware_flow_before_draining_online_data(self) -> None:
        port = BufferedPort(hardware_flow=False)
        write_all(port, b"online data")

        with self.assertRaises(ValueError):
            escape_to_command_mode(port)

        self.assertEqual(port.events, [])
        self.assertEqual(port.pending, b"online data")
        self.assertEqual(port.received, b"")


if __name__ == "__main__":
    unittest.main()
