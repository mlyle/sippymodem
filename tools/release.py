#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build Linux binary and source tarballs from an exact Git revision."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

from pkg_config import read_version
from revision_build import RevisionBuildError, export_revision

ROOT = Path(__file__).resolve().parents[1]
PJSIP_PATH = "third_party/pjproject"


def archive_tree(source, archive, name, epoch):
    def set_metadata(member):
        member.uid = member.gid = 0
        member.uname = member.gname = "root"
        member.mtime = epoch
        return member

    with archive.open("wb") as out, gzip.GzipFile(
            fileobj=out, filename="", mode="wb", mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as bundle:
            bundle.add(source, name, filter=set_metadata)


def copy_notices(source, package):
    notices = package / "share/doc/sippymodem/pjproject"
    pjsip = source / PJSIP_PATH
    for path in sorted(pjsip.rglob("*")):
        if path.is_file() and path.name.startswith(("COPYING", "LICENSE")):
            target = notices / path.relative_to(pjsip)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    if not (notices / "COPYING").is_file():
        raise ValueError("the exported PJSIP license is missing")


def build_release(revision, output, jobs):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-", dir=output) as directory:
        work = Path(directory)
        source = work / "source"
        commit, submodules = export_revision(ROOT, revision, source)
        if not any(module["path"] == PJSIP_PATH for module in submodules):
            raise ValueError("the selected revision must pin PJSIP")
        version = read_version(source)
        epoch = int(subprocess.check_output(
            ["git", "show", "-s", "--format=%ct", commit], cwd=ROOT, text=True))
        prefix = f"sippymodem-{version}-{commit[:12]}"
        binary_name = f"{prefix}-linux-{platform.machine()}"
        source_name = f"{prefix}-source"
        destination = output / binary_name
        if destination.exists():
            raise FileExistsError(f"release directory already exists: {destination}")
        artifacts = work / "artifacts"
        artifacts.mkdir()
        source_archive = artifacts / (source_name + ".tar.gz")
        archive_tree(source, source_archive, source_name, epoch)

        stage = work / "stage"
        env = {key: value for key, value in os.environ.items()
               if key not in {"MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS", "MAKELEVEL"}}
        env["SOURCE_DATE_EPOCH"] = str(epoch)
        subprocess.run(["make", f"-j{jobs}", "install", "PJSIP_PROVIDER=submodule",
                        "PREFIX=/usr/local", "RELOCATABLE_SDK=1", f"DESTDIR={stage}"],
                       cwd=source, env=env, check=True)
        package = stage / "usr/local"
        shutil.copyfile(source / "docs/quickstart.md", package / "README.md")
        copy_notices(source, package)
        binary_archive = artifacts / (binary_name + ".tar.gz")
        with source_archive.open("rb") as file:
            source_digest = hashlib.file_digest(file, "sha256").hexdigest()
        manifest = dict(version=version, revision=commit, submodules=submodules,
                        source_archive=source_archive.name, source_sha256=source_digest)
        (package / "SOURCE.json").write_text(json.dumps(manifest, indent=2) + "\n")
        archive_tree(package, binary_archive, binary_name, epoch)
        with (artifacts / "SHA256SUMS").open("w", encoding="ascii") as checksums:
            for archive in (binary_archive, source_archive):
                with archive.open("rb") as file:
                    digest = hashlib.file_digest(file, "sha256").hexdigest()
                checksums.write(f"{digest}  {archive.name}\n")
        artifacts.rename(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", default="HEAD", help="Git commit or tag")
    parser.add_argument("--output", type=Path, default=ROOT / ".build/release")
    parser.add_argument("-j", "--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    if platform.system() != "Linux":
        parser.error("run this command on the target Linux architecture")
    try:
        directory = build_release(args.revision, args.output.resolve(), args.jobs)
    except (OSError, RevisionBuildError, ValueError, subprocess.CalledProcessError) as error:
        print(f"release: {error}", file=sys.stderr)
        return 1
    print(directory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
