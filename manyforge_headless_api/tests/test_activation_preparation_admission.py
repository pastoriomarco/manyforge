"""Activation preparation follows the current idle admission only; Close is
an ordinary Apply of the empty cell, with no sealed path."""

from __future__ import annotations

from typing import Any

import pytest

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
    )
    service._ADMISSION_WAIT_S = 0.3  # type: ignore[misc]
    service._ADMISSION_POLL_S = 0.01  # type: ignore[misc]
    return service, preparer


def _request(deployment: dict[str, Any] | None = None) -> ActivationPreparationRequest:
    return ActivationPreparationRequest.model_construct(
        source_document_digest=_DIGEST, scenario_id="cell",
        project_path="/projects/cell.scenario", captured_input_digest=_DIGEST,
        deployment=deployment or {"components": []}, model_source_closure={},
        program=None, expected_admission_generation=7,
    )


def test_preparation_needs_the_current_idle_admission() -> None:
    # Admission that stays closed past the wait: typed refusal, nothing prepared.
    service, preparer = _service(allowed=False)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_request())
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []
    assert service.cell_admission.reads > 1  # type: ignore[union-attr]


def test_preparation_waits_for_a_momentarily_closed_admission() -> None:
    # F3b: admission closes briefly (a committed Apply's initial Scene
    # placement, a Reset's Idle reaching the Kernel) and reopens: the
    # preparation waits and succeeds.
    service, preparer = _service(allowed=True, closed_reads=3)
    assert service.prepare_activation(_request()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert service.cell_admission.reads == 4  # type: ignore[union-attr]
    assert len(preparer.prepared) == 1


def test_preparation_waits_within_the_floor_of_every_apply_bound() -> None:
    from manyforge_headless_api.component_model import DEFAULT_LIFECYCLE_TIMEOUT_S

    assert HeadlessService._ADMISSION_WAIT_S == DEFAULT_LIFECYCLE_TIMEOUT_S


def test_the_empty_cell_is_prepared_like_any_scenario() -> None:
    service, preparer = _service(allowed=True)
    assert service.prepare_activation(_request()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert preparer.prepared[0]["deployment"] == {"components": []}
    assert preparer.prepared[0]["program"] is None
