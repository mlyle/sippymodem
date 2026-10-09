#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check target selection and native shared-library link contracts."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlatformBuildTests(unittest.TestCase):
    def test_compiler_target_selects_library_format(self):
        for triple, system, library, link_flag in (
            ("x86_64-linux-gnu", "Linux", "libsippymodem.so", "--version-script"),
            ("arm64-apple-darwin", "Darwin", "libsippymodem.dylib", "-dynamiclib"),
            ("x86_64-w64-mingw32", "Windows", "libsippymodem.dll", "--no-undefined"),
        ):
            with self.subTest(triple=triple), tempfile.TemporaryDirectory() as temporary:
                compiler = Path(temporary) / "compiler.py"
                compiler.write_text("import sys\nassert sys.argv[1:] == ['-dumpmachine']\nprint(" + repr(triple) + ")\n")
                output = subprocess.check_output([
                    "make", "--no-print-directory", "-s", "-f", str(ROOT / "build/platform.mk"),
                    "--eval=print: ; @printf '%s\\n' '$(TARGET_SYSTEM)' '$(SHARED_LIBRARY)' '$(SHARED_LINK_FLAGS)' '$(AR)'",
                    "print", "CC=" + sys.executable + " " + str(compiler),
                ], text=True, timeout=10).splitlines()
                self.assertEqual(output[:2], [system, library])
                self.assertIn(link_flag, output[2])
                if system == "Windows":
                    self.assertEqual(output[3], triple + "-ar")
                if system != "Linux":
                    self.assertNotIn("--version-script", output[2])

    def test_explicit_target_wins_over_compiler_detection(self):
        output = subprocess.check_output([
            "make", "--no-print-directory", "-s", "-f", str(ROOT / "build/platform.mk"),
            "--eval=print: ; @printf '%s\\n' '$(SHARED_LIBRARY)'",
            "print", "TARGET_SYSTEM=Darwin", "CC=false",
        ], text=True, timeout=10)
        self.assertEqual(output.strip(), "libsippymodem.dylib")

    def test_windows_frontend_reports_its_posix_requirement(self):
        result = subprocess.run([
            "make", "--no-print-directory", "slmodemd", "TARGET_SYSTEM=Windows",
            "PJSIP_AVAILABLE=yes", "PKG_CONFIG=false", "CCACHE=",
        ], cwd=ROOT / "src", capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PTY frontend requires a POSIX host", result.stderr)


if __name__ == "__main__":
    unittest.main()
