#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Serial-port operations shared by unattended live-call peers."""

from __future__ import annotations

import math
import time
from typing import Protocol


class SerialPort(Protocol):
    """Small serial-port surface needed by the live peer."""

    rtscts: bool

    def flush(self) -> None:
        """Wait until queued output has reached the modem."""

    def write(self, data: memoryview) -> int | None:
        """Queue part of *data* for transmission to the modem."""


def write_all(port: SerialPort, data: bytes) -> None:
    """Write all of a bounded command or online marker to *port*."""

    view = memoryview(data)
    while view:
        written = port.write(view)
        if (
            not isinstance(written, int)
            or written <= 0
            or written > len(view)
        ):
            raise OSError("serial write made no progress")
        view = view[written:]


def escape_to_command_mode(
    port: SerialPort, guard_seconds: float = 1.1
) -> None:
    """Enter command mode without letting queued online data spoil the guard.

    Hardware transmit flow control remains enabled until earlier online data
    has reached the modem.  It is then disabled so a carrier transition cannot
    hold CTS low and prevent the escape sequence or following AT command.  The
    caller should restore flow control after the modem returns online.
    """

    if (
        not isinstance(guard_seconds, (int, float))
        or isinstance(guard_seconds, bool)
        or not math.isfinite(guard_seconds)
        or guard_seconds <= 0
    ):
        raise ValueError("escape guard must be a positive finite duration")
    if port.rtscts is not True:
        raise ValueError(
            "hardware transmit flow control must be enabled while online"
        )

    port.flush()
    time.sleep(guard_seconds)
    port.rtscts = False
    write_all(port, b"+++")
    port.flush()
    time.sleep(guard_seconds)
