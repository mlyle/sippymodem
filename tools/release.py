#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build portable archives and optional Debian packages from an exact revision."""

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
PLATFORMS = {
    ("Linux", "x86_64"): ("linux-x86_64", "amd64"),
    ("Linux", "aarch64"): ("linux-aarch64", "arm64"),
    ("Darwin", "arm64"): ("macos-arm64", None),
}


def archive_tree(source, archive, name, epoch):
    def set_metadata(member):
        member.uid = member.gid = 0
        member.uname = member.gname = "root"
        member.mtime = epoch
        if member.isdir():
            member.mode = 0o755
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


def build_deb(installed, stage, artifacts, version, arch, env):
    stage.mkdir(parents=True)
    package = stage / "usr"
    installed.rename(package)
    (package / "README.md").unlink()
    (package / "SOURCE.json").rename(package / "share/doc/sippymodem/SOURCE.json")

    examples = package / "share/sippymodem/examples/systemd"
    unit = examples / "sippymodem@.service"
    unit.write_text(unit.read_text().replace("/usr/local/bin/slmodemd", "/usr/bin/slmodemd"))
    unit_dir = package / "lib/systemd/system"
    unit_dir.mkdir(parents=True)
    shutil.copyfile(unit, unit_dir / unit.name)

    metadata = stage / "DEBIAN"
    metadata.mkdir()
    installed_kib = sum((path.stat().st_size + 1023) // 1024
                        for path in package.rglob("*") if path.is_file())
    control = ("Package: sippymodem\n"
               f"Version: {version}\n"
               f"Architecture: {arch}\n"
               f"Installed-Size: {installed_kib}\n"
               "Section: comm\n"
               "Priority: optional\n"
               "Maintainer: Michael Lyle <mlyle@lyle.org>\n"
               "Description: Dial-up data modem over SIP and RTP\n"
               " Includes the slmodemd executable, native SDK and a systemd template.\n")
    # dpkg-shlibdeps requires a source control file even when emitting deps to stdout.
    debian = stage.parent
    (debian / "control").write_text("Source: sippymodem\n\n" + control)
    dependencies = subprocess.check_output(
        ["dpkg-shlibdeps", "-O", "-e", str(package / "bin/slmodemd"),
         "-e", str(package / "lib/libsippymodem.so")],
        cwd=debian.parent, env=env, text=True).strip()
    prefix = "shlibs:Depends="
    if not dependencies.startswith(prefix) or "\n" in dependencies:
        raise ValueError("unexpected dpkg-shlibdeps output")
    if dependencies[len(prefix):]:
        control += "Depends: " + dependencies[len(prefix):] + "\n"
    (metadata / "control").write_text(control)
    (metadata / "triggers").write_text("activate-noawait ldconfig\n")
    archive = artifacts / f"sippymodem_{version}_{arch}.deb"
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", stage, archive],
                   env=env, check=True)
    return archive


def check_release(work, binary, deb):
    portable = work / "portable install"
    portable.mkdir()
    with tarfile.open(binary) as archive:
        archive.extractall(portable, filter="data")
    subprocess.run([sys.executable, ROOT / "tools/release_smoke.py",
                    portable / binary.name.removesuffix(".tar.gz")], check=True)
    if deb is not None:
        target = work / "debian install"
        subprocess.run(["dpkg-deb", "--extract", deb, target], check=True)
        subprocess.run([sys.executable, ROOT / "tools/release_smoke.py", target / "usr"],
                       check=True)


def build_release(revision, output, jobs, *, deb=False, check=False):
    target = PLATFORMS.get((platform.system(), platform.machine()))
    if target is None or (deb and target[1] is None):
        raise ValueError("build natively on Linux x86-64/ARM64 or macOS ARM64 (tarball only)")
    target_name, deb_arch = target
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
        binary_name = f"{prefix}-{target_name}"
        source_name = f"{binary_name}-source"
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
        make = "gmake" if platform.system() == "Darwin" else "make"
        subprocess.run([make, f"-j{jobs}", "install", "PJSIP_PROVIDER=submodule",
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
        archives = [binary_archive, source_archive]
        deb_archive = None
        if deb:
            deb_archive = build_deb(
                package, work / "debian/sippymodem", artifacts, version, deb_arch, env)
            archives.append(deb_archive)
        if check:
            check_release(work, binary_archive, deb_archive)
        with (artifacts / "SHA256SUMS").open("w", encoding="ascii") as checksums:
            for archive in sorted(archives):
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
    parser.add_argument("--deb", action="store_true", help="also build a Debian package on Linux")
    parser.add_argument("--check", action="store_true", help="smoke-test the extracted binary packages")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    if not hasattr(tarfile, "data_filter"):
        parser.error("this Python lacks safe archive extraction support (use Python 3.12+)")
    try:
        directory = build_release(args.revision, args.output.resolve(), args.jobs,
                                  deb=args.deb, check=args.check)
    except (OSError, RevisionBuildError, ValueError, subprocess.CalledProcessError) as error:
        print(f"release: {error}", file=sys.stderr)
        return 1
    print(directory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
