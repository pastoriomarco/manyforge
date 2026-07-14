#!/usr/bin/env python3
"""Fail closed on a mixed RoboPlan/Pinocchio/Coal Python or C++ stack."""

from __future__ import annotations

import importlib
import importlib.metadata
import site
import subprocess
import sys
from pathlib import Path


EXPECTED_DISTRIBUTIONS = {
    "pin": "4.1.0",
    "libpinocchio": "4.1.0",
    "coal": "3.0.3",
    "libcoal": "3.0.3",
    "roboplan-core": "0.5.1",
    "roboplan-example-models": "0.5.1",
    "roboplan-simple-ik": "0.5.1",
    "roboplan-oink": "0.5.1",
    "roboplan-rrt": "0.5.1",
    "roboplan-toppra": "0.5.1",
    # Upstream still publishes this subpackage as version "1" in the 0.5.1
    # source tree. The exact source pin remains its generation boundary.
    "roboplan-cartesian-planning": "1",
}
REQUIRED_MODULES = (
    "pinocchio",
    "coal",
    "roboplan.core",
    "roboplan.example_models",
    "roboplan.simple_ik",
    "roboplan.optimal_ik",
    "roboplan.rrt",
    "roboplan.toppra",
    "roboplan.cartesian_planning",
)
OWNED_LIBRARY_PREFIXES = (
    "libpinocchio",
    "libcoal",
    "libhpp-fcl",
    "libeigenpy",
)


def _cmeel_prefix() -> Path:
    candidates = [Path(value) / "cmeel.prefix" for value in site.getsitepackages()]
    existing = [path.resolve() for path in candidates if path.is_dir()]
    if len(existing) != 1:
        raise RuntimeError(
            "expected exactly one active cmeel.prefix, found "
            + ", ".join(str(path) for path in existing)
        )
    return existing[0]


def _native_extensions(modules: list[object]) -> list[Path]:
    result: set[Path] = set()
    for module in modules:
        raw_path = getattr(module, "__file__", None)
        if not raw_path:
            continue
        path = Path(raw_path).resolve()
        if path.suffix == ".so":
            result.add(path)
        elif path.parent.is_dir():
            result.update(candidate.resolve() for candidate in path.parent.glob("*.so"))
    if not result:
        raise RuntimeError("no native RoboPlan/Pinocchio/Coal extensions were found")
    return sorted(result)


def _check_linkage(extension: Path, cmeel_prefix: Path) -> None:
    completed = subprocess.run(
        ["ldd", str(extension)],
        check=True,
        capture_output=True,
        text=True,
    )
    output = completed.stdout
    if "not found" in output:
        raise RuntimeError(f"{extension}: unresolved shared library:\n{output}")
    cmeel_lib = (cmeel_prefix / "lib").resolve()
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped.startswith(OWNED_LIBRARY_PREFIXES):
            continue
        fields = stripped.split("=>", 1)
        if len(fields) != 2:
            raise RuntimeError(f"{extension}: unresolvable owned library line: {line}")
        resolved = Path(fields[1].strip().split()[0]).resolve()
        if not resolved.is_relative_to(cmeel_lib):
            raise RuntimeError(
                f"{extension}: {fields[0].strip()} resolved outside {cmeel_lib}: "
                f"{resolved}"
            )


def main() -> int:
    failures: list[str] = []
    for distribution, expected in EXPECTED_DISTRIBUTIONS.items():
        try:
            observed = importlib.metadata.version(distribution)
        except importlib.metadata.PackageNotFoundError:
            failures.append(f"{distribution}: distribution is missing")
            continue
        if observed != expected:
            failures.append(f"{distribution}: expected {expected}, observed {observed}")

    modules: list[object] = []
    for name in REQUIRED_MODULES:
        try:
            modules.append(importlib.import_module(name))
        except Exception as exc:  # noqa: BLE001 - preserve the failing import boundary
            failures.append(f"{name}: import failed: {exc}")

    if not failures:
        try:
            cmeel_prefix = _cmeel_prefix()
            for extension in _native_extensions(modules):
                _check_linkage(extension, cmeel_prefix)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            failures.append(str(exc))

    if failures:
        print("RoboPlan dependency stack verification failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print(
        "RoboPlan dependency stack verified: RoboPlan 0.5.1, "
        "Pinocchio 4.1.0, Coal 3.0.3"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
