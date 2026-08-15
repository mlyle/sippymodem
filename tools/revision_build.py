#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Build and cache an executable from an exact Git revision."""

from __future__ import annotations

from configparser import ConfigParser
from dataclasses import dataclass
from datetime import datetime, timezone
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
from typing import Callable, IO, Mapping, Sequence


MANIFEST_SCHEMA = 1
LABEL_RE = re.compile(r"[a-z0-9](?:[a-z0-9_.-]{0,30}[a-z0-9])?\Z")
GIT_OBJECT_RE = re.compile(r"[0-9a-f]{40,64}\Z")
COMMAND_ITEMS_MAX = 64
COMMAND_ITEM_MAX = 1024


class RevisionBuildError(RuntimeError):
    """A revision could not be exported, built, or verified safely."""


@dataclass(frozen=True)
class RevisionBuild:
    label: str
    repository: Path
    revision: str
    command: tuple[str, ...]
    executable: Path


@dataclass(frozen=True)
class FrozenBinary:
    directory: Path
    executable: Path
    revision: str
    sha256: str
    reused: bool


def _private_directory(path: Path) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        attributes = path.lstat()
    except OSError as error:
        raise RevisionBuildError("frozen-build directory is unavailable") from error
    if (
        not stat.S_ISDIR(attributes.st_mode)
        or stat.S_ISLNK(attributes.st_mode)
        or attributes.st_uid != os.geteuid()
        or stat.S_IMODE(attributes.st_mode) & 0o077
    ):
        raise RevisionBuildError(
            "frozen-build directory must be private and owner-owned"
        )


def _validated(config: RevisionBuild) -> RevisionBuild:
    if LABEL_RE.fullmatch(config.label) is None:
        raise RevisionBuildError("build label has an invalid form")
    repository = config.repository.expanduser().resolve()
    if not repository.is_dir():
        raise RevisionBuildError("build repository is unavailable")
    if (
        not config.revision
        or len(config.revision) > 256
        or any(ord(character) < 0x20 for character in config.revision)
    ):
        raise RevisionBuildError("build revision is invalid")
    if not 1 <= len(config.command) <= COMMAND_ITEMS_MAX or any(
        not isinstance(item, str)
        or not item
        or len(item) > COMMAND_ITEM_MAX
        or "\0" in item
        for item in config.command
    ):
        raise RevisionBuildError("build command is invalid")
    executable = PurePosixPath(config.executable.as_posix())
    if (
        executable.is_absolute()
        or not executable.parts
        or any(part in {"", ".", ".."} for part in executable.parts)
    ):
        raise RevisionBuildError("build executable must be a relative path")
    return RevisionBuild(
        label=config.label,
        repository=repository,
        revision=config.revision,
        command=config.command,
        executable=Path(*executable.parts),
    )


def _git_environment() -> dict[str, str]:
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
    }


