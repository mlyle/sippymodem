#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Bounded command/response access to one live modem pseudoterminal."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import os
import re
import selectors
import termios
import time
import tty
from typing import Callable, IO


DEVICE_PATH_RE = re.compile(r"/dev/pts/[0-9]+\Z")
RESPONSE_LINE_MAX = 512
ONLINE_PATTERN_MAX = 512
ONLINE_CAPTURE_MAX = 8192
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
CONNECT_MODULATIONS = {
    b"V21": "V.21",
    b"V22": "V.22",
    b"V22BIS": "V.22bis",
    b"V23": "V.23",
    b"V32": "V.32",
    b"V32BIS": "V.32bis",
    b"V34": "V.34",
    b"V90": "V.90",
    b"V92": "V.92",
    b"BELL103": "Bell 103",
    b"BELL212A": "Bell 212A",
    b"BAUDOT TTY/TDD": "Baudot TTY/TDD",
}
CONNECT_PROTOCOLS = {b"LAPM": "LAPM", b"NONE": "none"}
CONNECT_COMPRESSIONS = {
    b"V44": "V.44",
    b"V42BIS": "V.42bis",
    b"NONE": "none",
}
CONNECT_RATE_MAX = 1_000_000
ONLINE_INITIAL_RESPONSE = (
    b"DMODEM-PATTERN-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"
)
REMOTE_RETRAIN_REQUEST = b"\r\n\r\nRETRAIN\r\n"
REMOTE_RETRAIN_START_RE = re.compile(
    rb"(?:^|[\r\n])GETTY:RETRAIN:START:"
    rb"(?P<nonce>[0-9a-fA-F]{32})(?::[^\r\n]*)?(?:\r\n|\r|\n)"
)
ONLINE_PING_REQUEST = b"\r\n\r\nPING\r\n"
ONLINE_PONG_RE = re.compile(
    rb"(?:^|[\r\n])GETTY:PONG:[0-9a-fA-F]{1,64}:"
    rb"[0-9]{1,20}(?:\r\n|\r|\n)"
)


class LiveTerminalError(RuntimeError):
    """A modem terminal published unsafe or unusable state."""


@dataclass(frozen=True)
class ConnectReport:
    """Allowlisted modem details associated with one CONNECT response."""

    modulation: str | None = None
    protocol: str | None = None
    compression: str | None = None
    receive_bit_rate: int | None = None
    transmit_bit_rate: int | None = None


@dataclass(frozen=True)
class ReturnOnlineReport:
    """Results from the escape guard and the following ATO1 command."""

    escape_result: str
    command_result: str | None = None
    retrain_result: str | None = None

    @property
    def connected(self) -> bool:
        return (
            self.escape_result == "ok"
            and (
                self.command_result == "connect"
                or (
                    self.command_result == "ok"
                    and self.retrain_result == "connect"
                )
            )
        )


@dataclass(frozen=True)
class RemoteRetrainReport:
    """Bounded evidence returned by the peer retrain protocol."""

    complete: bool
    continuation: bytes


