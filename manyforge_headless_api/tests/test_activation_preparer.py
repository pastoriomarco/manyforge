from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
import logging
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
import rfc8785

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.program import Program
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.skill_loader import default_skills_dir, load_skill, load_skill_catalog
from manyforge_headless_api import activation_preparer
from manyforge_headless_api.activation_preparer import ActivationPreparer
from manyforge_headless_api.models import ActivationPreparationRequest
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


def test_activation_request_contains_no_role_assignment() -> None:
    request = {
        "source_document_digest": _DIGEST, "scenario_id": "scenario_a",
        "project_path": "/projects/scenario_a.scenario", "captured_input_digest": _DIGEST,
        "deployment": {"component": "robot"}, "model_source_closure": _CLOSURE,
        "program": {"api_version": "manyforge.program.v3"},
        "expected_admission_generation": 1,
    }
    assert "program_role_assignment" not in ActivationPreparationRequest.model_validate(
        request
    ).model_dump()
    with pytest.raises(ValueError):
        ActivationPreparationRequest.model_validate({
            **request, "program_role_assignment": {"component_roles": [], "target_roles": []},
        })


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
    (tmp_path / "artifacts").mkdir()
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
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    context = {"api_version": "manyforge.program.v3", "program_id": "test",
               "program_profile": "composable-cell-v1", "name": "test",
               "catalog": {"api_version": catalog.api_version,
                           "content_version": catalog.content_version}}
    skill = load_skill(default_skills_dir() / "move_to_named_config.skill.yaml")
    expanded = load_skill_catalog(default_skills_dir(), catalog=catalog).expand(
        skill_id=skill["skill_id"], arguments=skill["example"]["arguments"],
        instance_prefix="first", program_context=context,
    )
    document = {**context, "tree": expanded["tree"], "program_revision": _DIGEST}
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return Program.from_dict(document, catalog).to_dict()


