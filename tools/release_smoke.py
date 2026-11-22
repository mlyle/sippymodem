#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the CLI and shared SDK in an installed or extracted release."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from interop_config import EndpointConfig, InteropConfig, InteropPorts
from local_interop import run_interop

ROOT = Path(__file__).resolve().parents[1]


def check_cli(package: Path, work: Path) -> None:
    executable = package / "bin/slmodemd"
    base = ("ATZ", "ATE0", "ATX3", "AT&A7")
    raw = ("AT+MS=122,0,2400,2400", "AT\\N0")
    lapm = ("AT+MS=34,0,4800,33600", "AT\\N4", "AT+DS=0")
    for name, origination, answer, modulation, protocol in (
            ("legacy-fallback", (), raw, "V.22bis", "none"),
            ("v34-lapm", lapm, lapm, "V.34", "LAPM")):
        environment = (("SIP_LOG_LEVEL", "2"),)
        result = run_interop(InteropConfig(
            enabled=True,
            artifact_directory=work / name,
            originator=EndpointConfig(executable, (), environment, base + origination),
            answerer=EndpointConfig(executable, (), environment, base + answer),
            originator_payload=b"ORIG-TO-ANSWER:\x00\x11\x13\xff",
            answerer_payload=b"ANSWER-TO-ORIG:\x00\x11\x13\xff",
            startup_timeout_seconds=10,
            setup_timeout_seconds=5,
            connect_timeout_seconds=45,
            online_timeout_seconds=15,
            cleanup_timeout_seconds=5,
            ports=InteropPorts(),
        ))
        evidence = result.public_dict()
        print(name, json.dumps(evidence, sort_keys=True), flush=True)
        if result.outcome != "passed" or any(
                evidence[side]["modulation"] != modulation
                or evidence[side]["protocol"] != protocol
                for side in ("originator", "answerer")):
            raise ValueError(f"{name} did not exchange data with the expected link")


def check_sdk(package: Path, work: Path) -> None:
    environment = dict(os.environ, PKG_CONFIG_LIBDIR=str(package / "lib/pkgconfig"),
                       PKG_CONFIG_PATH="")
    for name in ("PKG_CONFIG_SYSROOT_DIR", "LD_LIBRARY_PATH",
                 "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        environment.pop(name, None)
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "sippymodem"],
        cwd=work, env=environment, text=True, timeout=10)
    executable = work / "installed-sdk-test"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DSIPPYMODEM_INSTALLED_TEST",
        "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_library_wait.c"),
        "-o", str(executable), *shlex.split(flags),
        "-Xlinker", "-rpath", "-Xlinker", str(package / "lib"),
    ], cwd=work, env=environment, check=True, timeout=30)
    subprocess.run([executable], cwd=work, env=environment, check=True, timeout=30)
    print("installed SDK exchanged literal bytes over SIP", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="directory containing bin/, lib/ and include/")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="sippymodem-smoke-") as directory:
        work = Path(directory)
        check_cli(args.package.resolve(), work)
        check_sdk(args.package.resolve(), work)


if __name__ == "__main__":
    main()
