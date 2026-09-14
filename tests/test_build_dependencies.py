#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class HeaderDependencyTests(unittest.TestCase):
    def test_generated_dependencies_trigger_an_incremental_rebuild(self) -> None:
        # This test object was absent from the hand-maintained dependency list.
        source = "test_v34_phase4_decision_receiver.c"
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            target = build / Path(source).with_suffix(".o")
            header = build / "layout.h"
            dependency = target.with_suffix(".d")
            target.write_bytes(b"fixture object\n")
            header.write_text("/* fixture layout */\n", encoding="utf-8")
            dependency.write_text(f"{target}: {header}\n", encoding="utf-8")
            os.utime(header, (1, 1))
            os.utime(target, (2, 2))
            command = [
                "make", "--no-print-directory", "--question",
                f"--assume-old={source}", f"BUILD_DIR={build}",
                "PJSIP_AVAILABLE=no", "PKG_CONFIG=false", str(target),
            ]

            def needs_rebuild() -> int:
                result = subprocess.run(
                    command, cwd=ROOT / "tests", capture_output=True,
                    text=True, timeout=10,
                )
                self.assertIn(result.returncode, (0, 1), result.stderr)
                return result.returncode

            self.assertEqual(needs_rebuild(), 0)
            os.utime(header, (3, 3))
            self.assertEqual(needs_rebuild(), 1, "header change was ignored")
            os.utime(target, (4, 4))
            self.assertEqual(needs_rebuild(), 0)
            # Querying the graph must not invoke a compiler or alter fixtures.
            self.assertEqual(target.read_bytes(), b"fixture object\n")


if __name__ == "__main__":
    unittest.main()
