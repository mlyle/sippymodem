#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Own the processes, terminals, ports, and evidence for paired modem calls."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import stat
import subprocess
import tempfile
import threading
import time
from typing import IO, Mapping


if __package__:
    from .interop_config import (
        EndpointConfig,
        InteropPorts,
        LOOPBACK_ADDRESS,
        PORT_NAMES,
    )
    from .live_terminal import ModemTerminal
else:
    from interop_config import (
        EndpointConfig,
        InteropPorts,
        LOOPBACK_ADDRESS,
        PORT_NAMES,
    )
    from live_terminal import ModemTerminal


STARTUP_LINE_MAX = 4096
UNSOLICITED_LINE_MAX = 512
DAEMON_LOG_MAX = 4 * 1024 * 1024
DEVICE_LINE_RE = re.compile(rb"/dev/pts/[0-9]+\Z")
TTY_ANNOUNCEMENT_RE = re.compile(
    rb"\bTTY[ \t]+is[ \t]+[`'](/dev/pts/[0-9]+)[`']",
    re.IGNORECASE,
)


class InteropRunError(RuntimeError):
    """A bounded interop operation could not complete."""


def remaining(deadline: float) -> float:
    duration = deadline - time.monotonic()
    if duration <= 0:
        raise TimeoutError
    return duration


