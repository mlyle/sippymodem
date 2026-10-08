#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DefaultBuildTests(unittest.TestCase):
    def plan(self, *targets: str) -> str:
        return subprocess.run(
            ["make", "--no-print-directory", "--dry-run",
             "PJSIP_PROVIDER=none", "PKG_CONFIG=false", "CCACHE=", *targets],
            cwd=ROOT, capture_output=True, text=True, check=True, timeout=10,
        ).stdout

    def test_default_build_reaches_the_modem(self) -> None:
        explicit = self.plan("all")
        self.assertIn("-C src", explicit)
        self.assertEqual(self.plan(), explicit)

    def test_aliases_share_one_modem_build(self) -> None:
        self.assertEqual(self.plan("all", "slmodemd", "libsippymodem").count("-C src"), 1)

    def test_library_build_excludes_the_compatibility_frontend(self) -> None:
        plan = self.plan("libsippymodem")
        self.assertIn("libsippymodem", plan)
        command = ["make", "--no-print-directory", "--dry-run", "--always-make",
                   "PJSIP_AVAILABLE=yes", "PKG_CONFIG=false", "CCACHE=", "libsippymodem"]
        result = subprocess.run(command, cwd=ROOT / "src", capture_output=True,
                                text=True, check=True, timeout=10)
        self.assertIn("library.o", result.stdout)
        self.assertIn("libsippymodem.so", result.stdout)
        for frontend in ("main.o", "runtime.o", "terminal.o", "modem_at.o",
                         "modem_frontend.o", "slmodem_terminal.o", "log_target.o"):
            self.assertNotIn("-o " + frontend + " ", result.stdout)


class HeaderDependencyTests(unittest.TestCase):
    def test_phase_profile_is_required_and_generator_changes_rebuild(self) -> None:
        for directory in (ROOT / "src", ROOT / "tests"):
            with (
                self.subTest(directory=directory.name),
                tempfile.TemporaryDirectory() as temporary,
            ):
                fixture = Path(temporary)
                dsp = fixture / "dsp"
                build = fixture / "build"
                dsp.mkdir()
                build.mkdir()
                source = dsp / "fixed_phase.c"
                generator = dsp / "generate_phase_profile.py"
                header = fixture / "phase_profile.h"
                target = (dsp if directory.name == "src" else build) / "fixed_phase.o"
                for path in (source, generator, header):
                    path.write_text("/* fixture */\n", encoding="ascii")
                    os.utime(path, (1, 1))
                target.write_bytes(b"fixture object\n")
                os.utime(target, (2, 2))
                command = [
                    "make", "--no-print-directory", "--question",
                    "--assume-old=Makefile",
                    f"DSP_SOURCE_DIR={dsp}", f"BUILD_DIR={build}",
                    f"PHASE_PROFILE_HEADER={header}",
                    "PJSIP_AVAILABLE=no", "PKG_CONFIG=false", str(target),
                ]

                def needs_rebuild() -> int:
                    result = subprocess.run(
                        command, cwd=directory, capture_output=True,
                        text=True, timeout=10,
                    )
                    self.assertIn(result.returncode, (0, 1), result.stderr)
                    return result.returncode

                self.assertEqual(needs_rebuild(), 0)
                os.utime(generator, (3, 3))
                self.assertEqual(needs_rebuild(), 1, "generator change was ignored")
                os.utime(header, (4, 4))
                os.utime(target, (5, 5))
                self.assertEqual(needs_rebuild(), 0)
                header.unlink()
                self.assertEqual(needs_rebuild(), 1, "missing profile was ignored")
                self.assertEqual(target.read_bytes(), b"fixture object\n")

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