class ResponseDecoder:
    """Own bounded line assembly for a single modem terminal."""

    def __init__(self) -> None:
        self._line = bytearray()
        self._online_data = bytearray()
        self._results: deque[str] = deque()
        self._discarding_line = False
        self._online = False
        self._skip_line_feed = False
        self._modulation: str | None = None
        self._protocol: str | None = None
        self._compression: str | None = None
        self._receive_bit_rate: int | None = None
        self._transmit_bit_rate: int | None = None

    def feed(self, data: bytes) -> None:
        for byte in data:
            if self._online:
                if self._skip_line_feed:
                    self._skip_line_feed = False
                    if byte == 0x0A:
                        continue
                if len(self._online_data) >= ONLINE_CAPTURE_MAX:
                    raise LiveTerminalError("online terminal data is too long")
                self._online_data.append(byte)
                continue
            if byte in (0x0A, 0x0D):
                if not self._discarding_line and self._line:
                    line = bytes(self._line)
                    detail = _classify_connect_detail(line)
                    if detail is not None:
                        name, value = detail
                        if name == "modulation":
                            self._modulation = value
                        elif name == "protocol":
                            self._protocol = value
                        elif name == "compression":
                            self._compression = value
                        elif name == "transmit_bit_rate":
                            self._transmit_bit_rate = value
                    result = _classify_response(line)
                    if result is not None:
                        self._results.append(result)
                        if result == "connect":
                            self._receive_bit_rate = _connect_rate(line)
                            self._online = True
                            self._skip_line_feed = byte == 0x0D
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

    @property
    def online(self) -> bool:
        return self._online

    def take_online_data(self) -> bytes:
        data = bytes(self._online_data)
        self._online_data.clear()
        return data

    @property
    def connect_report(self) -> ConnectReport:
        return ConnectReport(
            modulation=self._modulation,
            protocol=self._protocol,
            compression=self._compression,
            receive_bit_rate=self._receive_bit_rate,
            transmit_bit_rate=self._transmit_bit_rate,
        )

    def begin_command_response(self) -> None:
        self._line.clear()
        self._online_data.clear()
        self._results.clear()
        self._discarding_line = False
        self._online = False
        self._skip_line_feed = False
        self._modulation = None
        self._protocol = None
        self._compression = None
        self._receive_bit_rate = None
        self._transmit_bit_rate = None


def _classify_response(line: bytes) -> str | None:
    normalized = line.strip().upper()
    if normalized == b"CONNECT" or normalized.startswith(b"CONNECT "):
        return "connect"
    return RESULT_BY_LINE.get(normalized)


def _positive_rate(value: bytes) -> int | None:
    if not value or len(value) > 7 or not value.isdigit():
        return None
    rate = int(value)
    return rate if 0 < rate <= CONNECT_RATE_MAX else None


def _connect_rate(line: bytes) -> int | None:
    normalized = line.strip().upper()
    prefix = b"CONNECT "
    if not normalized.startswith(prefix):
        return None
    return _positive_rate(normalized[len(prefix) :])


