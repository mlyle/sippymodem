#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Emit SDK metadata, keeping the existing modem compatibility version."""

from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prefix", "libdir", "includedir", "pjsip-package"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--private-libs", default="-lm")
    args = parser.parse_args()
    values = vars(args)
    if any("\n" in value or "\r" in value for value in values.values()):
        parser.error("pkg-config values must fit on one line")
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", args.pjsip_package):
        parser.error("invalid PJSIP package name")
    if not re.fullmatch(r"(?:-l[A-Za-z0-9_+.-]+(?: +|$))*", args.private_libs):
        parser.error("invalid private library flags")
    values["private_libs"] = " ".join(args.private_libs.split())
    source = (ROOT / "src/modem.c").read_text(encoding="utf-8")
    versions = re.findall(r'^#define MODEM_VERSION "([0-9.]+)"$', source, re.M)
    if len(versions) != 1:
        parser.error("expected one modem compatibility version")
    values["version"] = versions[0]
    text = (ROOT / "build/sippymodem.pc.in").read_text(encoding="utf-8")
    for name, value in values.items():
        text = text.replace("@" + name + "@", value)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
