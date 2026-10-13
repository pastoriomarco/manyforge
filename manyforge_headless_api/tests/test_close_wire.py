from copy import deepcopy

from fastapi.testclient import TestClient
from pydantic import ValidationError
import pytest

from manyforge_headless_api.app import create_app
from manyforge_headless_api.cclc import CCLCOpenRequestClient
from manyforge_headless_api.close_wire import (
    CanonicalEmptySelection, LifecycleTargetRetirementIntent, retirement_offer,
    retirement_result, settlement_result,
)
from manyforge_headless_api.manager import ManagerCommandClient, MANAGER_RESPONSE_VERSION
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService
from manyforge_headless_api.tests.test_cclc import FakeAdmission


def _intent():
    return {
        "request_context": {"request_id": "retire.1", "request_owner_epoch": "owner.1",
                            "source_label": "composer", "correlation_id": "close.1"},
        "scope": {"request_owner_epoch": "owner.1", "publication_generation": 2,
                  "active_public_pointer_digest": "sha256:" + "a" * 64},
        "expected_targets": [{"controllable_component_id": "arm", "requested_state": "active",
                              "origin": "operator", "target_generation": 3, "reached": False,
                              "recovered_cause": "fault"}],
    }


def _selection():
    return {"request_owner_epoch": "manager.1", "cell_id": "cell.1", "deployment_artifact_id": "deployment.empty",
            **{key: "sha256:" + str(index) * 64 for index, key in enumerate(
                ["deployment_revision", "candidate_retained_bundle_id", "candidate_graph_revision", "admission_digest"], 1)}}


def _result(intent):
    return {"request_context": deepcopy(intent["request_context"]), "replay_status": "accepted_new", "scope": deepcopy(intent["scope"]),
            "retired_targets": deepcopy(intent["expected_targets"])}


@pytest.mark.parametrize("mutation", ["foreign_owner", "duplicate", "reverse", "missing_reached", "missing_cause", "numeric_bool", "unsafe_generation", "extra"])
def test_retirement_ingress_preserves_complete_cas(mutation):
    value = _intent()
    if mutation == "foreign_owner": value["scope"]["request_owner_epoch"] = "other"
    if mutation == "duplicate": value["expected_targets"] *= 2
    if mutation == "reverse": value["expected_targets"].append({**value["expected_targets"][0], "controllable_component_id": "a"})
    if mutation == "missing_reached": del value["expected_targets"][0]["reached"]
    if mutation == "missing_cause": del value["expected_targets"][0]["recovered_cause"]
    if mutation == "numeric_bool": value["expected_targets"][0]["reached"] = 1
    if mutation == "unsafe_generation": value["scope"]["publication_generation"] = 2**53
    if mutation == "extra": value["confirmed"] = True
    with pytest.raises(ValidationError): LifecycleTargetRetirementIntent.model_validate(value)


@pytest.mark.parametrize("field", ["request_owner_epoch", "publication_generation", "active_public_pointer_digest"])
def test_retirement_and_settlement_refuse_foreign_publication(field):
    raw = _intent(); intent = LifecycleTargetRetirementIntent.model_validate(raw)
    result = _result(raw)
    result["scope"][field] = 4 if field == "publication_generation" else "sha256:" + "b" * 64
    with pytest.raises(HeadlessApiError): retirement_result(result, intent)
    with pytest.raises(HeadlessApiError): settlement_result({"scope": result["scope"], "settled": True}, intent.scope)


def test_retirement_result_preserves_every_internal_target_fact_and_empty_offer():
    raw = _intent(); intent = LifecycleTargetRetirementIntent.model_validate(raw)
    assert retirement_result(_result(raw), intent)["retired_targets"][0]["recovered_cause"] == "fault"
    bad = _result(raw); bad["retired_targets"][0]["reached"] = True
    with pytest.raises(HeadlessApiError): retirement_result(bad, intent)
    assert retirement_offer({"scope": raw["scope"], "targets": []})["targets"] == []
    with pytest.raises(HeadlessApiError): settlement_result({"scope": raw["scope"], "settled": 1}, intent.scope)


def test_retirement_optional_correlation_omits_absence_but_keeps_required_null_cause():
    raw = _intent(); del raw["request_context"]["correlation_id"]
    raw["expected_targets"][0]["recovered_cause"] = None
    assert LifecycleTargetRetirementIntent.model_validate(raw).model_dump(mode="json") == raw


