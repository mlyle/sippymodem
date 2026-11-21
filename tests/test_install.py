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
        for system in ("Linux", "Darwin", "Windows"):
            with self.subTest(system=system):
                self.check_staged_install(system)

    def test_relocated_sdk_handles_paths_with_spaces(self) -> None:
        self.check_staged_install("Linux", relocatable=True)

    def check_staged_install(self, system: str, relocatable: bool = False) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            tree, stage = work / "source", work / "stage"
            for directory in ("src", "build", "tools", "examples/systemd"):
                (tree / directory).mkdir(parents=True)
            for name in ("build/sippymodem.pc.in", "tools/pkg_config.py", "build/version"):
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
            if system != "Linux":
                installed = {name: value for name, value in installed.items()
                             if not name.startswith("share/sippymodem/examples/")}
            if system == "Darwin":
                del installed["lib/libsippymodem.so"]
                installed["lib/libsippymodem.dylib"] = ("src/libsippymodem.dylib", 0o755)
            elif system == "Windows":
                del installed["bin/slmodemd"]
                del installed["lib/libsippymodem.so"]
                installed["bin/libsippymodem.dll"] = ("src/libsippymodem.dll", 0o755)
                installed["lib/libsippymodem.dll.a"] = ("src/libsippymodem.dll.a", 0o644)
            for source, _mode in installed.values():
                (tree / source).write_text(source + "\n", encoding="ascii")
            prefix = "/opt/sippy modem"
            destination = stage / prefix.lstrip("/")
            destination.mkdir(parents=True)
            unrelated = destination / "keep.me"
            unrelated.write_text("local configuration\n", encoding="ascii")
            command = ["make", "--no-print-directory", "-f", str(ROOT / "build/install.mk"),
                       "--eval=all: ; @:", f"PREFIX={prefix}", f"DESTDIR={stage}",
                       f"TARGET_SYSTEM={system}", f"RELOCATABLE_SDK={int(relocatable)}"]
            result = subprocess.run(command + ["install"], cwd=tree, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, (source, mode) in installed.items():
                path = destination / name
                self.assertEqual(path.read_bytes(), (tree / source).read_bytes())
                self.assertEqual(path.stat().st_mode & 0o777, mode)
            metadata = destination / "lib/pkgconfig/sippymodem.pc"
            if not relocatable:
                self.assertIn("prefix=" + prefix + "\n", metadata.read_text())
            self.assertNotIn(str(stage), metadata.read_text())
            version = (ROOT / "build/version").read_text().strip()
            self.assertIn("Version: " + version + "\n", metadata.read_text())
            # Installed shared consumers need only the public SDK metadata.
            environment = dict(os.environ, PKG_CONFIG_LIBDIR=str(metadata.parent),
                               PKG_CONFIG_PATH="")
            flags = subprocess.check_output(["pkg-config", "--cflags", "--libs",
                                             "sippymodem"], env=environment, text=True)
            arguments = shlex.split(flags)
            if relocatable:
                for option, relative in (("-I", "include"), ("-L", "lib")):
                    resolved = [Path(flag[2:]).resolve() for flag in arguments
                                if flag.startswith(option)]
                    self.assertEqual(resolved, [destination / relative])
            else:
                self.assertIn("-I" + prefix + "/include", arguments)
                self.assertIn("-L" + prefix + "/lib", arguments)
            self.assertIn("-lsippymodem", arguments)
            if system == "Windows":
                flags = subprocess.check_output(["pkg-config", "--static", "--libs",
                                                 "sippymodem"], env=environment, text=True)
                self.assertIn("-lws2_32", shlex.split(flags))
                self.assertIn("-lbcrypt", shlex.split(flags))
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
        ], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
