#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


class CompilerBuildTests(unittest.TestCase):
    def values(
        self,
        directory: Path,
        compiler: str,
        cache: str,
        *,
        cxx: str = "c++",
        cache_directory: str | None = None,
    ) -> tuple[str, str, str]:
        fragment = ROOT / "build" / "compiler.mk"
        arguments = [
            "make",
            "--no-print-directory",
            "--silent",
            "-f",
            str(fragment),
            "--eval=print: ; @printf '%s\\n' '$(CC)' '$(CXX)' '$(CCACHE_DIR)'",
            "print",
            f"CC={compiler}",
            f"CXX={cxx}",
            f"CCACHE={cache}",
        ]
        if cache_directory is not None:
            arguments.append(f"CCACHE_DIR={cache_directory}")
        environment = os.environ.copy()
        # Test the fragment's defaults, independently of the parent build's
        # exported cache settings (including CCACHE= exporting empty values).
        environment.pop("CCACHE_DIR", None)
        environment.pop("CCACHE_BASEDIR", None)
        output = subprocess.run(
            arguments,
            cwd=directory,
            env=environment,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.splitlines()
        self.assertEqual(len(output), 3)
        return output[0], output[1], output[2]

    def test_wraps_custom_compiler_once(self) -> None:
        compiler, cxx, cache_directory = self.values(
            ROOT,
            "clang",
            "/tool/ccache",
            cxx="clang++",
        )
        self.assertEqual(compiler, "/tool/ccache clang")
        self.assertEqual(cxx, "/tool/ccache clang++")
        self.assertEqual(cache_directory, str(ROOT / ".build" / "ccache"))

        compiler, cxx, _cache_directory = self.values(
            ROOT,
            "/tool/ccache clang",
            "/tool/ccache",
            cxx="/tool/ccache clang++",
        )
        self.assertEqual(compiler, "/tool/ccache clang")
        self.assertEqual(cxx, "/tool/ccache clang++")

    def test_empty_ccache_disables_wrapping(self) -> None:
        compiler, cxx, cache_directory = self.values(
            ROOT,
            "clang",
            "",
            cxx="clang++",
        )
        self.assertEqual(compiler, "clang")
        self.assertEqual(cxx, "clang++")
        self.assertEqual(cache_directory, "")

    def test_default_cache_ignores_parent_cache_settings(self) -> None:
        for directory in ("", "/parent-build/cache"):
            with self.subTest(directory=directory), mock.patch.dict(
                os.environ,
                {"CCACHE_DIR": directory, "CCACHE_BASEDIR": "/parent-build"},
            ):
                _compiler, _cxx, actual = self.values(
                    ROOT, "clang", "/tool/ccache"
                )
                self.assertEqual(actual, str(ROOT / ".build" / "ccache"))

    def test_direct_subdirectory_build_uses_shared_cache(self) -> None:
        compiler, cxx, cache_directory = self.values(
            ROOT / "src",
            "gcc",
            "ccache",
            cxx="g++",
            cache_directory="/tmp/compiler-cache",
        )
        self.assertEqual(compiler, "ccache gcc")
        self.assertEqual(cxx, "ccache g++")
        self.assertEqual(cache_directory, "/tmp/compiler-cache")


if __name__ == "__main__":
    unittest.main()