def test_retirement_reads_and_settlement_never_submit_again(tmp_path):
    raw = _intent(); intent = LifecycleTargetRetirementIntent.model_validate(raw)
    calls = []
    class Client(CCLCOpenRequestClient):
        def _exchange(self, command, payload):
            calls.append((command, payload))
            if command == "lifecycle_target_retirement_offer": return {"scope": raw["scope"], "targets": raw["expected_targets"]}
            if command == "lifecycle_target_retirement_result": return None
            if command == "lifecycle_owner_settlement": return {"scope": raw["scope"], "settled": True}
            return _result(raw)
    client = Client(tmp_path)
    client.lifecycle_target_retirement_offer()
    client.retire_lifecycle_targets(intent)
    assert client.lifecycle_target_retirement_result(intent) is None
    client.lifecycle_owner_settlement(intent.scope)
    assert calls == [("lifecycle_target_retirement_offer", {}), ("lifecycle_target_retire", raw),
                     ("lifecycle_target_retirement_result", raw), ("lifecycle_owner_settlement", {"scope": raw["scope"]})]


@pytest.mark.parametrize("mutation", ["none", "extra", "prepared", "malformed_digest", "missing_field", "error"])
def test_selection_uses_only_passive_exact_sealed_command(tmp_path, mutation):
    calls = []; selection = _selection()
    class Manager(ManagerCommandClient):
        def _exchange(self, request):
            calls.append(request)
            response = {"api_version": MANAGER_RESPONSE_VERSION, "ok": True, "canonical_empty_selection": selection}
            if mutation == "extra": response["prepared_activation"] = {}
            if mutation == "prepared": selection["prepared_activation_id"] = "fake"
            if mutation == "malformed_digest": selection["candidate_graph_revision"] = "graph.other"
            if mutation == "missing_field": del selection["cell_id"]
            if mutation == "error": return {"api_version": MANAGER_RESPONSE_VERSION, "ok": False, "error": {"code": "unavailable", "message": "package changed"}}
            return response
    manager = Manager(tmp_path)
    if mutation == "none": assert manager.canonical_empty_selection() == selection
    else:
        with pytest.raises(HeadlessApiError): manager.canonical_empty_selection()
    assert calls == [{"api_version": "manyforge.manager_command.v1", "command": "canonical_empty_selection"}]


def test_selection_uses_approved_identity_bounds_and_record_bound():
    selection = _selection(); selection["cell_id"] = "x" * 128
    assert CanonicalEmptySelection.model_validate(selection).cell_id == selection["cell_id"]
    for invalid in ["x" * 129, "e\u0301", "", "é" * 65]:
        with pytest.raises(ValidationError): CanonicalEmptySelection.model_validate({**selection, "cell_id": invalid})
    with pytest.raises(ValidationError, match="16 KiB"):
        CanonicalEmptySelection.model_validate({**selection, "cell_id": "x" * (16 * 1024)})


def test_headless_retirement_http_maps_reads_and_invalid_input_without_effects(tmp_path):
    raw = _intent(); calls = []
    class Port(CCLCOpenRequestClient):
        def _exchange(self, command, payload):
            calls.append(command)
            if command.endswith("offer"): return {"scope": raw["scope"], "targets": raw["expected_targets"]}
            if command.endswith("result"): return None
            if command.endswith("settlement"): return {"scope": raw["scope"], "settled": True}
            return _result(raw)
    class Manager:
        def canonical_empty_selection(self): calls.append("canonical_empty_selection"); return _selection()
    service = HeadlessService(object(), Manager(), cell_admission=FakeAdmission())
    with TestClient(create_app(service=service, cclc=Port(tmp_path))) as client:
        assert client.get("/api/manager/canonical-empty-selection").json() == _selection()
        assert client.get("/api/cclc/targets/retirement-offer").status_code == 200
        bad = deepcopy(raw); bad["expected_targets"][0]["reached"] = 1
        assert client.post("/api/cclc/targets/retire", json=bad).status_code == 422
        assert client.post("/api/cclc/targets/retire", json=raw).json() == _result(raw)
        assert client.post("/api/cclc/targets/retire/result", json=raw).json() == {"result": None}
        assert client.post("/api/cclc/targets/settlement", json={"scope": raw["scope"]}).json()["settled"] is True
    assert calls == ["canonical_empty_selection", "lifecycle_target_retirement_offer", "lifecycle_target_retire",
                     "lifecycle_target_retirement_result", "lifecycle_owner_settlement"]


