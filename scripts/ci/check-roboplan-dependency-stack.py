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
    "roboplan": "0.7.0",
    "roboplan-common": "0.7.0",
    "roboplan-core": "0.7.0",
    "roboplan-example-models": "0.7.0",
    "roboplan-simple-ik": "0.7.0",
    "roboplan-oink": "0.7.0",
    "roboplan-rrt": "0.7.0",
    "roboplan-toppra": "0.7.0",
    "roboplan-cartesian-planning": "0.7.0",
}
REQUIRED_MODULES = (
    "pinocchio",
    "coal",
    "roboplan_common",
    "roboplan.core",
    "roboplan.filters",
    "roboplan.example_models",
    "roboplan.simple_ik",
    "roboplan.optimal_ik",
    "roboplan.rrt",
    "roboplan.toppra",
    "roboplan.cartesian_planning",
)
ROBOPLAN_DISTRIBUTIONS = tuple(
    name
    for name in EXPECTED_DISTRIBUTIONS
    if name == "roboplan" or name.startswith("roboplan-")
)
ROBOPLAN_NATIVE_MODULES = (
    "roboplan.core._core_ext",
    "roboplan.filters._filters_ext",
    "roboplan.example_models._example_models_ext",
    "roboplan.simple_ik._simple_ik_ext",
    "roboplan.optimal_ik._optimal_ik_ext",
    "roboplan.rrt._rrt_ext",
    "roboplan.toppra._toppra_ext",
    "roboplan.cartesian_planning._cartesian_ext",
)
ROBOPLAN_LIBRARIES = (
    "libroboplan.so",
    "libroboplan_example_models.so",
    "libroboplan_simple_ik.so",
    "libroboplan_oink.so",
    "libroboplan_rrt.so",
    "libroboplan_toppra.so",
    "libroboplan_cartesian_planning.so",
)
OWNED_LIBRARY_PREFIXES = (
    "libpinocchio",
    "libcoal",
    "libhpp-fcl",
    "libeigenpy",
)
ROBOPLAN_LIBRARY_PREFIXES = ("libroboplan",)
EXTERNAL_CLOSURE_LIBRARY_PREFIXES = ("libtoppra", "libproxsuite")


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


def _roboplan_installation(
    native_modules: list[object],
) -> tuple[Path, list[Path]]:
    roots: set[Path] = set()
    recorded: set[Path] = set()
    for name in ROBOPLAN_DISTRIBUTIONS:
        distribution = importlib.metadata.distribution(name)
        roots.add(Path(distribution.locate_file("")).resolve())
        files = distribution.files
        if files is None:
            raise RuntimeError(f"{name}: installed-file inventory is unavailable")
        recorded.update(Path(distribution.locate_file(path)).resolve() for path in files)
    if len(roots) != 1:
        raise RuntimeError(
            "expected one RoboPlan distribution prefix, found "
            + ", ".join(str(path) for path in sorted(roots))
        )

    roots.pop()
    selected_libraries: list[Path] = []
    for name in ROBOPLAN_LIBRARIES:
        matches = sorted(path for path in recorded if path.name == name and path.is_file())
        if len(matches) != 1:
            raise RuntimeError(
                f"expected one recorded RoboPlan library named {name}, found "
                + ", ".join(str(path) for path in matches)
            )
        selected_libraries.append(matches[0])
    library_roots = {path.parent.resolve() for path in selected_libraries}
    if len(library_roots) != 1:
        raise RuntimeError(
            "expected one recorded RoboPlan library root, found "
            + ", ".join(str(path) for path in sorted(library_roots))
        )
    library_root = library_roots.pop()

    for module in native_modules:
        raw_path = getattr(module, "__file__", None)
        if not raw_path:
            raise RuntimeError(f"{module.__name__}: native module path is unavailable")
        path = Path(raw_path).resolve()
        if path.suffix != ".so" or path not in recorded:
            raise RuntimeError(f"unrecorded RoboPlan native module selected: {path}")
        unrecorded_siblings = sorted(
            sibling.resolve()
            for sibling in path.parent.glob("*.so")
            if sibling.resolve() not in recorded
        )
        if unrecorded_siblings:
            raise RuntimeError(
                "unrecorded competing RoboPlan native module(s): "
                + ", ".join(str(item) for item in unrecorded_siblings)
            )

    system_duplicates = sorted(Path("/usr/local/lib").glob("libroboplan*.so*"))
    if system_duplicates and library_root != Path("/usr/local/lib"):
        raise RuntimeError(
            "competing RoboPlan libraries exist outside the selected distribution: "
            + ", ".join(str(path) for path in system_duplicates)
        )
    return library_root, [path.resolve() for path in selected_libraries]


def _check_linkage(
    extension: Path,
    cmeel_prefix: Path,
    roboplan_library_root: Path | None = None,
    external_library_root: Path = Path("/usr/local/lib"),
) -> None:
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
    for line in output.splitlines():
        stripped = line.strip()
        expected_root: Path | None = None
        if stripped.startswith(ROBOPLAN_LIBRARY_PREFIXES):
            if roboplan_library_root is None:
                raise RuntimeError(
                    f"{extension}: RoboPlan library root was not supplied"
                )
            expected_root = roboplan_library_root
        elif stripped.startswith(EXTERNAL_CLOSURE_LIBRARY_PREFIXES):
            expected_root = external_library_root.resolve()
        if expected_root is None:
            continue
        fields = stripped.split("=>", 1)
        if len(fields) != 2:
            raise RuntimeError(f"{extension}: unresolvable selected library line: {line}")
        resolved = Path(fields[1].strip().split()[0]).resolve()
        if not resolved.is_relative_to(expected_root):
            raise RuntimeError(
                f"{extension}: {fields[0].strip()} resolved outside "
                f"{expected_root}: {resolved}"
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

    roboplan_native_modules: list[object] = []
    for name in ROBOPLAN_NATIVE_MODULES:
        try:
            roboplan_native_modules.append(importlib.import_module(name))
        except Exception as exc:  # noqa: BLE001 - preserve the failing import boundary
            failures.append(f"{name}: native import failed: {exc}")

    if not failures:
        try:
            cmeel_prefix = _cmeel_prefix()
            roboplan_library_root, roboplan_libraries = _roboplan_installation(
                roboplan_native_modules
            )
            for extension in _native_extensions(modules) + roboplan_libraries:
                _check_linkage(
                    extension,
                    cmeel_prefix,
                    roboplan_library_root,
                )
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            failures.append(str(exc))

    if failures:
        print("RoboPlan dependency stack verification failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print(
        "RoboPlan dependency stack verified: RoboPlan 0.7.0, "
        "Pinocchio 4.1.0, Coal 3.0.3, one recorded planner ABI"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