def _run_text(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(
            arguments,
            cwd=cwd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=_git_environment(),
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise RevisionBuildError("Git revision inspection failed") from error
    return result.stdout


def _repository_root(repository: Path) -> Path:
    root = Path(
        _run_text(
            ("git", "-C", str(repository), "rev-parse", "--show-toplevel")
        ).strip()
    ).resolve()
    if root != repository:
        raise RevisionBuildError("build repository must name its Git root")
    return root


def _resolve_revision(repository: Path, revision: str) -> str:
    resolved = _run_text(
        (
            "git",
            "-C",
            str(repository),
            "rev-parse",
            "--verify",
            f"{revision}^{{commit}}",
        )
    ).strip()
    if GIT_OBJECT_RE.fullmatch(resolved) is None:
        raise RevisionBuildError("Git returned an invalid commit ID")
    return resolved


def _safe_archive_member(member: tarfile.TarInfo, destination: Path) -> None:
    name = PurePosixPath(member.name)
    if name.is_absolute() or any(part == ".." for part in name.parts):
        raise RevisionBuildError("Git archive contains an unsafe path")
    try:
        tarfile.data_filter(member, str(destination))
    except tarfile.FilterError as error:
        raise RevisionBuildError("Git archive contains an unsafe member") from error


def _extract_archive(repository: Path, revision: str, destination: Path) -> None:
    destination.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        process = subprocess.Popen(
            (
                "git",
                "-C",
                str(repository),
                "archive",
                "--format=tar",
                revision,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=_git_environment(),
        )
    except OSError as error:
        raise RevisionBuildError("Git archive could not start") from error
    assert process.stdout is not None
    assert process.stderr is not None
    try:
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                for member in archive:
                    _safe_archive_member(member, destination)
                    archive.extract(member, destination, filter="data")
        except (OSError, tarfile.TarError) as error:
            process.kill()
            process.wait()
            raise RevisionBuildError("Git archive could not be extracted") from error
        stderr = process.stderr.read()
        if process.wait() != 0:
            raise RevisionBuildError(
                "Git archive failed: "
                + stderr.decode("utf-8", errors="replace")[:256]
            )
    finally:
        process.stdout.close()
        process.stderr.close()


def _submodules(repository: Path, revision: str) -> tuple[tuple[str, str], ...]:
    entry = _run_text(
        ("git", "-C", str(repository), "ls-tree", revision, "--", ".gitmodules")
    ).strip()
    if not entry:
        return ()
    text = _run_text(
        ("git", "-C", str(repository), "show", f"{revision}:.gitmodules")
    )
    parser = ConfigParser()
    try:
        parser.read_string(text)
    except Exception as error:
        raise RevisionBuildError(".gitmodules is malformed") from error
    result: list[tuple[str, str]] = []
    for section in parser.sections():
        if not section.startswith("submodule ") or not parser.has_option(
            section, "path"
        ):
            continue
        path = parser.get(section, "path")
        pure = PurePosixPath(path)
        if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
            raise RevisionBuildError("submodule path is unsafe")
        line = _run_text(
            (
                "git",
                "-C",
                str(repository),
                "ls-tree",
                revision,
                "--",
                path,
            )
        ).strip()
        match = re.fullmatch(
            r"160000 commit (?P<object>[0-9a-f]{40,64})\t.+", line
        )
        if match is None:
            raise RevisionBuildError("submodule commit is unavailable")
        result.append((path, match.group("object")))
    return tuple(result)


def _export_tree(
    repository: Path,
    revision: str,
    destination: Path,
    records: list[dict[str, str]],
) -> None:
    _extract_archive(repository, revision, destination)
    for relative, commit in _submodules(repository, revision):
        submodule_repository = repository / relative
        if not submodule_repository.is_dir():
            raise RevisionBuildError(
                f"submodule checkout is unavailable: {relative}"
            )
        _resolve_revision(submodule_repository.resolve(), commit)
        submodule_destination = destination / relative
        if submodule_destination.exists():
            if not submodule_destination.is_dir():
                raise RevisionBuildError("submodule archive path is not a directory")
            submodule_destination.rmdir()
        records.append({"path": relative, "revision": commit})
        _export_tree(
            submodule_repository.resolve(),
            commit,
            submodule_destination,
            records,
        )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _write_private_json(path: Path, value: Mapping[str, object]) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(dict(value), stream, indent=2, sort_keys=True)
        stream.write("\n")


def _recipe_key(config: RevisionBuild, revision: str) -> str:
    recipe = json.dumps(
        {
            "repository": str(config.repository),
            "revision": revision,
            "command": config.command,
            "executable": config.executable.as_posix(),
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(recipe).hexdigest()[:12]


def _load_cached(
    directory: Path, config: RevisionBuild, revision: str
) -> FrozenBinary:
    try:
        manifest = json.loads(
            (directory / "manifest.json").read_text(encoding="utf-8")
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RevisionBuildError("cached build manifest is unavailable") from error
    executable = directory / "bin" / config.executable.name
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema") != MANIFEST_SCHEMA
        or manifest.get("revision") != revision
        or manifest.get("command") != list(config.command)
        or manifest.get("source_executable") != config.executable.as_posix()
        or not executable.is_file()
        or not os.access(executable, os.X_OK)
    ):
        raise RevisionBuildError("cached build does not match its recipe")
    digest = _sha256(executable)
    if manifest.get("sha256") != digest:
        raise RevisionBuildError("cached build checksum does not match")
    return FrozenBinary(directory, executable, revision, digest, True)


def _retain_failed_build(
    staging: Path,
    source: Path,
    config: RevisionBuild,
    revision: str,
    submodules: Sequence[Mapping[str, str]],
    returncode: int,
) -> Path:
    if source.exists():
        shutil.rmtree(source)
    _write_private_json(
        staging / "manifest.json",
        {
            "schema": MANIFEST_SCHEMA,
            "status": "failed",
            "label": config.label,
            "repository": str(config.repository),
            "revision": revision,
            "command": list(config.command),
            "source_executable": config.executable.as_posix(),
            "submodules": list(submodules),
            "returncode": returncode,
            "built_at": datetime.now(timezone.utc).isoformat(),
        },
    )
    failed = staging.parent / (
        f"{config.label}-{revision[:12]}-failed-{time.time_ns()}"
    )
    staging.rename(failed)
    return failed


def _publish_staging(staging: Path, destination: Path) -> bool:
    try:
        staging.rename(destination)
        return True
    except OSError as error:
        if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
            raise
        shutil.rmtree(staging)
        return False


def freeze_revision_build(
    config: RevisionBuild,
    root: Path,
    progress: Callable[[str], None] | None = None,
) -> FrozenBinary:
    """Return an immutable-by-convention cached binary for one Git commit."""

    def report(stage: str) -> None:
        if progress is not None:
            progress(stage)

    report("validating build configuration")
    config = _validated(config)
    report("inspecting source repository")
    _repository_root(config.repository)
    report("resolving exact revision")
    revision = _resolve_revision(config.repository, config.revision)
    root = root.expanduser().resolve()
    _private_directory(root)
    key = _recipe_key(config, revision)
    destination = root / f"{config.label}-{revision[:12]}-{key}"
    if destination.exists():
        report("verifying cached executable")
        frozen = _load_cached(destination, config, revision)
        report("frozen executable ready")
        return frozen

    staging = Path(tempfile.mkdtemp(prefix=".build-", dir=root))
    os.chmod(staging, 0o700)
    source = staging / "source"
    binary_directory = staging / "bin"
    build_log = staging / "build.log"
    submodules: list[dict[str, str]] = []
    try:
        report("exporting revision and submodules")
        _export_tree(config.repository, revision, source, submodules)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(build_log, flags, 0o600)
        report("compiling revision-frozen executable")
        with os.fdopen(descriptor, "wb") as stream:
            try:
                completed = subprocess.run(
                    config.command,
                    cwd=source,
                    stdin=subprocess.DEVNULL,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    check=False,
                    close_fds=True,
                )
            except OSError as error:
                raise RevisionBuildError("build command could not start") from error
        if completed.returncode != 0:
            failure = _retain_failed_build(
                staging,
                source,
                config,
                revision,
                submodules,
                completed.returncode,
            )
            raise RevisionBuildError(
                f"build command failed; see private log {failure / 'build.log'}"
            )
        built = (source / config.executable).resolve()
        try:
            built.relative_to(source.resolve())
        except ValueError as error:
            raise RevisionBuildError("built executable escaped the source tree") from error
        if not built.is_file() or not os.access(built, os.X_OK):
            raise RevisionBuildError("build did not produce its executable")
        binary_directory.mkdir(mode=0o700)
        frozen_executable = binary_directory / config.executable.name
        shutil.copy2(built, frozen_executable)
        frozen_executable.chmod(
            stat.S_IMODE(frozen_executable.stat().st_mode) | stat.S_IXUSR
        )
        digest = _sha256(frozen_executable)
        shutil.rmtree(source)
        report("finalizing frozen-build cache")
        _write_private_json(
            staging / "manifest.json",
            {
                "schema": MANIFEST_SCHEMA,
                "label": config.label,
                "repository": str(config.repository),
                "revision": revision,
                "command": list(config.command),
                "source_executable": config.executable.as_posix(),
                "sha256": digest,
                "submodules": submodules,
                "built_at": datetime.now(timezone.utc).isoformat(),
            },
        )
        if not _publish_staging(staging, destination):
            report("verifying concurrent cached executable")
            frozen = _load_cached(destination, config, revision)
            report("frozen executable ready")
            return frozen
        frozen = FrozenBinary(
            destination, destination / "bin" / config.executable.name,
            revision, digest, False
        )
        report("frozen executable ready")
        return frozen
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        raise
