from __future__ import annotations

import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

from manyforge_headless_api.deployment_compiler import (
    DeploymentCandidateCompiler,
)
from manyforge_headless_api.native import HeadlessApiError


_DIGEST = "sha256:" + "1" * 64


def _compiler(tmp_path: Path) -> DeploymentCandidateCompiler:
    installed = tmp_path / "installed"
    candidate = tmp_path / "candidate"
    workspace = tmp_path / "workspace"
    executable = tmp_path / "compiler"
    for path in (installed, candidate, workspace):
        path.mkdir()
    executable.write_text("fixture", encoding="utf-8")
    prepared = []
    for family in ("a", "b", "c"):
        root = tmp_path / family
        root.mkdir()
        prepared.append((family, root))
    return DeploymentCandidateCompiler(
        executable=executable,
        workspace_root=workspace,
        installed_package_root=installed,
        candidate_package_root=candidate,
        prepared_roots=tuple(prepared),
        timeout_seconds=3.0,
    )


def _result() -> dict[str, str]:
    return {
        "schema_version": "manyforge.deployment_candidate_compilation.v1",
        "scenario_id": "cell_a",
        "scenario_catalog_revision": _DIGEST,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": _DIGEST,
        "retained_bundle_id": _DIGEST,
        "graph_revision": _DIGEST,
        "program_id": "program_a",
        "program_revision": _DIGEST,
        "program_binding_id": _DIGEST,
    }


def test_compiler_passes_exact_canonical_inputs_without_a_shell(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    compiler = _compiler(tmp_path)
    observed: dict[str, object] = {}

    def run(command, **kwargs):
        observed["command"] = command
        observed["kwargs"] = kwargs
        deployment_path = Path(command[command.index("--candidate-deployment") + 1])
        program_path = Path(command[command.index("--candidate-program") + 1])
        observed["deployment"] = json.loads(deployment_path.read_text())
        observed["program"] = json.loads(program_path.read_text())
        kwargs["stdout"].write(json.dumps(_result()).encode("utf-8"))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    result = compiler.compile(
        scenario_id="cell_a",
        expected_scenario_catalog_revision=_DIGEST,
        deployment={"z": 1, "a": "deployment"},
        program={"z": 2, "a": "program"},
    )

    assert result == _result()
    assert observed["deployment"] == {"a": "deployment", "z": 1}
    assert observed["program"] == {"a": "program", "z": 2}
    assert "shell" not in observed["kwargs"]
    assert observed["kwargs"]["stdin"] is subprocess.DEVNULL
    assert not list((tmp_path / "candidate").glob("*-candidate-*.json"))


def test_compiler_is_capacity_one_and_reports_a_stable_conflict(
    tmp_path: Path,
) -> None:
    compiler = _compiler(tmp_path)
    assert compiler._lock.acquire(blocking=False)  # exact bounded worker seam
    try:
        with pytest.raises(HeadlessApiError) as raised:
            compiler.compile(
                scenario_id="cell_a",
                expected_scenario_catalog_revision=_DIGEST,
                deployment={"api_version": "manyforge.deployment.v1"},
                program={"api_version": "manyforge.program.v2"},
            )
    finally:
        compiler._lock.release()
    assert raised.value.code == "deployment_candidate_compiler_busy"


def test_compiler_rejects_timeout_and_invalid_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    compiler = _compiler(tmp_path)

    def timeout(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["compiler"], 3.0)

    monkeypatch.setattr(subprocess, "run", timeout)
    with pytest.raises(HeadlessApiError) as raised:
        compiler.compile(
            scenario_id="cell_a",
            expected_scenario_catalog_revision=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            program={"api_version": "manyforge.program.v2"},
        )
    assert raised.value.code == "deployment_candidate_compiler_timeout"

    def malformed(_command, **kwargs):
        kwargs["stdout"].write(b"{}")
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", malformed)
    with pytest.raises(HeadlessApiError) as raised:
        compiler.compile(
            scenario_id="cell_a",
            expected_scenario_catalog_revision=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            program={"api_version": "manyforge.program.v2"},
        )
    assert raised.value.code == "deployment_candidate_compiler_response_invalid"
