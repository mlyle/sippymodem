#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.revision_build import (  # noqa: E402
    RevisionBuild,
    RevisionBuildError,
    freeze_revision_build,
)


class RevisionBuildTests(unittest.TestCase):
    def make_repository(self, root: Path) -> tuple[Path, str]:
        repository = root / "repository"
        repository.mkdir()
        git_environment = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("GIT_")
        }
        subprocess.run(
            ("git", "init", "-q", str(repository)),
            check=True,
            env=git_environment,
        )
        subprocess.run(
            ("git", "-C", str(repository), "config", "user.name", "Test"),
            check=True,
            env=git_environment,
        )
        subprocess.run(
            (
                "git", "-C", str(repository), "config", "user.email",
                "test@example.invalid",
            ),
            check=True,
            env=git_environment,
        )
        (repository / "payload.txt").write_text("committed\n", encoding="utf-8")
        (repository / "Makefile").write_text(
            "all:\n"
            "\tcp payload.txt modem\n"
            "\tchmod 755 modem\n",
            encoding="utf-8",
        )
        subprocess.run(
            ("git", "-C", str(repository), "add", "Makefile", "payload.txt"),
            check=True,
            env=git_environment,
        )
        subprocess.run(
            ("git", "-C", str(repository), "commit", "-q", "-m", "fixture"),
            check=True,
            env=git_environment,
        )
        revision = subprocess.run(
            ("git", "-C", str(repository), "rev-parse", "HEAD"),
            check=True,
            env=git_environment,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        return repository, revision

    def test_build_is_tied_to_commit_and_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository, revision = self.make_repository(root)
            (repository / "payload.txt").write_text("dirty\n", encoding="utf-8")
            cache = root / "private-cache"
            config = RevisionBuild(
                label="candidate",
                repository=repository,
                revision="HEAD",
                command=("make",),
                executable=Path("modem"),
            )

            first = freeze_revision_build(config, cache)
            second = freeze_revision_build(config, cache)

            self.assertFalse(first.reused)
            self.assertTrue(second.reused)
            self.assertEqual(first.revision, revision)
            self.assertEqual(first.executable.read_text(encoding="utf-8"), "committed\n")
            self.assertEqual(first.executable, second.executable)
            self.assertEqual(first.sha256, second.sha256)
            self.assertFalse((first.directory / "source").exists())
            manifest = json.loads(
                (first.directory / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["revision"], revision)
            self.assertEqual(manifest["sha256"], first.sha256)
            self.assertEqual(manifest["command"], ["make"])
            self.assertEqual(
                stat.S_IMODE(cache.stat().st_mode) & 0o077,
                0,
            )

    def test_changed_recipe_gets_a_distinct_cache_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository, _revision = self.make_repository(root)
            cache = root / "private-cache"
            first = freeze_revision_build(
                RevisionBuild(
                    "candidate", repository, "HEAD", ("make",), Path("modem")
                ),
                cache,
            )
            second = freeze_revision_build(
                RevisionBuild(
                    "candidate", repository, "HEAD", ("make", "all"), Path("modem")
                ),
                cache,
            )
            self.assertNotEqual(first.directory, second.directory)

    def test_rejects_unsafe_recipe_and_public_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository, _revision = self.make_repository(root)
            with self.assertRaises(RevisionBuildError):
                freeze_revision_build(
                    RevisionBuild(
                        "Bad Label", repository, "HEAD", ("make",), Path("modem")
                    ),
                    root / "cache",
                )
            with self.assertRaises(RevisionBuildError):
                freeze_revision_build(
                    RevisionBuild(
                        "candidate", repository, "HEAD", ("make",), Path("../modem")
                    ),
                    root / "cache",
                )
            cache = root / "public-cache"
            cache.mkdir(mode=0o755)
            cache.chmod(0o755)
            with self.assertRaises(RevisionBuildError):
                freeze_revision_build(
                    RevisionBuild(
                        "candidate", repository, "HEAD", ("make",), Path("modem")
                    ),
                    cache,
                )

    def test_failed_build_retains_only_private_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository, revision = self.make_repository(root)
            cache = root / "private-cache"
            with self.assertRaisesRegex(
                RevisionBuildError, "build command failed"
            ) as caught:
                freeze_revision_build(
                    RevisionBuild(
                        "candidate",
                        repository,
                        revision,
                        ("make", "missing-target"),
                        Path("modem"),
                    ),
                    cache,
                )
            failures = list(cache.glob("candidate-*-failed-*"))
            self.assertEqual(len(failures), 1)
            failure = failures[0]
            self.assertIn(str(failure / "build.log"), str(caught.exception))
            self.assertTrue((failure / "build.log").is_file())
            self.assertFalse((failure / "source").exists())
            manifest = json.loads(
                (failure / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["status"], "failed")
            self.assertNotEqual(manifest["returncode"], 0)


if __name__ == "__main__":
    unittest.main()
