#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run one foreground modem with splitvt trace and C-Kermit panes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile

if __package__:
    from .live_terminal import LiveTerminalError, read_published_device
else:
    from live_terminal import LiveTerminalError, read_published_device


ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENT_KEYS = frozenset("""
    SIP_AUTO_ANSWER SIP_BOUND_ADDR SIP_CODEC SIP_DOMAIN SIP_FRAME_MS SIP_GAIN
    SIP_ID SIP_JB_MS SIP_LOGIN SIP_LOG_LEVEL SIP_PASSWORD SIP_PCM_CODEC_GRID SIP_PORT SIP_PROXY
    SIP_PUBLIC_ADDR SIP_REALM SIP_REGISTER SIP_REG_URI SIP_REMOTE_CLOCK
    SIP_RTP_PORT SIP_RX_GAIN SIP_SERVER SIP_TARGET SIP_TRACE SIP_TX_GAIN SIP_USER
""".split())


def load_profile(path: Path, profile: str) -> dict[str, str]:
    # A profile is data, never a shell script. Do not echo malformed contents:
    # JSON parser errors and unknown keys can themselves contain credentials.
    with path.open("rb") as stream:
        metadata = os.fstat(stream.fileno())
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode & 0o077:
            raise ValueError("profile file must be regular and owner-only (chmod 600)")
        content = stream.read(65537)
    if len(content) > 65536:
        raise ValueError("profile file exceeds 64 KiB")
    try:
        profiles = json.loads(content)
    except (ValueError, UnicodeError):
        raise ValueError("profile file must contain valid JSON") from None
    if not isinstance(profiles, dict) or profile not in profiles:
        raise ValueError("selected provider profile is missing")
    values = profiles[profile]
    if not isinstance(values, dict) or not values:
        raise ValueError("provider profile must contain SIP environment settings")
    for key, value in values.items():
        if key not in ENVIRONMENT_KEYS:
            raise ValueError("provider profile contains an unsupported setting")
        if (not isinstance(value, str) or len(value) > 1024
                or any(ord(char) < 32 or ord(char) == 127 for char in value)):
            raise ValueError("provider settings must be bounded single-line strings")
    return dict(values)


def clean_environment() -> dict[str, str]:
    # Never let another shell session's provider settings leak into this one,
    # or give credentials to the terminal programs.
    return {key: value for key, value in os.environ.items()
            if not key.startswith("SIP_")}


def stop(child: subprocess.Popen | None) -> None:
    if child is None or child.poll() is not None:
        return
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def terminal_pane(kermit: str, device: str, completed: Path) -> int:
    """Tell the owner when Kermit exits, including a splitvt window hangup."""
    child = None
    result = 1
    try:
        child = subprocess.Popen([
            kermit, "-Y", "-l", device, "-b", "115200", "-C",
            "set carrier-watch off,set flow-control none,"
            "set exit warning off,connect",
        ])
        result = child.wait()
        return result
    finally:
        stop(child)
        completed.write_text("0" if result == 0 else "1", encoding="ascii")


def run(modem: Path, values: dict[str, str], splitvt: str, kermit: str) -> int:
    environment = clean_environment()
    modem_environment = dict(environment, SIP_REGISTER="1", SIP_TRACE="1")
    modem_environment.update(values)
    child = screen = None
    failed = False
    # splitvt splits pane commands on whitespace without shell quoting. Alias
    # the interpreter/script under a private, whitespace-free path; all other
    # pane arguments travel as environment values, never shell fragments.
    with tempfile.TemporaryDirectory(prefix="modem-terminal-", dir="/tmp") as directory:
        work = Path(directory)
        (work / "python").symlink_to(sys.executable)
        (work / "entry").symlink_to(Path(__file__).resolve())
        trace = work / "trace.log"
        trace.touch(mode=0o600)
        completed = work / "terminal.done"
        with trace.open("wb", buffering=0) as output:
            try:
                child = subprocess.Popen(
                    [str(modem)], stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=output,
                    env=modem_environment, start_new_session=True,
                )
                device = read_published_device(child.stdout, 15)
                environment.update(MODEM_TERMINAL_KERMIT=kermit,
                                   MODEM_TERMINAL_DEVICE=device,
                                   MODEM_TERMINAL_DONE=str(completed))
                upper = f"tail -n +1 -f {trace}"
                lower = f"{work}/python {work}/entry --terminal-pane"
                screen = subprocess.Popen([
                    splitvt, "-norc", "-nologin", "-bottom",
                    "-upper", upper, "-lower", lower,
                ], env=environment)
                while True:
                    try:
                        return screen.wait(timeout=0.2)
                    except subprocess.TimeoutExpired:
                        if completed.exists():
                            status = completed.read_text(encoding="ascii")
                            if status:
                                return int(status)
                        if child.poll() is not None:
                            raise RuntimeError("modem exited while the terminal was open")
            except Exception:
                failed = True
                raise
            finally:
                stop(screen)
                stop(child)
                if child is not None and child.stdout is not None:
                    child.stdout.close()
                if failed:
                    with trace.open("rb") as recorded:
                        size = recorded.seek(0, os.SEEK_END)
                        recorded.seek(max(0, size - 8192))
                        sys.stderr.write(recorded.read().decode("utf-8", "replace"))


def interrupted(_number: int, _frame: object) -> None:
    raise KeyboardInterrupt


def main() -> int:
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    if sys.argv[1:] == ["--terminal-pane"]:
        try:
            return terminal_pane(os.environ["MODEM_TERMINAL_KERMIT"],
                                 os.environ["MODEM_TERMINAL_DEVICE"],
                                 Path(os.environ["MODEM_TERMINAL_DONE"]))
        except KeyboardInterrupt:
            return 130
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", help="profile name from the configuration (e.g. pstn or ata)")
    parser.add_argument("--config", type=Path, default=ROOT / ".sip-terminal.json")
    parser.add_argument("--modem", type=Path, default=ROOT / "src/slmodemd")
    args = parser.parse_args()
    try:
        values = load_profile(args.config, args.profile)
        programs = [shutil.which(name) for name in ("splitvt", "kermit", "tail")]
        if not all(programs):
            raise ValueError("install splitvt, C-Kermit and tail before launching")
        if not args.modem.is_file() or not os.access(args.modem, os.X_OK):
            raise ValueError("modem executable is missing; run make first")
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            raise ValueError("splitvt needs an interactive terminal")
        print("Trace above; modem terminal below. Ctrl-O q quits both panes.\n"
              "C-Kermit: Ctrl-\\ then Q quits and stops the modem; "
              "Ctrl-\\ then C returns to its command prompt.", flush=True)
        return run(args.modem.resolve(), values, programs[0], programs[1])
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError, LiveTerminalError, TimeoutError) as error:
        print(f"modem terminal: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
