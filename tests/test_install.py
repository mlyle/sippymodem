#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise staged installation without compiling or changing the host."""

from pathlib import Path
import shutil
import shlex
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallTests(unittest.TestCase):
    def test_staged_install_and_uninstall_preserve_unrelated_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            tree, stage = work / "source", work / "stage"
            for directory in ("src", "build", "tools", "examples/systemd"):
                (tree / directory).mkdir(parents=True)
            for name in ("build/sippymodem.pc.in", "tools/pkg_config.py", "src/modem.c"):
                shutil.copy2(ROOT / name, tree / name)
            installed = {
                "bin/slmodemd": ("src/slmodemd", 0o755),
                "lib/libsippymodem.so": ("src/libsippymodem.so", 0o755),
                "lib/libsippymodem.a": ("src/libsippymodem.a", 0o644),
                "include/sippymodem.h": ("src/sippymodem.h", 0o644),
                "include/sippymodem_fd.h": ("src/sippymodem_fd.h", 0o644),
                "share/doc/sippymodem/COPYING": ("src/COPYING", 0o644),
                "share/sippymodem/examples/systemd/sippymodem@.service":
                    ("examples/systemd/sippymodem@.service", 0o644),
                "share/sippymodem/examples/systemd/default.env.example":
                    ("examples/systemd/default.env.example", 0o644),
                "share/sippymodem/examples/systemd/README.md":
                    ("examples/systemd/README.md", 0o644),
            }
            for source, _mode in installed.values():
                (tree / source).write_text(source + "\n", encoding="ascii")
            prefix = "/opt/sippy modem"
            destination = stage / prefix.lstrip("/")
            destination.mkdir(parents=True)
            unrelated = destination / "keep.me"
            unrelated.write_text("local configuration\n", encoding="ascii")
            command = ["make", "--no-print-directory", "-f", str(ROOT / "build/install.mk"),
                       "--eval=all: ; @:", f"PREFIX={prefix}", f"DESTDIR={stage}",
                       "PJSIP_PACKAGE=libpjproject"]
            result = subprocess.run(command + ["install"], cwd=tree, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, (source, mode) in installed.items():
                path = destination / name
                self.assertEqual(path.read_bytes(), (tree / source).read_bytes())
                self.assertEqual(path.stat().st_mode & 0o777, mode)
            metadata = destination / "lib/pkgconfig/sippymodem.pc"
            self.assertIn("prefix=" + prefix + "\n", metadata.read_text())
            self.assertNotIn(str(stage), metadata.read_text())
            dependency = metadata.parent / "libpjproject.pc"
            dependency.write_text("Name: PJSIP fixture\nDescription: Test dependency\n"
                                  "Version: 1\nLibs: -lpthread\n", encoding="ascii")
            environment = dict(os.environ, PKG_CONFIG_LIBDIR=str(metadata.parent),
                               PKG_CONFIG_PATH="")
            flags = subprocess.check_output(["pkg-config", "--cflags", "--static", "--libs",
                                             "sippymodem"], env=environment, text=True)
            self.assertIn("-I" + prefix + "/include", shlex.split(flags))
            self.assertIn("-L" + prefix + "/lib", shlex.split(flags))
            self.assertIn("-lpthread", shlex.split(flags))
            dependency.unlink()
            self.assertEqual(sorted(p.name for p in (destination / "include").iterdir()),
                             ["sippymodem.h", "sippymodem_fd.h"])
            result = subprocess.run(command + ["uninstall"], cwd=tree, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual([p for p in destination.rglob("*") if p.is_file()], [unrelated])

    def test_metadata_rejects_multiline_values(self) -> None:
        result = subprocess.run([
            "python3", str(ROOT / "tools/pkg_config.py"), "--prefix=/usr/local\nLibs: -levil",
            "--libdir=/usr/local/lib", "--includedir=/usr/local/include",
            "--pjsip-package=libpjproject",
        ], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
