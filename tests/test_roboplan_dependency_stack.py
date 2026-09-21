"""Tests for the fail-closed RoboPlan native dependency-stack probe."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SCRIPT = _REPO_ROOT / "scripts/ci/check-roboplan-dependency-stack.py"
_SPEC = importlib.util.spec_from_file_location(
    "check_roboplan_dependency_stack",
    _SCRIPT,
)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)


def test_required_module_inventory_uses_upstream_package_namespaces() -> None:
    assert _MODULE.REQUIRED_MODULES == (
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


def test_linkage_accepts_owned_libraries_from_active_cmeel_prefix(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    prefix = tmp_path / "site-packages/cmeel.prefix"
    library = prefix / "lib/libpinocchio_default.so.4.1"
    library.parent.mkdir(parents=True)
    library.touch()
    monkeypatch.setattr(
        _MODULE.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout=(
                f"libpinocchio_default.so.4.1 => {library} "
                "(0x0000000000000000)\n"
            )
        ),
    )

    _MODULE._check_linkage(tmp_path / "extension.so", prefix)  # noqa: SLF001


@pytest.mark.parametrize("library_name", ["libcoal.so.3.0", "libeigenpy.so.3.13"])
def test_linkage_rejects_owned_library_outside_active_cmeel_prefix(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    library_name: str,
) -> None:
    prefix = tmp_path / "site-packages/cmeel.prefix"
    prefix.mkdir(parents=True)
    system_library = tmp_path / "usr/lib" / library_name
    system_library.parent.mkdir(parents=True)
    system_library.touch()
    monkeypatch.setattr(
        _MODULE.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout=f"{library_name} => {system_library} (0x0)\n"
        ),
    )

    with pytest.raises(RuntimeError, match="resolved outside"):
        _MODULE._check_linkage(  # noqa: SLF001
            tmp_path / "extension.so",
            prefix,
        )


def test_linkage_rejects_unresolved_library(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    prefix = tmp_path / "site-packages/cmeel.prefix"
    prefix.mkdir(parents=True)
    monkeypatch.setattr(
        _MODULE.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout="libpinocchio_default.so.4.1 => not found\n"
        ),
    )

    with pytest.raises(RuntimeError, match="unresolved shared library"):
        _MODULE._check_linkage(  # noqa: SLF001
            tmp_path / "extension.so",
            prefix,
        )


def test_linkage_rejects_roboplan_library_outside_selected_distribution(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    cmeel_prefix = tmp_path / "site-packages/cmeel.prefix"
    cmeel_prefix.mkdir(parents=True)
    selected = tmp_path / "site-packages/lib"
    selected.mkdir(parents=True)
    competing = tmp_path / "usr/local/lib/libroboplan.so"
    competing.parent.mkdir(parents=True)
    competing.touch()
    monkeypatch.setattr(
        _MODULE.subprocess,
        "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout=f"libroboplan.so => {competing} (0x0)\n"
        ),
    )

    with pytest.raises(RuntimeError, match="resolved outside"):
        _MODULE._check_linkage(  # noqa: SLF001
            tmp_path / "extension.so",
            cmeel_prefix,
            selected,
        )


def test_cmeel_prefix_requires_exactly_one_active_generation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    first = tmp_path / "first"
    second = tmp_path / "second"
    (first / "cmeel.prefix").mkdir(parents=True)
    (second / "cmeel.prefix").mkdir(parents=True)
    monkeypatch.setattr(
        _MODULE.site,
        "getsitepackages",
        lambda: [str(first), str(second)],
    )

    with pytest.raises(RuntimeError, match="exactly one active cmeel.prefix"):
        _MODULE._cmeel_prefix()  # noqa: SLF001
