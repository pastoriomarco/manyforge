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

    force_close_ok = True

    def force_close(self, **kwargs: Any) -> dict[str, Any]:
        if kwargs.get("step") == "arm":
            return {"ok": True, "result": {"step": "arm", "stop_generation": 5}}
        if not self.force_close_ok:
            return {"ok": False, "error": {"code": "scenario_force_close_stop_unconfirmed"}}
        return {"ok": True, "result": {
            "step": "close_operations", "stop_generation": 5, "operations": [],
            "released_orphan_resources": [], "motion_block_cleared": False}}


_ATTACHMENT = {
    "carried_object_attachment_id": "attachment.1",
    "carried_object_attachment_revision": _DIGEST,
    "scene_resource_id": "graspable",
    "controllable_component_id": "lite6_robot",
    "command_target_id": "gripper",
}


class _Core:
    """The runtime snapshot's logical Scene, with or without a carried part."""

    def __init__(self, attachments: list[dict[str, Any]]) -> None:
        self.attachments = attachments

    def runtime_snapshot(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.headless_runtime_snapshot.v1",
            "sections": {"logical_scene": {"payload": {
                "api_version": "manyforge.logical_scene_source.v1",
                "snapshot": {"carried_object_attachments": self.attachments},
            }}},
        }

    def record_scenario_force_close(self, record: dict[str, Any]) -> dict[str, Any]:
        return {"event_id": "event.1", "recorded": True}


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
    *, allowed: bool, unavailable: Any, certified: bool = True,
    attachments: list[dict[str, Any]] | None = None,
) -> tuple[HeadlessService, _Preparer]:
    preparer = _Preparer()
    service = HeadlessService(
        core=_Core(attachments or []),  # type: ignore[arg-type]
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
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []


def test_ordinary_preparation_on_a_busy_cell_is_refused_as_not_ready() -> None:
    # M6 S1: preparation follows the current admission facts only. A busy
    # cell (no Stop fault, authoring closed) is "not ready", never a stale
    # generation, whatever generation the request names.
    service, preparer = _service(allowed=False, unavailable=False)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_ordinary())
    assert refused.value.code == "authoring_not_ready"
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
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []


def test_canonical_empty_preparation_waits_while_the_cell_stop_settles() -> None:
    # SX-370: a latch whose Stop has no certificate yet is a typed wait.
    service, preparer = _service(allowed=False, unavailable=None, certified=False)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []


@pytest.mark.parametrize("allowed, unavailable", [
    (True, False), (False, "stop_fault_recovery_closing"),
])
def test_close_is_refused_while_a_part_is_attached(allowed: bool, unavailable: Any) -> None:
    # M6: the caller detaches first; the canonical-empty Apply keeps the Scene,
    # so the attachment would outlive its carrier.
    service, preparer = _service(
        allowed=allowed, unavailable=unavailable, attachments=[dict(_ATTACHMENT)])
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"
    assert refused.value.details == {"carried_objects": [{
        "scene_resource_id": "graspable",
        "controllable_component_id": "lite6_robot",
        "command_target_id": "gripper",
    }]}
    assert preparer.prepared == [] and preparer.released == []


def test_close_is_accepted_without_an_attachment() -> None:
    service, preparer = _service(allowed=True, unavailable=False, attachments=[])
    assert service.prepare_activation(_canonical_empty()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.empty"}}
    assert preparer.prepared == ["canonical_empty"]


def test_ordinary_preparation_ignores_carried_objects() -> None:
    # Only Close is guarded; a Scenario preparation keeps its own rules.
    service, preparer = _service(
        allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    assert service.prepare_activation(_ordinary()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert preparer.prepared == ["ordinary"]


def _context() -> Any:
    from manyforge_headless_api.models import CallerAttributionRequest

    return CallerAttributionRequest.model_validate({
        "request_id": "request.force-close", "request_owner_epoch": "kernel.epoch",
        "source_label": "composer_ui", "correlation_id": None})


def test_idle_authoring_refuses_an_attached_part_whatever_the_caller_sends() -> None:
    # Item 38 / SX-604: there is no caller flag; an extra member is refused
    # and the attachment check applies outside Force close.
    service, preparer = _service(allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    with pytest.raises(ValueError):
        CanonicalEmptyActivationPreparationRequest.model_validate(
            {**_canonical_empty().model_dump(mode="json"), "force_close": True})
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"
    assert preparer.prepared == []


@pytest.mark.parametrize("allowed, unavailable", [
    (True, False), (False, "stop_fault_recovery_closing"),
])
def test_force_close_in_progress_prepares_with_a_part_attached(allowed: bool, unavailable: Any) -> None:
    # Item 38 (b): the robot's process is already dead, so Force close could
    # not detach. After this service ran the Kernel's Force close step, the
    # next empty preparation skips the attachment check, once.
    service, preparer = _service(
        allowed=allowed, unavailable=unavailable, attachments=[dict(_ATTACHMENT)])
    assert service.scenario_force_close_arm(5)["ok"] is True
    assert service.scenario_force_close_operations(_context())["ok"] is True
    assert service.prepare_activation(_canonical_empty()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.empty"}}
    assert preparer.prepared == ["canonical_empty"] and preparer.released == []
    # Consumed: a later Close (ordinary authoring) refuses again.
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"


def test_force_close_without_its_kernel_step_keeps_the_check() -> None:
    # Only the armed Kernel step opens the bypass: an arm alone, a refused
    # Kernel step, or a new arm after the step does not.
    service, preparer = _service(allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    service.scenario_force_close_arm(5)
    service.execution.force_close_ok = False  # type: ignore[attr-defined]
    assert service.scenario_force_close_operations(_context())["ok"] is False
    with pytest.raises(HeadlessApiError):
        service.prepare_activation(_canonical_empty())
    service.execution.force_close_ok = True  # type: ignore[attr-defined]
    assert service.scenario_force_close_operations(_context())["ok"] is True
    service.scenario_force_close_arm(6)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"
    assert preparer.prepared == []


def test_force_close_end_after_a_detach_failure_leaves_no_bypass() -> None:
    # SX-616: the detach failed in transport after the Kernel step; the
    # caller's sequence ends with step `end`, and the check applies again.
    service, preparer = _service(allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    service.scenario_force_close_arm(5)
    assert service.scenario_force_close_operations(_context())["ok"] is True
    assert service.scenario_force_close_end() == {
        "ok": True, "command": "force_close", "result": {"step": "end", "operations": []}}
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"
    assert preparer.prepared == []


def test_a_failed_preparation_consumes_the_force_close_exception() -> None:
    # SX-616: even without step `end`, the attempt's own preparation consumes
    # it whatever its outcome; a later preparation checks again.
    service, preparer = _service(allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    service.scenario_force_close_arm(5)
    assert service.scenario_force_close_operations(_context())["ok"] is True

    def fail(_selection: dict[str, Any]) -> dict[str, Any]:
        raise HeadlessApiError("activation_preparation_failed", "preparation failed")

    preparer.prepare_canonical_empty = fail  # type: ignore[method-assign]
    with pytest.raises(HeadlessApiError) as failed:
        service.prepare_activation(_canonical_empty())
    assert failed.value.code == "activation_preparation_failed"
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"


def test_an_ordinary_preparation_also_consumes_the_force_close_exception() -> None:
    service, preparer = _service(allowed=True, unavailable=False, attachments=[dict(_ATTACHMENT)])
    service.scenario_force_close_arm(5)
    assert service.scenario_force_close_operations(_context())["ok"] is True
    service.prepare_activation(_ordinary())
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_canonical_empty())
    assert refused.value.code == "scenario_close_attachment_present"
