from __future__ import annotations

import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

from manyforge_headless_api.activation_preparer import ActivationPreparer
from manyforge_headless_api.native import (
    HeadlessApiConfig,
    HeadlessApiError,
    MAX_ACTIVATION_PREPARATION_REQUEST_BYTES,
)


_DIGEST = "sha256:" + "1" * 64
_CLOSURE = {
    "schema_version": "manyforge.activation_model_source_closure.v1",
    "components": [],
}
_PREPARED_CLOSURE = {
    "schema_version": "manyforge.activation_prepared_model_closure.v1",
    "components": [],
}


@pytest.fixture(autouse=True)
def _stub_model_preparation(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        "manyforge_headless_api.activation_preparer._model_preparation_profiles",
        lambda _root: {},
    )
    monkeypatch.setattr(
        "manyforge_headless_api.activation_preparer._prepare_model_closure",
        lambda **_: _PREPARED_CLOSURE,
    )


def test_headless_config_accepts_source_owned_activation_preparer(
    tmp_path: Path,
) -> None:
    root = tmp_path.resolve()

    config = HeadlessApiConfig(
        projection_root=root / "projection",
        install_root=root / "install",
        artifact_root=root / "artifacts",
        node_catalog_path=root / "node-catalog.yaml",
        node_environment_manifest_path=root / "node-environment.yaml",
        alarm_catalog_path=root / "alarm-catalog.yaml",
        historian_database_path=root / "history.sqlite3",
        manager_run_root=root / "manager-run",
        execution_run_root=root / "execution-run",
        cell_id="cell_a",
        activation_preparer_path=root / "preparer",
        activation_preparer_capability_root=root / "capabilities",
        activation_preparer_activation_root=root / "artifacts",
    )

    assert config.activation_preparer_capability_root == root / "capabilities"


def _preparer(tmp_path: Path) -> ActivationPreparer:
    capabilities = tmp_path / "capabilities"
    executable = tmp_path / "preparer"
    capabilities.mkdir()
    executable.write_text("fixture", encoding="utf-8")
    return ActivationPreparer(
        executable=executable,
        capability_root=capabilities,
        activation_root=tmp_path / "artifacts",
        timeout_seconds=3.0,
    )


def _result() -> dict[str, object]:
    identity: dict[str, object] = {
        "schema_version": "manyforge.prepared_activation_input.v3",
        "source_document_digest": _DIGEST,
        "scenario_id": "scenario_a",
        "captured_input_digest": _DIGEST,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": _DIGEST,
        "retained_bundle_id": _DIGEST,
        "graph_revision": _DIGEST,
        "program_id": "program_a",
        "program_revision": _DIGEST,
        "program_outcome": {"kind": "bound", "program_binding_id": _DIGEST},
        "node_environment_digest": _DIGEST,
        "assistant_mode": "forbidden",
        "typed_parameter_overrides": {},
        "activation_payload": {"raw_artifacts": [{"bytes": "YQ=="}]},
    }
    import hashlib
    import rfc8785

    return {
        **identity,
        "prepared_activation_id": "sha256:"
        + hashlib.sha256(rfc8785.dumps(identity)).hexdigest(),
    }


