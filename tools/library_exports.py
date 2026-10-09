#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate a Windows export definition from the public C headers."""

from pathlib import Path
import re
import sys


def exports(headers: list[Path]) -> list[str]:
    names: set[str] = set()
    for header in headers:
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(), flags=re.S)
        names.update(re.findall(r"\b(sippymodem_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*;", text))
    if not names:
        raise ValueError("no public function declarations found")
    return sorted(names)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: library_exports.py HEADER...", file=sys.stderr)
        return 2
    try:
        names = exports([Path(name) for name in sys.argv[1:]])
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    sys.stdout.write("EXPORTS\n" + "".join("    " + name + "\n" for name in names))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
