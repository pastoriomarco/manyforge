"""Activation preparation follows the current idle admission only; Close is
an ordinary Apply of the empty cell, with no sealed path."""

from __future__ import annotations

import time
from typing import Any

from manyforge_headless_api.models import ActivationPreparationRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService

_DIGEST = "sha256:" + "a" * 64


class _Admission:
    def __init__(self, allowed: bool, *, closed_reads: int = 0) -> None:
        self.allowed = allowed
        self.closed_reads = closed_reads
        self.reads = 0

    def get(self) -> dict[str, Any]:
        # Closed for the first ``closed_reads`` reads, then ``allowed``.
        self.reads += 1
        allowed = self.allowed and self.reads > self.closed_reads
        return {"authoring_allowed": allowed, "admission_generation": 7}


class _Preparer:
    def __init__(self) -> None:
        self.prepared: list[dict[str, Any]] = []

    def prepare(self, **kwargs: Any) -> dict[str, Any]:
        self.prepared.append(kwargs)
        return {"prepared_activation_id": "prepared.ordinary"}


class _Execution:
    pass


class _Core:
    pass


def _service(
    *, allowed: bool, closed_reads: int = 0
) -> tuple[HeadlessService, _Preparer]:
    preparer = _Preparer()
    service = HeadlessService(
        core=_Core(),  # type: ignore[arg-type]
        manager=object(),  # type: ignore[arg-type]
        execution=_Execution(),  # type: ignore[arg-type]
        cell_admission=_Admission(allowed, closed_reads=closed_reads),
        activation_preparer=preparer,
        application_start_id="3f9a0000000000a1",
    )
    service._ADMISSION_WAIT_S = 0.3  # type: ignore[misc]
    service._ADMISSION_POLL_S = 0.01  # type: ignore[misc]
    return service, preparer


_KEY = "c0" + "ab" * 15


def _request(deployment: dict[str, Any] | None = None) -> ActivationPreparationRequest:
    return ActivationPreparationRequest.model_construct(
        request_key=_KEY, source_document_digest=_DIGEST, scenario_id="cell",
        project_path="/projects/cell.scenario", captured_input_digest=_DIGEST,
        deployment=deployment or {"components": []}, model_source_closure={},
        program=None, expected_admission_generation=7,
    )


def _refused(code: str) -> bool:
    # As the route's HTTP status classes them: 4xx is a definite refusal.
    return code == "authoring_not_ready"


def _prepared(service: HeadlessService) -> dict[str, Any]:
    """Accept one preparation (OD-214) and return its ended row."""

    reply = service.prepare_activation(_request(), refused=_refused)
    assert reply["schema_version"] == "manyforge.activation_preparation_reply.v1"
    assert reply["command"]["request_key"] == _KEY
    assert reply["command"]["end"] is None
    deadline = time.monotonic() + 5.0
    while (row := service.activation_preparations.find(_KEY))["end"] is None:
        assert time.monotonic() < deadline
        time.sleep(0.01)
    return row


def test_preparation_needs_the_current_idle_admission() -> None:
    # Admission that stays closed past the wait: the row ends refused with
    # the typed code at the admission step; nothing prepared.
    service, preparer = _service(allowed=False)
    row = _prepared(service)
    assert row["step"] == "awaiting_admission"
    assert (row["end"]["outcome"], row["end"]["code"]) == ("refused", "authoring_not_ready")
    assert preparer.prepared == []
    assert service.cell_admission.reads > 1  # type: ignore[union-attr]
    try:
        service.prepared_activation(_DIGEST)
    except HeadlessApiError as missing:
        assert missing.code == "activation_preparation_result_not_found"
    else:
        raise AssertionError("nothing was prepared")


def test_preparation_waits_for_a_momentarily_closed_admission() -> None:
    # F3b: admission closes briefly (a committed Apply's initial Scene
    # placement, a Reset's Idle reaching the Kernel) and reopens: the
    # preparation waits and succeeds.
    service, preparer = _service(allowed=True, closed_reads=3)
    row = _prepared(service)
    assert (row["step"], row["end"]["outcome"]) == ("preparing", "succeeded")
    assert service.prepared_activation(_DIGEST) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert service.cell_admission.reads == 4  # type: ignore[union-attr]
    assert len(preparer.prepared) == 1


def test_preparation_waits_within_the_floor_of_every_apply_bound() -> None:
    from manyforge_headless_api.component_model import DEFAULT_LIFECYCLE_TIMEOUT_S

    assert HeadlessService._ADMISSION_WAIT_S == DEFAULT_LIFECYCLE_TIMEOUT_S


def test_the_empty_cell_is_prepared_like_any_scenario() -> None:
    service, preparer = _service(allowed=True)
    assert _prepared(service)["end"]["outcome"] == "succeeded"
    assert service.prepared_activation(_DIGEST) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert preparer.prepared[0]["deployment"] == {"components": []}
    assert preparer.prepared[0]["program"] is None
