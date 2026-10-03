from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest
import rfc8785

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_headless_api.activation_preparer import ActivationPreparer
from manyforge_headless_api.native import (
    HeadlessApiConfig,
    HeadlessApiError,
    MAX_ACTIVATION_PREPARATION_REQUEST_BYTES,
)


_DIGEST = "sha256:" + "1" * 64
_ROOT = Path(__file__).resolve().parents[2]
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


@pytest.mark.parametrize("cell_id", ["", "../runtime", "Uppercase", "a" * 129])
def test_preparer_rejects_invalid_runtime_configuration(tmp_path: Path, cell_id: str) -> None:
    with pytest.raises(ValueError, match="runtime_cell_id"):
        ActivationPreparer(
            executable=tmp_path / "preparer", capability_root=tmp_path,
            activation_root=tmp_path / "artifacts", runtime_cell_id=cell_id,
            timeout_seconds=3.0,
        )


def _preparer(tmp_path: Path) -> ActivationPreparer:
    capabilities = tmp_path / "capabilities"
    executable = tmp_path / "preparer"
    capabilities.mkdir()
    executable.write_text("fixture", encoding="utf-8")
    return ActivationPreparer(
        executable=executable,
        capability_root=capabilities,
        activation_root=tmp_path / "artifacts",
        runtime_cell_id="cell_a",
        timeout_seconds=3.0,
    )


@lru_cache(maxsize=1)
def _admitted_program() -> dict[str, object]:
    return Program(
        _ROOT / "examples" / "pick_and_place_minimal.program.yaml",
        build_node_environment().catalog,
    ).to_dict()


def _result(program: object = ...) -> dict[str, object]:
    if program is ...:
        program = copy.deepcopy(_admitted_program())
    identity: dict[str, object] = {
        "schema_version": "manyforge.prepared_activation_input.v5",
        "source_document_digest": _DIGEST,
        "scenario_id": "scenario_a",
        "captured_input_digest": _DIGEST,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": _DIGEST,
        "retained_bundle_id": _DIGEST,
        "graph_revision": _DIGEST,
        "program_id": program["program_id"] if program is not None else None,
        "program_revision": (
            program["program_revision"] if program is not None else None
        ),
        "program_outcome": (
            {"kind": "bound", "program_binding_id": _DIGEST}
            if program is not None
            else {"kind": "no_program"}
        ),
        "node_environment_digest": _DIGEST,
        "assistant_mode": "forbidden",
        "activation_payload": {
            "program": program,
            "raw_artifacts": [{"bytes": "YQ=="}],
        },
    }
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
    )

    expected = _result()
    expected.pop("activation_payload")
    assert result == expected
    assert observed["input"] == {
        "schema_version": "manyforge.activation_preparation_request.v3",
        "runtime_cell_id": "cell_a",
        "source_document_digest": _DIGEST,
        "scenario_id": "scenario_a",
        "captured_input_digest": _DIGEST,
        "deployment": {"a": "deployment", "z": 1},
        "model_source_closure": _CLOSURE,
        "prepared_model_closure": _PREPARED_CLOSURE,
        "program": {"a": "program", "z": 2},
        "program_role_assignment": {"component_roles": [], "target_roles": []},
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
            deployment={"api_version": "manyforge.deployment_intent.v2"},
            model_source_closure=_CLOSURE,
            program={"api_version": "manyforge.program.v2"},
            program_role_assignment={"component_roles": [], "target_roles": []},
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
        )
    assert raised.value.code == "activation_preparer_response_invalid"


def _prepare_with_result(
    preparer: ActivationPreparer,
    monkeypatch: pytest.MonkeyPatch,
    result: dict[str, object],
) -> dict[str, object]:
    def run(_command, **kwargs):
        kwargs["stdout"].write(json.dumps(result).encode("utf-8"))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    return preparer.prepare(
        source_document_digest=_DIGEST,
        scenario_id="scenario_a",
        captured_input_digest=_DIGEST,
        deployment={"api_version": "manyforge.deployment.v1"},
        model_source_closure=_CLOSURE,
        program=None,
        program_role_assignment={"component_roles": [], "target_roles": []},
    )


def test_preparer_admits_no_program_without_program_validation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _prepare_with_result(preparer, monkeypatch, _result(None))
    assert result["program_id"] is None
    assert preparer.prepared_value(result["prepared_activation_id"]) == result


def _without_tree_with_valid_identity(program: dict[str, object]) -> None:
    program.pop("tree")
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )


def _with_unknown_node_parameter(program: dict[str, object]) -> None:
    program["tree"]["parameters"]["not_a_catalog_parameter"] = 1
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )


@pytest.mark.parametrize(
    ("malform", "code"),
    [
        (_without_tree_with_valid_identity, "activation_preparation_failed"),
        (_with_unknown_node_parameter, "activation_preparation_failed"),
        (
            lambda program: program.__setitem__("name", "stale identity"),
            "activation_preparation_failed",
        ),
        (
            lambda program: program.__setitem__("program_id", "other_program"),
            "activation_preparer_response_invalid",
        ),
    ],
)
def test_preparer_refuses_a_program_the_program_validator_refuses(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, malform, code: str
) -> None:
    preparer = _preparer(tmp_path)
    program = copy.deepcopy(_admitted_program())
    malform(program)
    result = _result(program)
    if code == "activation_preparer_response_invalid":
        # The receipt names the admitted Program; the payload diverges from it.
        identity = {
            key: value for key, value in result.items()
            if key != "prepared_activation_id"
        }
        identity["program_id"] = _admitted_program()["program_id"]
        result = {
            **identity,
            "prepared_activation_id": "sha256:"
            + hashlib.sha256(rfc8785.dumps(identity)).hexdigest(),
        }

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_result(preparer, monkeypatch, result)

    assert raised.value.code == code
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_refuses_a_program_payload_for_no_program(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result(None)
    identity = {
        key: value for key, value in result.items()
        if key != "prepared_activation_id"
    }
    identity["activation_payload"] = {
        **identity["activation_payload"],
        "program": copy.deepcopy(_admitted_program()),
    }
    result = {
        **identity,
        "prepared_activation_id": "sha256:"
        + hashlib.sha256(rfc8785.dumps(identity)).hexdigest(),
    }

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_result(preparer, monkeypatch, result)

    assert raised.value.code == "activation_preparer_response_invalid"
