#!/usr/bin/env python3
"""Freshly generate and review one target-specific Release golden."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


_ROOT = Path(__file__).resolve().parents[2]
_DEPLOYMENT = _ROOT / "manyforge_deployment"
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")


class ReleaseGoldenError(RuntimeError):
    """The generated candidate or selected golden is invalid."""


@dataclass(frozen=True)
class ReleaseCandidate:
    canonical_line: bytes
    build_target: str
    target_slug: str
    admission_digest: str
    compiler_build_digest: str


def release_target_slug(build_target: str) -> str:
    aliases = {
        "Linux/AMD64": "linux-amd64",
        "Linux/amd64": "linux-amd64",
        "Linux/x86_64": "linux-amd64",
        "Linux/AArch64": "linux-arm64",
        "Linux/aarch64": "linux-arm64",
        "Linux/arm64": "linux-arm64",
    }
    if build_target not in aliases:
        raise ReleaseGoldenError(
            "Release goldens support only official Linux AMD64/ARM64 targets, "
            f"not {build_target!r}"
        )
    return aliases[build_target]


def _object(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseGoldenError(f"cannot read {label}: {error}") from error
    if not isinstance(value, dict):
        raise ReleaseGoldenError(f"{label} is not a JSON object")
    return value


def _text(value: dict[str, object], field: str, label: str) -> str:
    result = value.get(field)
    if not isinstance(result, str) or not result:
        raise ReleaseGoldenError(f"{label}.{field} is not text")
    return result


def load_release_candidate(
    package_root: Path, generator_stdout: bytes
) -> ReleaseCandidate:
    admission_path = package_root / "canonical-empty-admission.json"
    try:
        admission_bytes = admission_path.read_bytes()
    except OSError as error:
        raise ReleaseGoldenError("generator omitted its admission record") from error
    if generator_stdout != admission_bytes + b"\n":
        raise ReleaseGoldenError("generator stdout differs from its admission record")

    admission = _object(admission_path, "admission")
    admission_digest = _text(admission, "admission_digest", "admission")
    compiler_digest = _text(admission, "compiler_build_digest", "admission")
    if not _DIGEST.fullmatch(admission_digest) or not _DIGEST.fullmatch(
        compiler_digest
    ):
        raise ReleaseGoldenError("admission contains a malformed digest")

    build_path = (
        package_root
        / "build-manifests"
        / "compiler_build"
        / f"{compiler_digest[7:]}.json"
    )
    build = _object(build_path, "compiler-build manifest")
    if _text(build, "compiler_build_digest", "compiler-build manifest") != (
        compiler_digest
    ):
        raise ReleaseGoldenError("admission and compiler-build manifest diverge")
    profile = _text(build, "build_profile", "compiler-build manifest")
    if profile != "Release":
        raise ReleaseGoldenError(f"build profile is {profile!r}, not 'Release'")
    target = _text(build, "target", "compiler-build manifest")
    return ReleaseCandidate(
        admission_bytes + b"\n",
        target,
        release_target_slug(target),
        admission_digest,
        compiler_digest,
    )


def golden_path(candidate: ReleaseCandidate) -> Path:
    return _DEPLOYMENT / "test" / "golden" / (
        f"canonical_empty_admission.{candidate.target_slug}.release.json"
    )


def review_candidate(
    candidate: ReleaseCandidate, destination: Path, *, update: bool
) -> None:
    if not update:
        try:
            checked = destination.read_bytes()
        except OSError as error:
            raise ReleaseGoldenError(f"target golden is absent: {destination}") from error
        if checked != candidate.canonical_line:
            raise ReleaseGoldenError(
                f"target golden differs: {destination}; review the change and "
                "use --update only for an intentionally frozen candidate"
            )
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(candidate.canonical_line)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def _generate() -> ReleaseCandidate:
    with tempfile.TemporaryDirectory(
        prefix="manyforge-canonical-empty-release-"
    ) as root_text:
        root = Path(root_text)
        build = root / "build"
        package = root / "package"
        package.mkdir()
        subprocess.run(
            [
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(_DEPLOYMENT),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
                f"-DMANYFORGE_CORE_INCLUDE_DIR={_ROOT / 'manyforge_core/include'}",
            ],
            check=True,
        )
        subprocess.run(
            [
                "cmake",
                "--build",
                str(build),
                "--target",
                "manyforge_generate_canonical_empty_package",
                "--parallel",
            ],
            check=True,
        )
        generated = subprocess.run(
            [
                str(build / "manyforge_generate_canonical_empty_package"),
                "--output-root",
                str(package),
            ],
            check=True,
            capture_output=True,
        )
        if generated.stderr:
            raise ReleaseGoldenError("generator emitted unexpected stderr")
        return load_release_candidate(package, generated.stdout)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--update", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        if not Path("/.dockerenv").exists():
            raise ReleaseGoldenError(
                "run this through `docker compose run --rm dev python3 "
                "scripts/qualification/review_canonical_empty_release_golden.py`"
            )
        candidate = _generate()
        destination = golden_path(candidate)
        review_candidate(candidate, destination, update=arguments.update)
    except (ReleaseGoldenError, subprocess.CalledProcessError) as error:
        print(f"canonical-empty Release golden failed: {error}", file=sys.stderr)
        return 1

    action = "updated for review" if arguments.update else "matches"
    print(f"target: {candidate.build_target} ({candidate.target_slug})")
    print(f"compiler build: {candidate.compiler_build_digest}")
    print(f"admission: {candidate.admission_digest}")
    print(f"golden {action}: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
