from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest
import rfc8785

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_headless_api import activation_preparer
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
    return Program(
        _ROOT / "examples" / "pick_and_place_minimal.program.yaml",
        build_node_environment().catalog,
    ).to_dict()


def _result(program: object = ...) -> dict[str, object]:
    if program is ...:
        program = copy.deepcopy(_admitted_program())
    identity: dict[str, object] = {
        "schema_version": "manyforge.prepared_activation_input.v6",
        "origin": {"kind": "scenario", "scenario_id": "scenario_a",
                   "source_document_digest": _DIGEST},
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
    assert "origin_kind=scenario origin_id=scenario_a" in caplog.text
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


def _prepare_with_output(
    preparer: ActivationPreparer,
    monkeypatch: pytest.MonkeyPatch,
    output: bytes,
) -> dict[str, object]:
    def run(_command, **kwargs):
        kwargs["stdout"].write(output)
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


def _prepare_with_result(
    preparer: ActivationPreparer,
    monkeypatch: pytest.MonkeyPatch,
    result: dict[str, object],
) -> dict[str, object]:
    return _prepare_with_output(
        preparer, monkeypatch, _publish(preparer._activation_root, result)
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

    receipt = _prepare_with_output(preparer, monkeypatch, output)

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


def _sealed_selection() -> dict[str, str]:
    return {"request_owner_epoch": "manager-actual", "cell_id": "cell_a",
            "deployment_artifact_id": "cell_a", "deployment_revision": _DIGEST,
            "candidate_retained_bundle_id": _DIGEST, "candidate_graph_revision": _DIGEST,
            "admission_digest": "sha256:" + "2" * 64}


def _sealed_result() -> dict[str, object]:
    selection = _sealed_selection()
    origin = {"kind": "sealed_canonical_empty", "cell_id": selection["cell_id"],
              "deployment_artifact_id": selection["deployment_artifact_id"],
              "deployment_revision": selection["deployment_revision"],
              "retained_bundle_id": selection["candidate_retained_bundle_id"],
              "graph_revision": selection["candidate_graph_revision"],
              "admission_digest": selection["admission_digest"]}
    value = _result(None)
    value["origin"] = origin
    value["captured_input_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps({
        "schema_version": "manyforge.sealed_canonical_empty_capture.v1", "origin": origin,
    })).hexdigest()
    value["activation_payload"].update({"program_binding": None,
                                      "program_role_assignment": {"component_roles": [], "target_roles": []}})
    return _reidentified(value)


def test_sealed_preparation_uses_actual_package_and_shared_worker_cache(tmp_path, monkeypatch) -> None:
    preparer = _preparer(tmp_path)
    preparer._canonical_empty_package_root = tmp_path / "actual-manager-package"
    preparer._canonical_empty_admission_filename = Path("actual-admission.json")
    observed = {}

    def run(command, **kwargs):
        observed["command"] = command
        observed["input"] = json.loads(Path(command[command.index("--input") + 1]).read_bytes())
        kwargs["stdout"].write(_publish(tmp_path / "artifacts", _sealed_result()))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(subprocess, "run", run)
    receipt = preparer.prepare_canonical_empty(_sealed_selection())
    assert observed["input"] == {
        "schema_version": "manyforge.sealed_canonical_empty_preparation_request.v1",
        "runtime_cell_id": "cell_a", "origin": _sealed_result()["origin"],
        "captured_input_digest": _sealed_result()["captured_input_digest"],
    }
    assert "scenario_id" not in receipt and "source_document_digest" not in receipt
    command = observed["command"]
    assert command[command.index("--canonical-empty-package-root") + 1] == str(tmp_path / "actual-manager-package")
    assert command[command.index("--canonical-empty-admission-filename") + 1] == "actual-admission.json"
    assert "--publish-canonical-empty" not in command
    assert preparer.prepared_value(receipt["prepared_activation_id"]) == receipt
    assert not list(tmp_path.glob("activation-preparation-*.json"))
    preparer._lock.acquire()
    try:
        with pytest.raises(HeadlessApiError) as busy:
            preparer.prepare_canonical_empty(_sealed_selection())
        assert busy.value.code == "activation_preparer_busy"
    finally:
        preparer._lock.release()


def test_sealed_preparation_without_actual_admission_configuration_is_unavailable(tmp_path, monkeypatch) -> None:
    def unexpected(*_args, **_kwargs):
        pytest.fail("missing configured admission must not invoke worker")
    monkeypatch.setattr(subprocess, "run", unexpected)
    with pytest.raises(HeadlessApiError) as error:
        _preparer(tmp_path).prepare_canonical_empty(_sealed_selection())
    assert error.value.code == "canonical_empty_preparation_unavailable"


@pytest.mark.parametrize("field,value", [("program", {"program_id": "fake"}),
    ("program_binding", {}), ("program_role_assignment", {"component_roles": ["fake"], "target_roles": []})])
def test_sealed_stored_product_rejects_program_facts(field, value) -> None:
    product = _sealed_result()
    product["activation_payload"][field] = value
    product = _reidentified(product)
    with pytest.raises(HeadlessApiError) as error:
        activation_preparer._require_stored_product(_receipt(product), rfc8785.dumps(product))
    assert error.value.code == "activation_preparer_response_invalid"


def test_ordinary_worker_receipt_preserves_existing_json_member_semantics(tmp_path, monkeypatch) -> None:
    preparer = _preparer(tmp_path)
    product = _result()
    output = _publish(tmp_path / "artifacts", product)
    # Existing JSON parsing keeps the final member; stored canonical bytes and
    # their exact content identity are still checked independently.
    output = b'{"schema_version":"superseded",' + output[1:]
    assert _prepare_with_output(preparer, monkeypatch, output) == _receipt(product)


def test_sealed_worker_receipt_must_match_actual_selection(tmp_path, monkeypatch) -> None:
    preparer = _preparer(tmp_path)
    preparer._canonical_empty_package_root = tmp_path / "actual-manager-package"
    preparer._canonical_empty_admission_filename = Path("actual-admission.json")
    def run(_command, **kwargs):
        product = _sealed_result()
        product["origin"]["admission_digest"] = "sha256:" + "e" * 64
        kwargs["stdout"].write(_publish(tmp_path / "artifacts", _reidentified(product)))
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError) as error:
        preparer.prepare_canonical_empty(_sealed_selection())
    assert error.value.code == "activation_preparer_response_invalid"
    assert not preparer._prepared
