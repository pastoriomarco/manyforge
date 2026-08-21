"""Focused descriptor/freshness checks; executed by the parent container lane."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "manyforge_deployment/tools/generate_program_binding_transport.py"
DESCRIPTOR = (
    ROOT / "manyforge_deployment/resources/program_binding_transport.v1.json"
)
FIXTURE = (
    ROOT
    / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)


def _run(
    path: Path,
    *,
    check: bool = True,
    output_root: Path = ROOT,
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(TOOL),
        "--descriptor",
        str(path),
        "--output-root",
        str(output_root),
    ]
    if check:
        command.append("--check")
    return subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def test_checked_in_program_binding_transport_outputs_are_fresh() -> None:
    assert _run(DESCRIPTOR).returncode == 0


def test_checked_in_native_binding_fixture_is_exact() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--check",
            "--fixture-source",
            str(FIXTURE),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize(
    "mutation",
    [
        lambda value: value["limits"].update({"role_rows": 4097}),
        lambda value: value["records"]["component"].__setitem__(
            0, "renamed_component_role_id"
        ),
        lambda value: value["records"]["target"].remove("command_target_id"),
        lambda value: value["records"]["component"].append("component_role_id"),
        lambda value: value["scalars"]["digest"].update(
            {"pattern": "^sha256:[0-9A-F]{64}$"}
        ),
        lambda value: value["scalars"]["identity"].update(
            {"maximum_utf8_bytes": 129}
        ),
        lambda value: value["scalars"]["text"].update(
            {"maximum_utf8_bytes": 4097}
        ),
        lambda value: value["invariants"].remove("canonical_sorted_unique"),
        lambda value: value["catalog_record_kinds"].append("unknown_kind"),
    ],
)
def test_program_binding_transport_descriptor_rejects_drift(
    tmp_path: Path,
    mutation,
) -> None:
    value = json.loads(DESCRIPTOR.read_text(encoding="utf-8"))
    mutation(value)
    candidate = tmp_path / "descriptor.json"
    candidate.write_text(json.dumps(value), encoding="utf-8")
    result = _run(candidate)
    assert result.returncode == 1
    assert result.stderr


def test_generation_reconstructs_outputs_and_check_detects_drift(
    tmp_path: Path,
) -> None:
    assert _run(DESCRIPTOR, check=False, output_root=tmp_path).returncode == 0
    generated = (
        tmp_path / "manyforge_headless_api/program_binding_transport.py"
    )
    assert generated.is_file()
    assert _run(DESCRIPTOR, output_root=tmp_path).returncode == 0

    generated.write_text("corrupt generated output\n", encoding="utf-8")
    assert _run(DESCRIPTOR, output_root=tmp_path).returncode == 1
    assert _run(DESCRIPTOR, check=False, output_root=tmp_path).returncode == 0
    assert _run(DESCRIPTOR, output_root=tmp_path).returncode == 0