def test_preparer_passes_one_exact_canonical_input_without_a_shell(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    observed: dict[str, object] = {}

    def run(command, **kwargs):
        observed["command"] = command
        observed["kwargs"] = kwargs
        input_path = Path(command[command.index("--input") + 1])
        observed["input"] = json.loads(input_path.read_text())
        kwargs["stdout"].write(json.dumps(_result()).encode("utf-8"))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    result = preparer.prepare(
        source_document_digest=_DIGEST,
        scenario_id="scenario_a",
        captured_input_digest=_DIGEST,
        deployment={"z": 1, "a": "deployment"},
        model_source_closure=_CLOSURE,
        program={"z": 2, "a": "program"},
        program_role_assignment={"component_roles": [], "target_roles": []},
        typed_parameter_overrides={},
    )

    expected = _result()
    expected.pop("activation_payload")
    assert result == expected
    assert observed["input"] == {
        "schema_version": "manyforge.activation_preparation_request.v1",
        "source_document_digest": _DIGEST,
        "scenario_id": "scenario_a",
        "captured_input_digest": _DIGEST,
        "deployment": {"a": "deployment", "z": 1},
        "model_source_closure": _CLOSURE,
        "prepared_model_closure": _PREPARED_CLOSURE,
        "program": {"a": "program", "z": 2},
        "program_role_assignment": {"component_roles": [], "target_roles": []},
        "typed_parameter_overrides": {},
    }
    assert "shell" not in observed["kwargs"]
    assert observed["kwargs"]["stdin"] is subprocess.DEVNULL
    assert "--candidate-root" not in observed["command"]
    retained = preparer.prepared_value(result["prepared_activation_id"])
    assert retained == expected
    assert retained is not None
    retained["program_id"] = "mutated"
    assert preparer.prepared_value(result["prepared_activation_id"]) == expected
    preparer.release(result["prepared_activation_id"])
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_failure_retains_bounded_diagnostic_and_input_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    preparer = _preparer(tmp_path)
    diagnostic = "invalid binding: " + "x" * 5000

    def run(_command, **kwargs):
        kwargs["stderr"].write(diagnostic.encode())
        return SimpleNamespace(returncode=1)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            captured_input_digest=_DIGEST,
            deployment={},
            model_source_closure=_CLOSURE,
            program={},
            program_role_assignment={},
            typed_parameter_overrides={},
        )
    assert raised.value.code == "activation_preparation_failed"
    assert raised.value.details == {"preparer_detail": diagnostic[:4096]}
    assert "scenario=scenario_a" in caplog.text
    assert f"input={_DIGEST}" in caplog.text
    assert diagnostic[:4096] in caplog.text
    assert diagnostic not in caplog.text


def test_preparer_is_capacity_one_and_reports_a_stable_conflict(
    tmp_path: Path,
) -> None:
    preparer = _preparer(tmp_path)
    assert preparer._lock.acquire(blocking=False)  # exact bounded worker seam
    try:
        with pytest.raises(HeadlessApiError) as raised:
            preparer.prepare(
                source_document_digest=_DIGEST,
                scenario_id="scenario_a",
                captured_input_digest=_DIGEST,
                deployment={"api_version": "manyforge.deployment.v1"},
                model_source_closure=_CLOSURE,
                program={"api_version": "manyforge.program.v2"},
                program_role_assignment={"component_roles": [], "target_roles": []},
                typed_parameter_overrides={},
            )
    finally:
        preparer._lock.release()
    assert raised.value.code == "activation_preparer_busy"


def test_preparer_rejects_an_oversized_encoded_request_before_spawning(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    invoked = False

    def run(*_args, **_kwargs):
        nonlocal invoked
        invoked = True
        raise AssertionError("oversized request must not spawn the preparer")

    monkeypatch.setattr(subprocess, "run", run)
    monkeypatch.setattr(
        "manyforge_headless_api.activation_preparer.rfc8785.dumps",
        lambda _value: b"x" * (MAX_ACTIVATION_PREPARATION_REQUEST_BYTES + 1),
    )

    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment_intent.v1"},
            model_source_closure=_CLOSURE,
            program={"api_version": "manyforge.program.v2"},
            program_role_assignment={"component_roles": [], "target_roles": []},
            typed_parameter_overrides={},
        )

    assert raised.value.code == "activation_preparation_request_oversized"
    assert invoked is False


def test_preparer_rejects_timeout_and_invalid_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)

    def timeout(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["compiler"], 3.0)

    monkeypatch.setattr(subprocess, "run", timeout)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            model_source_closure=_CLOSURE,
            program={"api_version": "manyforge.program.v2"},
            program_role_assignment={"component_roles": [], "target_roles": []},
            typed_parameter_overrides={},
        )
    assert raised.value.code == "activation_preparation_timeout"

    def malformed(_command, **kwargs):
        kwargs["stdout"].write(b"{}")
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", malformed)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            model_source_closure=_CLOSURE,
            program={"api_version": "manyforge.program.v2"},
            program_role_assignment={"component_roles": [], "target_roles": []},
            typed_parameter_overrides={},
        )
    assert raised.value.code == "activation_preparer_response_invalid"