def _require_private_directory(path: Path) -> None:
    try:
        attributes = path.lstat()
    except OSError as error:
        raise InteropRunError("private artifact directory is unavailable") from error
    if (
        not stat.S_ISDIR(attributes.st_mode)
        or stat.S_ISLNK(attributes.st_mode)
        or attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise InteropRunError("artifact directory must be private and owner-owned")


def _open_private(path: Path, mode: str) -> IO[bytes] | IO[str]:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    if "b" in mode:
        return os.fdopen(descriptor, mode)
    return os.fdopen(descriptor, mode, encoding="utf-8")


@dataclass(frozen=True)
class RunArtifacts:
    directory: Path
    result_file: Path
    endpoint_directories: dict[str, Path]

    @classmethod
    def create(cls, root: Path) -> RunArtifacts:
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        _require_private_directory(root)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = Path(tempfile.mkdtemp(prefix=f"{stamp}-", dir=root))
        _require_private_directory(directory)
        endpoint_directories: dict[str, Path] = {}
        for role in ("originator", "answerer"):
            endpoint_directory = directory / role
            endpoint_directory.mkdir(mode=0o700)
            _require_private_directory(endpoint_directory)
            endpoint_directories[role] = endpoint_directory
        return cls(directory, directory / "result.json", endpoint_directories)

    def daemon_log(self, role: str) -> Path:
        return self.endpoint_directories[role] / "daemon.log"

    def recording_prefix(self, role: str) -> Path:
        return self.endpoint_directories[role] / "media"

    def write_exchange(self, role: str, transmitted: bytes, received: bytes) -> None:
        directory = self.endpoint_directories[role]
        with _open_private(directory / "terminal-tx.bin", "wb") as stream:
            stream.write(transmitted)
        with _open_private(directory / "terminal-rx.bin", "wb") as stream:
            stream.write(received)

    def write_result(self, result: Mapping[str, object]) -> None:
        with _open_private(self.result_file, "w") as stream:
            json.dump(dict(result), stream, sort_keys=True)
            stream.write("\n")


def reserve_loopback_ports() -> InteropPorts:
    """Select four distinct UDP ports while holding every earlier choice."""

    sockets: list[socket.socket] = []
    numbers: list[int] = []
    try:
        while len(numbers) < len(PORT_NAMES):
            reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                reservation.bind((LOOPBACK_ADDRESS, 0))
            except OSError:
                reservation.close()
                raise
            port = int(reservation.getsockname()[1])
            if port in numbers:
                reservation.close()
                continue
            sockets.append(reservation)
            numbers.append(port)
    except OSError as error:
        raise InteropRunError("cannot reserve loopback ports") from error
    finally:
        for reservation in sockets:
            reservation.close()
    return InteropPorts(**dict(zip(PORT_NAMES, numbers)))


class InteropTerminal(ModemTerminal):
    """Coordinate unsolicited ringing and deadline-bounded call results."""

    def __init__(self, descriptor: int) -> None:
        super().__init__(descriptor)
        self._unsolicited_line = bytearray()
        self._discarding_unsolicited_line = False

    def _contains_ring(self, data: bytes) -> bool:
        found = False
        for byte in data:
            if byte in (0x0A, 0x0D):
                if not self._discarding_unsolicited_line:
                    line = bytes(self._unsolicited_line).strip().upper()
                    found = found or line in {b"RING", b"2"}
                self._unsolicited_line.clear()
                self._discarding_unsolicited_line = False
            elif not self._discarding_unsolicited_line:
                if len(self._unsolicited_line) < UNSOLICITED_LINE_MAX:
                    self._unsolicited_line.append(byte)
                else:
                    self._unsolicited_line.clear()
                    self._discarding_unsolicited_line = True
        return found

    def wait_ring_until(self, deadline: float) -> str:
        while True:
            try:
                readable, _, _ = select.select(
                    [self._descriptor], [], [], remaining(deadline)
                )
            except InterruptedError:
                continue
            if not readable:
                raise TimeoutError
            try:
                data = os.read(self._descriptor, 4096)
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                raise OSError("modem terminal closed")
            found_ring = self._contains_ring(data)
            self._decoder.feed(data)
            result = self._decoder.pop()
            while result == "ok":
                result = self._decoder.pop()
            if result is not None:
                return result
            if found_ring:
                return "ring"

    def wait_connect_until(self, deadline: float) -> str:
        result = self._read_result(deadline)
        while result == "ok":
            result = self._read_result(deadline)
        return result

    def dial_until(self, deadline: float) -> str:
        result = self.command("ATD", remaining(deadline))
        return self.wait_connect_until(deadline) if result == "ok" else result

    def start_answer(self, deadline: float) -> None:
        self._write(b"ATA\r", deadline)

    def answer_until(self, deadline: float) -> str:
        ring = self.wait_ring_until(deadline)
        if ring != "ring":
            return ring
        self.start_answer(deadline)
        return self.wait_connect_until(deadline)


def published_device(line: bytes) -> str | None:
    candidate = line.rstrip(b"\r")
    if DEVICE_LINE_RE.fullmatch(candidate) is not None:
        return candidate.decode("ascii")
    match = TTY_ANNOUNCEMENT_RE.search(candidate)
    return match.group(1).decode("ascii") if match is not None else None


class EndpointProcess:
    """Own one child process, its private output capture, and its terminal."""

    def __init__(
        self,
        role: str,
        config: EndpointConfig,
        environment: Mapping[str, str],
        daemon_log: Path,
    ) -> None:
        if role not in {"originator", "answerer"}:
            raise InteropRunError("invalid endpoint role")
        self.role = role
        self.config = config
        self.environment = dict(environment)
        self.daemon_log = daemon_log
        self.process: subprocess.Popen[bytes] | None = None
        self.terminal: InteropTerminal | None = None
        self.log_truncated = False
        self._device_path: str | None = None
        self._device_ready = threading.Event()
        self._output_done = threading.Event()
        self._reader: threading.Thread | None = None

    def start(self, timeout_seconds: float) -> None:
        if self.process is not None:
            raise InteropRunError("endpoint process is already started")
        try:
            process = subprocess.Popen(
                [str(self.config.executable), *self.config.arguments],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=self.environment,
                close_fds=True,
                start_new_session=True,
            )
        except OSError as error:
            raise InteropRunError("endpoint process could not start") from error
        self.process = process
        assert process.stdout is not None
        self._reader = threading.Thread(
            target=self._capture_output,
            args=(process.stdout,),
            name=f"interop-{self.role}-log",
            daemon=True,
        )
        self._reader.start()
        deadline = time.monotonic() + timeout_seconds
        while not self._device_ready.is_set():
            duration = deadline - time.monotonic()
            if duration <= 0:
                raise InteropRunError("endpoint terminal announcement timed out")
            if self._output_done.is_set() or process.poll() is not None:
                raise InteropRunError("endpoint exited before publishing a terminal")
            self._device_ready.wait(min(duration, 0.05))
        assert self._device_path is not None
        try:
            self.terminal = InteropTerminal.open(self._device_path)
        except OSError as error:
            raise InteropRunError("endpoint terminal could not be opened") from error

    def _capture_output(self, stream: IO[bytes]) -> None:
        buffer = bytearray()
        discarding_line = False
        written = 0
        try:
            with stream, _open_private(self.daemon_log, "wb") as log:
                while True:
                    try:
                        chunk = os.read(stream.fileno(), 4096)
                    except InterruptedError:
                        continue
                    if not chunk:
                        break
                    available = max(0, DAEMON_LOG_MAX - written)
                    if available:
                        saved = chunk[:available]
                        log.write(saved)
                        written += len(saved)
                    if len(chunk) > available:
                        self.log_truncated = True
                    buffer.extend(chunk)
                    while b"\n" in buffer:
                        line, _, remainder = buffer.partition(b"\n")
                        buffer = bytearray(remainder)
                        if not discarding_line and len(line) <= STARTUP_LINE_MAX:
                            path = published_device(line)
                            if path is not None and self._device_path is None:
                                self._device_path = path
                                self._device_ready.set()
                        discarding_line = False
                    if len(buffer) > STARTUP_LINE_MAX:
                        buffer.clear()
                        discarding_line = True
        except OSError:
            pass
        finally:
            self._output_done.set()

    def _signal(self, signal_number: signal.Signals) -> None:
        assert self.process is not None
        try:
            os.killpg(self.process.pid, signal_number)
        except OSError:
            try:
                self.process.send_signal(signal_number)
            except OSError:
                pass

    def stop(self, timeout_seconds: float) -> bool:
        if self.terminal is not None:
            try:
                self.terminal.close()
            except OSError:
                pass
            self.terminal = None
        process = self.process
        if process is not None and process.poll() is None:
            self._signal(signal.SIGTERM)
            try:
                process.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                self._signal(signal.SIGKILL)
                try:
                    process.wait(timeout=timeout_seconds)
                except subprocess.TimeoutExpired:
                    pass
        elif process is not None:
            try:
                process.wait(timeout=0)
            except subprocess.TimeoutExpired:
                pass
        self._output_done.wait(timeout_seconds)
        if self._reader is not None:
            self._reader.join(timeout_seconds)
        return (process is None or process.poll() is not None) and (
            self._reader is None or not self._reader.is_alive()
        )