def _classify_connect_detail(line: bytes) -> tuple[str, str | int] | None:
    normalized = line.strip().upper()
    text_fields = (
        (b"MODULATION:", "modulation", CONNECT_MODULATIONS),
        (b"PROTOCOL:", "protocol", CONNECT_PROTOCOLS),
        (b"COMPRESSION:", "compression", CONNECT_COMPRESSIONS),
    )
    for prefix, name, values in text_fields:
        if normalized.startswith(prefix):
            value = values.get(normalized[len(prefix) :].strip())
            return (name, value) if value is not None else None
    prefix = b"TXRATE:"
    if normalized.startswith(prefix):
        rate = _positive_rate(normalized[len(prefix) :].strip())
        return ("transmit_bit_rate", rate) if rate is not None else None
    return None


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

    @property
    def connect_report(self) -> ConnectReport:
        return self._decoder.connect_report

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
        try:
            self._decoder.begin_command_response()
            self._write(b"+++", deadline)
            if self._read_result(deadline) == "ok":
                self.command("ATH", 3.0)
        except (LiveTerminalError, OSError, TimeoutError):
            pass

    def exchange_online(
        self, send: bytes, expected: bytes, timeout_seconds: float
    ) -> tuple[bool, bytes]:
        if (
            not isinstance(expected, bytes)
            or not expected
            or len(expected) > ONLINE_PATTERN_MAX
        ):
            raise LiveTerminalError("invalid online terminal exchange")
        return self._exchange_online(
            send, lambda received: expected in received, timeout_seconds
        )

    def exchange_online_pattern(
        self,
        send: bytes,
        pattern: re.Pattern[bytes],
        timeout_seconds: float,
    ) -> tuple[bool, bytes]:
        expression = getattr(pattern, "pattern", None)
        if (
            not isinstance(expression, bytes)
            or not expression
            or len(expression) > ONLINE_PATTERN_MAX
        ):
            raise LiveTerminalError("invalid online terminal pattern")
        return self._exchange_online(
            send,
            lambda received: pattern.search(received) is not None,
            timeout_seconds,
        )

    def send_online(self, data: bytes, timeout_seconds: float) -> None:
        if (
            not isinstance(data, bytes)
            or not data
            or len(data) > ONLINE_PATTERN_MAX
            or timeout_seconds <= 0
            or not self._decoder.online
        ):
            raise LiveTerminalError("invalid online terminal transmission")
        self._write(data, time.monotonic() + timeout_seconds)

    def return_online(
        self, timeout_seconds: float, guard_seconds: float = 1.1
    ) -> ReturnOnlineReport:
        if (
            timeout_seconds <= 0
            or guard_seconds < 0
            or guard_seconds >= timeout_seconds
            or not self._decoder.online
        ):
            raise LiveTerminalError("invalid return-online request")
        deadline = time.monotonic() + timeout_seconds
        time.sleep(guard_seconds)
        self._decoder.begin_command_response()
        self._write(b"+++", deadline)
        escape_result = self._read_result(deadline)
        if escape_result != "ok":
            return ReturnOnlineReport(escape_result)
        self._write(b"ATO1\r", deadline)
        command_result = self._read_result(deadline)
        retrain_result = (
            self._read_result(deadline) if command_result == "ok" else None
        )
        return ReturnOnlineReport(
            escape_result, command_result, retrain_result
        )

    def probe_initial_data(self, timeout_seconds: float) -> tuple[bool, bytes]:
        return self.exchange_online(
            b"",
            ONLINE_INITIAL_RESPONSE,
            timeout_seconds,
        )

    def start_remote_retrain(
        self, timeout_seconds: float
    ) -> tuple[bytes | None, bytes]:
        verified, received = self.exchange_online_pattern(
            REMOTE_RETRAIN_REQUEST,
            REMOTE_RETRAIN_START_RE,
            timeout_seconds,
        )
        match = REMOTE_RETRAIN_START_RE.search(received) if verified else None
        return (match.group("nonce").lower() if match is not None else None), received

    def complete_remote_retrain(
        self,
        nonce: bytes,
        received: bytes,
        timeout_seconds: float,
    ) -> RemoteRetrainReport:
        if (
            not re.fullmatch(rb"[0-9a-f]{32}", nonce)
            or not isinstance(received, bytes)
            or len(received) > ONLINE_CAPTURE_MAX
        ):
            raise LiveTerminalError("invalid remote retrain completion")
        pattern = re.compile(
            rb"GETTY:RETRAIN:END:"
            + re.escape(nonce)
            + rb":[0-9]{1,20}(?:\r\n|\r|\n)"
        )
        _, combined = self._exchange_online(
            b"",
            lambda data: pattern.search(data) is not None,
            timeout_seconds,
            initial=received,
        )
        complete = pattern.search(combined) is not None
        return RemoteRetrainReport(
            complete,
            combined[len(received):],
        )

    def probe_online_ping(self, timeout_seconds: float) -> tuple[bool, bytes]:
        return self._exchange_online(
            ONLINE_PING_REQUEST,
            lambda received: ONLINE_PONG_RE.search(received) is not None,
            timeout_seconds,
        )

    def _exchange_online(
        self,
        send: bytes,
        complete: Callable[[bytearray], bool],
        timeout_seconds: float,
        *,
        initial: bytes = b"",
    ) -> tuple[bool, bytes]:
        if (
            not isinstance(send, bytes)
            or len(send) > ONLINE_PATTERN_MAX
            or not isinstance(initial, bytes)
            or len(initial) > ONLINE_CAPTURE_MAX
            or timeout_seconds <= 0
            or not self._decoder.online
        ):
            raise LiveTerminalError("invalid online terminal exchange")
        deadline = time.monotonic() + timeout_seconds
        if send:
            self._write(send, deadline)
        received = bytearray(initial)
        received.extend(self._decoder.take_online_data())
        while not complete(received):
            if len(received) >= ONLINE_CAPTURE_MAX:
                raise LiveTerminalError("online terminal data is too long")
            try:
                _wait_for_descriptor(
                    self._descriptor, selectors.EVENT_READ, deadline
                )
            except TimeoutError:
                return False, bytes(received)
            try:
                data = os.read(
                    self._descriptor,
                    min(4096, ONLINE_CAPTURE_MAX - len(received)),
                )
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                raise OSError("modem terminal closed")
            self._decoder.feed(data)
            received.extend(self._decoder.take_online_data())
        return True, bytes(received)

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