@pytest.mark.parametrize("field", ["request_id", "request_owner_epoch", "source_label", "correlation_id"])
def test_retirement_result_requires_exact_request_identity(field):
    raw = _intent(); result = _result(raw); result["request_context"][field] = "foreign"
    with pytest.raises(HeadlessApiError): retirement_result(result, LifecycleTargetRetirementIntent.model_validate(raw))


@pytest.mark.parametrize("caller_null", [False, True])
@pytest.mark.parametrize("echo_null", [False, True])
def test_retirement_result_normalizes_absent_and_null_correlation(caller_null, echo_null):
    raw = _intent(); del raw["request_context"]["correlation_id"]
    expected_context = deepcopy(raw["request_context"])
    if caller_null: raw["request_context"]["correlation_id"] = None
    intent = LifecycleTargetRetirementIntent.model_validate(raw)
    result = _result(raw)
    result["request_context"] = deepcopy(expected_context)
    if echo_null: result["request_context"]["correlation_id"] = None
    assert retirement_result(result, intent)["request_context"] == expected_context
    result["request_context"]["correlation_id"] = "foreign"
    with pytest.raises(HeadlessApiError): retirement_result(result, intent)


@pytest.mark.parametrize("field", ["requested_state", "origin", "recovered_cause"])
def test_retirement_rejects_unknown_enum(field):
    raw = _intent(); raw["expected_targets"][0][field] = "invented"
    with pytest.raises(ValidationError): LifecycleTargetRetirementIntent.model_validate(raw)


def test_retirement_http_rejects_over_capacity_before_send(tmp_path):
    calls = []
    class Port(CCLCOpenRequestClient):
        def _exchange(self, command, payload): calls.append(command); return None
    raw = _intent(); raw["expected_targets"] = [{**raw["expected_targets"][0], "controllable_component_id": f"arm.{i:02}"} for i in range(65)]
    service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    with TestClient(create_app(service=service, cclc=Port(tmp_path))) as client:
        assert client.post("/api/cclc/targets/retire", json=raw).status_code == 422
    assert calls == []


@pytest.mark.parametrize("code,status,path", [
    ("request_id_conflict", 409, "/api/cclc/targets/retire"),
    ("request_result_expired", 409, "/api/cclc/targets/retire/result"),
    ("request_ledger_full", 503, "/api/cclc/targets/retire"),
    ("stale_request_owner_epoch", 409, "/api/cclc/targets/retire"),
    ("component_lifecycle_owner_settlement_unknown", 503, "/api/cclc/targets/settlement"),
])
def test_close_owner_http_preserves_typed_refusals(tmp_path, code, status, path):
    class Port(CCLCOpenRequestClient):
        def _exchange(self, command, payload): raise HeadlessApiError(code, "owner refused", details={"reason": "actual owner fact"})
    raw = _intent(); body = {"scope": raw["scope"]} if path.endswith("settlement") else raw
    service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    with TestClient(create_app(service=service, cclc=Port(tmp_path))) as client: response = client.post(path, json=body)
    assert response.status_code == status
    detail = response.json()["detail"]
    assert {key: detail[key] for key in ["code", "message", "details"]} == {"code": code, "message": "owner refused", "details": {"reason": "actual owner fact"}}
    assert isinstance(detail["request_id"], str) and detail["request_id"]


@pytest.mark.parametrize("code,status", [("manager_command_rejected", 400), ("full_stack_restart_required", 409)])
def test_selection_http_preserves_actual_producer_refusals(code, status):
    class Manager:
        def canonical_empty_selection(self): raise HeadlessApiError(code, "sealed package refused", details={"reason": "actual admission"})
    service = HeadlessService(object(), Manager(), cell_admission=FakeAdmission())
    with TestClient(create_app(service=service)) as client: response = client.get("/api/manager/canonical-empty-selection")
    assert response.status_code == status
    assert response.json()["detail"]["code"] == code