def _result(program: object = ...) -> dict[str, object]:
    if program is ...:
        program = copy.deepcopy(_admitted_program())
    identity: dict[str, object] = {
        "schema_version": "manyforge.prepared_activation_input.v7",
        "origin": {"kind": "scenario", "scenario_id": "scenario_a",
                   "source_document_digest": _DIGEST,
                   "project_path": "/projects/scenario_a.scenario"},
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
        "assistant_mode": "forbidden" if program is not None else "optional",
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


def _receipt(result: dict[str, object]) -> dict[str, object]:
    return {key: value for key, value in result.items() if key != "activation_payload"}


def _stored_path(activation_root: Path, result: dict[str, object]) -> Path:
    identity = str(result["prepared_activation_id"]).removeprefix("sha256:")
    return activation_root / "prepared-activations" / f"{identity}.json"


def _publish(activation_root: Path, result: dict[str, object]) -> bytes:
    """Store ``result`` as the native preparer does and return its stdout."""

    path = _stored_path(activation_root, result)
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(rfc8785.dumps(result))
    return rfc8785.dumps(_receipt(result)) + b"\n"


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
        kwargs["stdout"].write(_publish(tmp_path / "artifacts", _result()))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    result = preparer.prepare(
        source_document_digest=_DIGEST,
        scenario_id="scenario_a",
        project_path="/projects/scenario_a.scenario",
        captured_input_digest=_DIGEST,
        deployment={"z": 1, "a": "deployment"},
        model_source_closure=_CLOSURE,
        program=_admitted_program(),
    )

    expected = _result()
    expected.pop("activation_payload")
    assert result == expected
    assert observed["input"] == {
        "schema_version": "manyforge.activation_preparation_request.v4",
        "runtime_cell_id": "cell_a",
        "source_document_digest": _DIGEST,
        "scenario_id": "scenario_a",
        "project_path": "/projects/scenario_a.scenario",
        "captured_input_digest": _DIGEST,
        "deployment": {"a": "deployment", "z": 1},
        "model_source_closure": _CLOSURE,
        "prepared_model_closure": _PREPARED_CLOSURE,
        "program": _admitted_program(),
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


def test_preparer_rejects_invalid_program_before_native_subprocess(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    program = copy.deepcopy(_admitted_program())
    program["tree"]["parameters"]["unknown_catalog_parameter"] = True
    called = False

    def run(*_args, **_kwargs):
        nonlocal called
        called = True
        raise AssertionError("native preparer must not run")

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST, scenario_id="scenario_a",
            project_path="/projects/scenario_a.scenario", captured_input_digest=_DIGEST,
            deployment={"a": "deployment"}, model_source_closure=_CLOSURE,
            program=program,
        )
    assert raised.value.code == "activation_preparation_failed"
    assert not called


def test_preparer_failure_retains_bounded_diagnostic_and_input_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    preparer = _preparer(tmp_path)
    diagnostic = "invalid binding: " + "x" * 5000

    def run(_command, **kwargs):
        kwargs["stderr"].write(diagnostic.encode())
        return SimpleNamespace(returncode=1)

    monkeypatch.setattr(subprocess, "run", run)
    # Capture directly: suite logger configuration can disable propagation.
    module_logger = logging.getLogger(activation_preparer.__name__)
    previous_level = module_logger.level
    handler_already_attached = caplog.handler in module_logger.handlers
    module_logger.addHandler(caplog.handler)
    module_logger.setLevel(logging.ERROR)
    try:
        with pytest.raises(HeadlessApiError) as raised:
            preparer.prepare(
                source_document_digest=_DIGEST,
                scenario_id="scenario_a",
                project_path="/projects/scenario_a.scenario",
                captured_input_digest=_DIGEST,
                deployment={},
                model_source_closure=_CLOSURE,
                program=None,
            )
    finally:
        if not handler_already_attached:
            module_logger.removeHandler(caplog.handler)
        module_logger.setLevel(previous_level)
    assert raised.value.code == "activation_preparation_failed"
    assert raised.value.details == {"preparer_detail": diagnostic[:4096]}
    assert "origin_kind=scenario origin_id=scenario_a" in caplog.text
    assert f"input={_DIGEST}" in caplog.text
    assert diagnostic[:4096] in caplog.text
    assert diagnostic not in caplog.text


def test_typed_preparer_refusal_carries_its_cause_code(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # SMALL3: the preparer prints a typed refusal's catalog code on stdout;
    # the operator text on stderr is kept as it was.
    preparer = _preparer(tmp_path)
    diagnostic = (
        "manyforge_prepare_activation: Activation Deployment compiler: "
        "pneumatic_io_presets_coincident: gripper Open and Close resolve to the "
        "same gripper coordinate"
    )

    def run(_command, **kwargs):
        kwargs["stdout"].write(b'{"cause_code":"pneumatic_io_presets_coincident"}\n')
        kwargs["stderr"].write(diagnostic.encode())
        return SimpleNamespace(returncode=2)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            project_path="/projects/scenario_a.scenario",
            captured_input_digest=_DIGEST,
            deployment={},
            model_source_closure=_CLOSURE,
            program=None,
        )
    assert raised.value.code == "activation_preparation_failed"
    assert raised.value.details == {
        "preparer_detail": diagnostic,
        "cause_code": "pneumatic_io_presets_coincident",
    }


@pytest.mark.parametrize("total_count,truncated", [(1, False), (4, True)])
def test_reference_refusal_preserves_bounded_diagnostics(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, total_count: int, truncated: bool,
) -> None:
    preparer = _preparer(tmp_path)
    row = {"code": "component_reference_missing", "location": {
        "document": "program", "path": "/tree/parameters/target",
        "node_instance_id": "move", "parameter": "target",
    }, "payload": {"component_id": "robot"}}
    refusal = {"code": "program_reference_validation_failed", "diagnostics": [row],
               "total_count": total_count, "truncated": truncated}

    def run(_command, **kwargs):
        kwargs["stdout"].write(json.dumps(refusal).encode())
        return SimpleNamespace(returncode=2)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as raised:
        preparer.prepare(
            source_document_digest=_DIGEST, scenario_id="scenario_a",
            project_path="/projects/scenario_a.scenario", captured_input_digest=_DIGEST,
            deployment={}, model_source_closure=_CLOSURE, program=None,
        )
    assert raised.value.code == "program_reference_validation_failed"
    assert raised.value.details == {key: refusal[key] for key in
                                    ("diagnostics", "total_count", "truncated")}


def test_reference_refusal_rejects_inconsistent_truncation() -> None:
    output = json.dumps({"code": "program_reference_validation_failed",
                         "diagnostics": [], "total_count": 2,
                         "truncated": False}).encode()
    assert activation_preparer._reference_refusal(output) is None


def test_format_refusal_keeps_typed_versions() -> None:
    details = {"actual_api_version": "manyforge.program.v2",
               "supported_api_version": "manyforge.program.v3"}
    assert activation_preparer._format_refusal(json.dumps({
        "code": "program_format_unsupported", "details": details,
    }).encode()) == details
    assert activation_preparer._format_refusal(json.dumps({
        "code": "program_format_unsupported", "details": {**details, "extra": "x"},
    }).encode()) is None


def test_preparer_propagates_format_refusal(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    preparer = _preparer(tmp_path)
    details = {"actual_api_version": "manyforge.program.v2",
               "supported_api_version": "manyforge.program.v3"}

    def run(_command, **kwargs):
        kwargs["stdout"].write(json.dumps({
            "code": "program_format_unsupported", "details": details,
        }).encode())
        return SimpleNamespace(returncode=2)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as caught:
        preparer.prepare(
            source_document_digest=_DIGEST, scenario_id="scenario_a",
            project_path="/projects/scenario_a.scenario", captured_input_digest=_DIGEST,
            deployment={}, model_source_closure=_CLOSURE,
            program={"api_version": "manyforge.program.v2"},
        )
    assert caught.value.code == "program_format_unsupported"
    assert caught.value.details == details


def test_native_preparation_request_omits_role_assignment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    preparer = _preparer(tmp_path)
    captured: dict[str, object] = {}

    def run(command, **_kwargs):
        path = Path(command[command.index("--input") + 1])
        captured.update(json.loads(path.read_text()))
        return SimpleNamespace(returncode=2)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError):
        preparer.prepare(
            source_document_digest=_DIGEST, scenario_id="scenario_a",
            project_path="/projects/scenario_a.scenario", captured_input_digest=_DIGEST,
            deployment={"component": "robot"}, model_source_closure=_CLOSURE,
            program=_admitted_program(),
        )
    assert captured["schema_version"] == "manyforge.activation_preparation_request.v4"
    assert "program_role_assignment" not in captured


def test_reference_candidate_uses_server_config_and_preserves_empty_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    preparer = _preparer(tmp_path)
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    calls = []
    class CandidateError(RuntimeError): pass
    class ContractError(ValueError): pass
    class Incomplete(RuntimeError): pass
    class Candidate:
        def validate_program_references(self, program, draft):
            calls.append(("validate", json.loads(program), draft))
            return json.dumps({"code": "program_reference_validation_failed", "diagnostics": [],
                               "total_count": 0, "truncated": False})
        def selector_facts_json(self):
            return json.dumps({key: [] for key in ("components", "targets", "signals", "frames", "bodies", "rules")})
    def factory(*args):
        calls.append(("factory", args))
        return Candidate()
    monkeypatch.setitem(sys.modules, "manyforge_core_py", SimpleNamespace(
        prepare_activation_candidate=factory, PreparedActivationCandidateError=CandidateError,
        ProgramContractError=ContractError, ProgramReferenceValidationIncomplete=Incomplete,
    ))
    program = {"schema_version": "forest", "forest": []}
    digest = "sha256:" + hashlib.sha256(rfc8785.dumps(program)).hexdigest()
    result = preparer.validate_program_references(
        deployment={"components": []}, model_source_closure=_CLOSURE,
        program=program, program_content_digest=digest, draft=True,
    )
    assert result == {"diagnostics": [], "total_count": 0, "truncated": False, "selector_facts": {key: [] for key in ("components", "targets", "signals", "frames", "bodies", "rules")}}
    assert calls[0][1][0] == str(tmp_path / "capabilities")
    assert calls[0][1][-1] == "cell_a"
    assert calls[1] == ("validate", program, True)
    assert list((tmp_path / "artifacts").iterdir()) == []


def test_reference_candidate_is_reused_until_the_deployment_or_sources_change(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    preparer = _preparer(tmp_path)
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    monkeypatch.setattr(activation_preparer, "_prepare_model_closure", lambda **_kwargs: {})
    prepared: list[tuple] = []
    class CandidateError(RuntimeError): pass
    class ContractError(ValueError): pass
    class Incomplete(RuntimeError): pass
    class Candidate:
        def __init__(self, deployment: str) -> None:
            self.deployment = deployment
        def validate_program_references(self, program, _draft):
            return json.dumps({"code": "program_reference_validation_failed",
                               "diagnostics": [], "total_count": 0, "truncated": False})
        def selector_facts_json(self):
            return json.dumps({key: [self.deployment] if key == "components" else []
                               for key in ("components", "targets", "signals", "frames", "bodies", "rules")})
    def factory(_root, deployment, _sources, _prepared, _cell):
        prepared.append(deployment)
        return Candidate(deployment)
    monkeypatch.setitem(sys.modules, "manyforge_core_py", SimpleNamespace(
        prepare_activation_candidate=factory, PreparedActivationCandidateError=CandidateError,
        ProgramContractError=ContractError, ProgramReferenceValidationIncomplete=Incomplete,
    ))

    def validate(deployment, program, closure=_CLOSURE):
        digest = "sha256:" + hashlib.sha256(rfc8785.dumps(program)).hexdigest()
        return preparer.validate_program_references(
            deployment=deployment, model_source_closure=closure,
            program=program, program_content_digest=digest, draft=True)

    first = validate({"components": []}, {"forest": []})
    # A Program edit reuses the candidate and gives the fresh result.
    edited = validate({"components": []}, {"forest": [{"node_id": "timer"}]})
    assert len(prepared) == 1
    assert edited == first
    # A Deployment change prepares again.
    changed = validate({"components": ["robot"]}, {"forest": []})
    assert len(prepared) == 2 and changed != first
    # So does a model-source change.
    validate({"components": ["robot"]}, {"forest": []}, {**_CLOSURE, "components": [{"id": "a"}]})
    assert len(prepared) == 3


def test_reference_candidate_digest_only_hits_the_held_candidate_or_is_refused(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    monkeypatch.setattr(activation_preparer, "_prepare_model_closure", lambda **_kwargs: {})
    prepared: list[str] = []
    class CandidateError(RuntimeError): pass
    class ContractError(ValueError): pass
    class Incomplete(RuntimeError): pass
    class Candidate:
        def __init__(self, deployment: str) -> None:
            self.deployment = deployment
        def validate_program_references(self, program, _draft):
            return json.dumps({"code": "program_reference_validation_failed",
                               "diagnostics": [], "total_count": 0, "truncated": False})
        def selector_facts_json(self):
            return json.dumps({key: [self.deployment] if key == "components" else []
                               for key in ("components", "targets", "signals", "frames", "bodies", "rules")})
    def factory(_root, deployment, _sources, _prepared, _cell):
        prepared.append(deployment)
        return Candidate(deployment)
    monkeypatch.setitem(sys.modules, "manyforge_core_py", SimpleNamespace(
        prepare_activation_candidate=factory, PreparedActivationCandidateError=CandidateError,
        ProgramContractError=ContractError, ProgramReferenceValidationIncomplete=Incomplete,
    ))
    closure_digest = "sha256:" + hashlib.sha256(rfc8785.dumps(_CLOSURE)).hexdigest()

    def validate(preparer, deployment, program, **sources):
        digest = "sha256:" + hashlib.sha256(rfc8785.dumps(program)).hexdigest()
        return preparer.validate_program_references(
            deployment=deployment, program=program, program_content_digest=digest, draft=True, **sources)

    def refused(preparer, deployment, source_digest) -> None:
        with pytest.raises(HeadlessApiError) as caught:
            validate(preparer, deployment, {"forest": []}, model_source_closure_digest=source_digest)
        assert caught.value.code == "model_sources_unknown"

    preparer = _preparer(tmp_path)
    # Nothing held yet: the digest names no candidate.
    refused(preparer, {"components": []}, closure_digest)
    full = validate(preparer, {"components": []}, {"forest": [1]}, model_source_closure=_CLOSURE)
    # A digest-only request reuses the held candidate with the same result.
    assert validate(preparer, {"components": []}, {"forest": [1]}, model_source_closure_digest=closure_digest) == full
    assert len(prepared) == 1
    # An unknown closure digest, or a changed Deployment with the old digest, misses.
    refused(preparer, {"components": []}, "sha256:" + "f" * 64)
    refused(preparer, {"components": ["robot"]}, closure_digest)
    assert len(prepared) == 1
    # A restarted Headless holds no candidate.
    (tmp_path / "restarted").mkdir()
    refused(_preparer(tmp_path / "restarted"), {"components": []}, closure_digest)


def test_reference_candidate_incomplete_is_not_valid(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    preparer = _preparer(tmp_path)
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    class CandidateError(RuntimeError): pass
    class ContractError(ValueError): pass
    class Incomplete(RuntimeError): pass
    class Candidate:
        def validate_program_references(self, _program, _draft):
            raise Incomplete("unfinished")
    monkeypatch.setitem(sys.modules, "manyforge_core_py", SimpleNamespace(
        prepare_activation_candidate=lambda *_args: Candidate(),
        PreparedActivationCandidateError=CandidateError, ProgramContractError=ContractError,
        ProgramReferenceValidationIncomplete=Incomplete,
    ))
    program = {"forest": []}
    digest = "sha256:" + hashlib.sha256(rfc8785.dumps(program)).hexdigest()
    with pytest.raises(HeadlessApiError) as caught:
        preparer.validate_program_references(
            deployment={"components": []}, model_source_closure=_CLOSURE,
            program=program, program_content_digest=digest, draft=True,
        )
    assert caught.value.code == "activation_preparer_unavailable"


def test_reference_candidate_rejects_false_program_digest_before_builder(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    preparer = _preparer(tmp_path)
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    with pytest.raises(HeadlessApiError) as caught:
        preparer.validate_program_references(
            deployment={"components": []}, model_source_closure=_CLOSURE,
            program={"forest": []}, program_content_digest="sha256:" + "0" * 64,
            draft=True,
        )
    assert caught.value.code == "activation_preparation_failed"


def test_raw_draft_uses_shared_partial_forest_validation() -> None:
    from manyforge_behavior.authoring_forest import empty_authoring

    draft = empty_authoring(activation_preparer._program_catalog())
    activation_preparer._validate_raw_reference_program(draft, True)
    with pytest.raises(HeadlessApiError) as caught:
        activation_preparer._validate_raw_reference_program({"forest": []}, True)
    assert caught.value.code == "activation_preparation_failed"


def test_reference_candidate_rejects_old_format_before_digest(tmp_path: Path) -> None:
    preparer = _preparer(tmp_path)
    with pytest.raises(HeadlessApiError) as caught:
        preparer.validate_program_references(
            deployment={}, model_source_closure={},
            program={"api_version": "manyforge.program.v2"},
            program_content_digest="sha256:" + "0" * 64, draft=True,
        )
    assert caught.value.code == "program_format_unsupported"
    assert caught.value.details == {
        "actual_api_version": "manyforge.program.v2",
        "supported_api_version": "manyforge.program.v3",
    }


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
                project_path="/projects/scenario_a.scenario",
                captured_input_digest=_DIGEST,
                deployment={"api_version": "manyforge.deployment.v1"},
                model_source_closure=_CLOSURE,
                program=None,
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
            project_path="/projects/scenario_a.scenario",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment_intent.v2"},
            model_source_closure=_CLOSURE,
            program=None,
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
            project_path="/projects/scenario_a.scenario",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            model_source_closure=_CLOSURE,
            program=None,
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
            project_path="/projects/scenario_a.scenario",
            captured_input_digest=_DIGEST,
            deployment={"api_version": "manyforge.deployment.v1"},
            model_source_closure=_CLOSURE,
            program=None,
        )
    assert raised.value.code == "activation_preparer_response_invalid"


def _prepare_with_output(
    preparer: ActivationPreparer,
    monkeypatch: pytest.MonkeyPatch,
    output: bytes,
    program: dict[str, object] | None = None,
) -> dict[str, object]:
    def run(_command, **kwargs):
        kwargs["stdout"].write(output)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    return preparer.prepare(
        source_document_digest=_DIGEST,
        scenario_id="scenario_a",
        project_path="/projects/scenario_a.scenario",
        captured_input_digest=_DIGEST,
        deployment={"api_version": "manyforge.deployment.v1"},
        model_source_closure=_CLOSURE,
        program=program,
    )


def _prepare_with_result(
    preparer: ActivationPreparer,
    monkeypatch: pytest.MonkeyPatch,
    result: dict[str, object],
) -> dict[str, object]:
    return _prepare_with_output(
        preparer, monkeypatch, _publish(preparer._activation_root, result),
        _admitted_program() if result["program_id"] is not None else None,
    )


def _reidentified(identity: dict[str, object]) -> dict[str, object]:
    identity = {key: value for key, value in identity.items()
                if key != "prepared_activation_id"}
    return {
        **identity,
        "prepared_activation_id": "sha256:"
        + hashlib.sha256(rfc8785.dumps(identity)).hexdigest(),
    }


def test_preparer_returns_only_the_receipt_of_the_stored_product(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    output = _publish(tmp_path / "artifacts", result)
    # The compact response is the receipt alone: no second payload copy.
    assert json.loads(output) == _receipt(result)
    assert b"activation_payload" not in output

    receipt = _prepare_with_output(preparer, monkeypatch, output, _admitted_program())

    assert receipt == _receipt(result)
    assert preparer.prepared_value(result["prepared_activation_id"]) == receipt


def test_preparer_refuses_the_full_value_as_its_response(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    _publish(tmp_path / "artifacts", result)

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, rfc8785.dumps(result))

    assert raised.value.code == "activation_preparer_response_invalid"
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_bounds_its_compact_response(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, b" " * (1024 * 1024 + 1))

    assert raised.value.code == "activation_preparer_output_oversized"


def test_preparer_refuses_a_receipt_without_its_stored_product(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    output = _publish(tmp_path / "artifacts", result)
    _stored_path(tmp_path / "artifacts", result).unlink()

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, output)

    assert raised.value.code == "activation_preparer_response_invalid"
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_refuses_a_stored_product_reached_through_a_symlink(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    output = _publish(tmp_path / "artifacts", result)
    stored = _stored_path(tmp_path / "artifacts", result)
    elsewhere = tmp_path / "elsewhere.json"
    stored.rename(elsewhere)
    stored.symlink_to(elsewhere)

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, output)

    assert raised.value.code == "activation_preparer_response_invalid"


@pytest.mark.parametrize(
    "rewrite",
    [
        # Same value, but not its RFC 8785 bytes: the Manager would refuse it.
        lambda stored: stored + b"\n",
        # A duplicate member that a plain JSON parser would silently drop.
        lambda stored: b'{"schema_version":"duplicate",' + stored[1:],
    ],
    ids=["non_canonical_bytes", "duplicate_member"],
)
def test_preparer_refuses_a_stored_product_the_manager_would_refuse(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, rewrite
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    output = _publish(tmp_path / "artifacts", result)
    stored = _stored_path(tmp_path / "artifacts", result)
    stored.write_bytes(rewrite(stored.read_bytes()))

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, output)

    assert raised.value.code == "activation_preparer_response_invalid"
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_reports_a_stored_product_read_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    output = _publish(tmp_path / "artifacts", result)

    def failing_fstat(_descriptor: int) -> os.stat_result:
        raise OSError(5, "Input/output error")

    monkeypatch.setattr(activation_preparer.os, "fstat", failing_fstat)

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, output)

    assert raised.value.code == "activation_preparer_response_invalid"
    assert "could not be read" in str(raised.value)


def test_preparer_refuses_a_receipt_that_differs_from_the_stored_product(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    _publish(tmp_path / "artifacts", result)
    # Same stored identity, but the receipt names another candidate bundle.
    receipt = {**_receipt(result), "retained_bundle_id": "sha256:" + "2" * 64}

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_output(preparer, monkeypatch, rfc8785.dumps(receipt))

    assert raised.value.code == "activation_preparer_response_invalid"
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_refuses_a_stored_product_with_a_false_content_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result()
    # The receipt and stored value agree, but the payload is not the content
    # the RFC 8785 identity names.
    result["activation_payload"] = {
        **result["activation_payload"],
        "raw_artifacts": [{"bytes": "Yg=="}],
    }

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_result(preparer, monkeypatch, result)

    assert raised.value.code == "activation_preparer_response_invalid"
    assert "false content identity" in str(raised.value)
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


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
        api_version="manyforge.program.v3",
    )


def _with_unknown_node_parameter(program: dict[str, object]) -> None:
    program["tree"]["parameters"]["not_a_catalog_parameter"] = 1
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )


@pytest.mark.parametrize(
    ("malform", "code"),
    [
        (_without_tree_with_valid_identity, "activation_preparer_response_invalid"),
        (_with_unknown_node_parameter, "activation_preparer_response_invalid"),
        (
            lambda program: program.__setitem__("name", "stale identity"),
            "activation_preparer_response_invalid",
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
        result = _reidentified(
            {**result, "program_id": _admitted_program()["program_id"]}
        )

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_result(preparer, monkeypatch, result)

    assert raised.value.code == code
    assert preparer.prepared_value(result["prepared_activation_id"]) is None


def test_preparer_refuses_a_program_payload_for_no_program(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    preparer = _preparer(tmp_path)
    result = _result(None)
    result = _reidentified(
        {
            **result,
            "activation_payload": {
                **result["activation_payload"],
                "program": copy.deepcopy(_admitted_program()),
            },
        }
    )

    with pytest.raises(HeadlessApiError) as raised:
        _prepare_with_result(preparer, monkeypatch, result)

    assert raised.value.code == "activation_preparer_response_invalid"


def test_ordinary_worker_receipt_preserves_existing_json_member_semantics(tmp_path, monkeypatch) -> None:
    preparer = _preparer(tmp_path)
    product = _result()
    output = _publish(tmp_path / "artifacts", product)
    # Existing JSON parsing keeps the final member; stored canonical bytes and
    # their exact content identity are still checked independently.
    output = b'{"schema_version":"superseded",' + output[1:]
    assert _prepare_with_output(preparer, monkeypatch, output, _admitted_program()) == _receipt(product)


def test_candidate_selector_facts_are_forwarded_from_same_candidate(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    preparer = _preparer(tmp_path)
    monkeypatch.setattr(activation_preparer, "_validate_raw_reference_program", lambda *_args: None)
    facts = {"components": [{"component_id": "tool"}], "targets": [], "signals": [],
             "frames": [{"reference": {"component_id": "tool", "frame_id": "tip"},
                         "ambiguous": True, "moving_targets": []}], "bodies": [], "rules": []}
    class CandidateError(RuntimeError): pass
    class ContractError(ValueError): pass
    class Incomplete(RuntimeError): pass
    class Candidate:
        def validate_program_references(self, _program, _draft):
            return json.dumps({"code": "program_reference_validation_failed", "diagnostics": [],
                               "total_count": 0, "truncated": False})
        def selector_facts_json(self):
            return json.dumps(facts)
    monkeypatch.setitem(sys.modules, "manyforge_core_py", SimpleNamespace(
        prepare_activation_candidate=lambda *_args: Candidate(),
        PreparedActivationCandidateError=CandidateError, ProgramContractError=ContractError,
        ProgramReferenceValidationIncomplete=Incomplete))
    program = {"forest": []}
    digest = "sha256:" + hashlib.sha256(rfc8785.dumps(program)).hexdigest()
    result = preparer.validate_program_references(deployment={}, model_source_closure=_CLOSURE,
        program=program, program_content_digest=digest, draft=True)
    assert result["selector_facts"] == facts
