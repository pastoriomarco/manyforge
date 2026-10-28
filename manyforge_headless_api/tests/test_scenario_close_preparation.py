"""D6 (SX-366): while Scenario Close runs, authoring stays closed and only
the verified canonical-empty preparation, from any caller, is admitted."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.models import (
    ActivationPreparationRequest,
    CanonicalEmptyActivationPreparationRequest,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService

_DIGEST = "sha256:" + "a" * 64
_SELECTION = {
    "request_owner_epoch": "manager.1",
    "cell_id": "cell.1",
    "deployment_artifact_id": "artifact.canonical_empty",
    "deployment_revision": _DIGEST,
    "candidate_retained_bundle_id": _DIGEST,
    "candidate_graph_revision": _DIGEST,
    "admission_digest": _DIGEST,
}


class _Admission:
    def __init__(self, allowed: bool) -> None:
        self.allowed = allowed

    def get(self) -> dict[str, Any]:
        return {"authoring_allowed": self.allowed, "admission_generation": 7}


class _Manager:
    def canonical_empty_selection(self) -> dict[str, Any]:
        return dict(_SELECTION)


class _Execution:
    def __init__(self, unavailable: Any, certified: bool = True) -> None:
        self.unavailable = unavailable
        self.certified = certified

    def stop_fault(self) -> dict[str, Any]:
        offer = None if self.unavailable is False else {
            "recovery_unavailable_reason": self.unavailable,
            "configuration_expectations": [],
            "latch": {"latch_id": "stop-fault.1.1", "stop_certificate": (
                {"stop_certificate_id": "stop.1", "stop_certificate_revision": 1}
                if self.certified else None)},
        }
        return {"ok": True, "command": "get_stop_fault", "stop_fault": offer}


class _Preparer:
    def __init__(self) -> None:
        self.prepared: list[str] = []
        self.released: list[str] = []

    def prepare_canonical_empty(self, selection: dict[str, Any]) -> dict[str, Any]:
        self.prepared.append("canonical_empty")
        return {"prepared_activation_id": "prepared.empty"}

    def prepare(self, **_kwargs: Any) -> dict[str, Any]:
        self.prepared.append("ordinary")
        return {"prepared_activation_id": "prepared.ordinary"}

    def release(self, prepared_activation_id: str) -> None:
        self.released.append(prepared_activation_id)


def _service(
    *, allowed: bool, unavailable: Any, certified: bool = True
) -> tuple[HeadlessService, _Preparer]:
    preparer = _Preparer()
    service = HeadlessService(
        core=object(),  # type: ignore[arg-type]
        manager=_Manager(),  # type: ignore[arg-type]
        execution=_Execution(unavailable, certified),  # type: ignore[arg-type]
        cell_admission=_Admission(allowed),
        activation_preparer=preparer,
    )
    return service, preparer


def _canonical_empty() -> CanonicalEmptyActivationPreparationRequest:
    return CanonicalEmptyActivationPreparationRequest.model_validate({
        "origin": {
            "kind": "sealed_canonical_empty",
            "cell_id": _SELECTION["cell_id"],
            "deployment_artifact_id": _SELECTION["deployment_artifact_id"],
            "deployment_revision": _DIGEST,
            "retained_bundle_id": _DIGEST,
            "graph_revision": _DIGEST,
            "admission_digest": _DIGEST,
        },
        "request_owner_epoch": "manager.1",
        "expected_admission_generation": 7,
    })


def _ordinary() -> ActivationPreparationRequest:
    return ActivationPreparationRequest.model_construct(
        source_document_digest=_DIGEST, scenario_id="cell", project_path="/projects/cell.scenario",
        captured_input_digest=_DIGEST, deployment={}, model_source_closure={}, program=None,
        program_role_assignment={}, expected_admission_generation=7,
    )


def test_canonical_empty_preparation_is_admitted_while_closing() -> None:
    service, preparer = _service(allowed=False, unavailable="stop_fault_recovery_closing")
    assert service.prepare_activation(_canonical_empty()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.empty"}}
    assert preparer.prepared == ["canonical_empty"] and preparer.released == []


def test_ordinary_preparation_is_refused_while_closing() -> None:
    service, preparer = _service(allowed=False, unavailable="stop_fault_recovery_closing")
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_ordinary())
    assert refused.value.code == "stale_generation"
    assert preparer.prepared == []


@pytest.mark.parametrize("unavailable", [
    None, "stop_fault_recovery_installed_configuration_unavailable", False,
    # SX-370: closing plus an unrelated blocker (for example a held claim).
    "stop_fault_recovery_closing_not_ready",
])
def test_canonical_empty_preparation_needs_idle_admission_outside_closing(unavailable: Any) -> None:
    # A latch outside Close, another unavailability reason, or no latch at all.
    service, preparer = _service(allowed=False, unavailable=unavailable)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "stale_generation"
    assert preparer.prepared == []


def test_canonical_empty_preparation_waits_while_the_cell_stop_settles() -> None:
    # SX-370: a latch whose Stop has no certificate yet is a typed wait.
    service, preparer = _service(allowed=False, unavailable=None, certified=False)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []
