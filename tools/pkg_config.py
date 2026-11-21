#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Emit SDK metadata, using the release version."""

from pathlib import Path
import argparse
import posixpath
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def read_version(root: Path) -> str:
    version = (root / "build/version").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("expected a release version in build/version")
    return version


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prefix", "libdir", "includedir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--private-libs", default="-lm")
    parser.add_argument("--relocatable", action="store_true")
    args = parser.parse_args()
    values = {name: getattr(args, name) for name in
              ("prefix", "libdir", "includedir", "private_libs")}
    if any("\n" in value or "\r" in value for value in values.values()):
        parser.error("pkg-config values must fit on one line")
    if not re.fullmatch(r"(?:-l[A-Za-z0-9_+.-]+(?: +|$))*", args.private_libs):
        parser.error("invalid private library flags")
    values["private_libs"] = " ".join(args.private_libs.split())
    try:
        values["version"] = read_version(ROOT)
    except ValueError as error:
        parser.error(str(error))
    if args.relocatable:
        root_from_pc = posixpath.relpath(args.prefix, args.libdir + "/pkgconfig")
        include_from_root = posixpath.relpath(args.includedir, args.prefix)
        values.update(prefix="${pcfiledir}/" + root_from_pc,
                      libdir="${pcfiledir}/..",
                      includedir="${prefix}/" + include_from_root)
    # pkg-config already escapes its generated pcfiledir. Quoting that
    # expansion would double-escape spaces in a relocated install.
    quote = "" if args.relocatable else '"'
    values.update(library_flag=f'-L{quote}${{libdir}}{quote}',
                  include_flag=f'-I{quote}${{includedir}}{quote}')
    text = (ROOT / "build/sippymodem.pc.in").read_text(encoding="utf-8")
    for name, value in values.items():
        text = text.replace("@" + name + "@", value)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
