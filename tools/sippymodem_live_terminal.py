#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Bounded command/response access to one live modem pseudoterminal."""

from __future__ import annotations

from collections import deque
import os
import re
import selectors
import termios
import time
import tty
from typing import IO


DEVICE_PATH_RE = re.compile(r"/dev/pts/[0-9]+\Z")
RESPONSE_LINE_MAX = 512
PUBLISHED_LINE_MAX = 512
PUBLISHED_OUTPUT_MAX = 4096
RESULT_BY_LINE = {
    b"0": "ok",
    b"1": "connect",
    b"3": "no-carrier",
    b"4": "error",
    b"6": "no-dialtone",
    b"7": "busy",
    b"8": "no-answer",
    b"OK": "ok",
    b"NO CARRIER": "no-carrier",
    b"ERROR": "error",
    b"NO DIALTONE": "no-dialtone",
    b"BUSY": "busy",
    b"NO ANSWER": "no-answer",
}


class LiveTerminalError(RuntimeError):
    """A modem terminal published unsafe or unusable state."""


class ResponseDecoder:
    """Own bounded line assembly for a single modem terminal."""

    def __init__(self) -> None:
        self._line = bytearray()
        self._results: deque[str] = deque()
        self._discarding_line = False

    def feed(self, data: bytes) -> None:
        for byte in data:
            if byte in (0x0A, 0x0D):
                if not self._discarding_line and self._line:
                    result = _classify_response(bytes(self._line))
                    if result is not None:
                        self._results.append(result)
                self._line.clear()
                self._discarding_line = False
            elif not self._discarding_line:
                if len(self._line) < RESPONSE_LINE_MAX:
                    self._line.append(byte)
                else:
                    self._line.clear()
                    self._discarding_line = True

    def pop(self) -> str | None:
        return self._results.popleft() if self._results else None


def _classify_response(line: bytes) -> str | None:
    normalized = line.strip().upper()
    if normalized == b"CONNECT" or normalized.startswith(b"CONNECT "):
        return "connect"
    return RESULT_BY_LINE.get(normalized)


class ModemTerminal:
    """Own one nonblocking pseudoterminal client and its response state."""

    def __init__(self, descriptor: int) -> None:
        self._descriptor = descriptor
        self._decoder = ResponseDecoder()

    @classmethod
    def open(cls, device_path: str) -> ModemTerminal:
        flags = os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
        flags |= getattr(os, "O_CLOEXEC", 0)
        descriptor = os.open(device_path, flags)
        try:
            tty.setraw(descriptor, when=termios.TCSANOW)
        except BaseException:
            os.close(descriptor)
            raise
        return cls(descriptor)

    def close(self) -> None:
        if self._descriptor >= 0:
            descriptor = self._descriptor
            self._descriptor = -1
            os.close(descriptor)

    def __enter__(self) -> ModemTerminal:
        return self

    def __exit__(self, *_unused: object) -> None:
        self.close()

    def command(self, command: str, timeout_seconds: float) -> str:
        try:
            encoded = command.encode("ascii") + b"\r"
        except UnicodeEncodeError as error:
            raise LiveTerminalError("terminal command is not ASCII") from error
        deadline = time.monotonic() + timeout_seconds
        self._write(encoded, deadline)
        return self._read_result(deadline)

    def escape_and_hang_up(self) -> None:
        time.sleep(1.1)
        deadline = time.monotonic() + 3.0
        self._write(b"+++", deadline)
        if self._read_result(deadline) == "ok":
            try:
                self.command("ATH", 3.0)
            except (LiveTerminalError, OSError, TimeoutError):
                pass

    def _write(self, data: bytes, deadline: float) -> None:
        view = memoryview(data)
        while view:
            try:
                count = os.write(self._descriptor, view)
                if count <= 0:
                    raise OSError("terminal write made no progress")
                view = view[count:]
            except BlockingIOError:
                _wait_for_descriptor(
                    self._descriptor, selectors.EVENT_WRITE, deadline
                )
            except InterruptedError:
                continue

    def _read_result(self, deadline: float) -> str:
        while True:
            result = self._decoder.pop()
            if result is not None:
                return result
            _wait_for_descriptor(self._descriptor, selectors.EVENT_READ, deadline)
            try:
                data = os.read(self._descriptor, 4096)
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                raise OSError("modem terminal closed")
            self._decoder.feed(data)


def _wait_for_descriptor(descriptor: int, events: int, deadline: float) -> None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError
    with selectors.DefaultSelector() as selector:
        selector.register(descriptor, events)
        if not selector.select(remaining):
            raise TimeoutError


def read_published_device(stream: IO[bytes], timeout_seconds: float) -> str:
    descriptor = stream.fileno()
    deadline = time.monotonic() + timeout_seconds
    buffered = bytearray()
    received = 0
    while True:
        while b"\n" in buffered:
            line, _, remainder = buffered.partition(b"\n")
            buffered = bytearray(remainder)
            line = line.rstrip(b"\r")
            if len(line) > PUBLISHED_LINE_MAX:
                raise LiveTerminalError("published startup line is too long")
            try:
                candidate = line.decode("ascii")
            except UnicodeDecodeError:
                continue
            if DEVICE_PATH_RE.fullmatch(candidate) is not None:
                return candidate
        if received >= PUBLISHED_OUTPUT_MAX:
            raise LiveTerminalError("published startup output is too long")
        _wait_for_descriptor(descriptor, selectors.EVENT_READ, deadline)
        try:
            chunk = os.read(descriptor, min(512, PUBLISHED_OUTPUT_MAX - received))
        except InterruptedError:
            continue
        if not chunk:
            raise LiveTerminalError("modem did not publish a terminal")
        buffered.extend(chunk)
        received += len(chunk)
        if len(buffered) > PUBLISHED_LINE_MAX and b"\n" not in buffered:
            raise LiveTerminalError("published startup line is too long")
