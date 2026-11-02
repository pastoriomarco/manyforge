"""Activation preparation follows the current idle admission only; Close is
an ordinary Apply of the empty cell (CONTRACTS §11), with no sealed path."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.models import ActivationPreparationRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService

_DIGEST = "sha256:" + "a" * 64


class _Admission:
    def __init__(self, allowed: bool) -> None:
        self.allowed = allowed

    def get(self) -> dict[str, Any]:
        return {"authoring_allowed": self.allowed, "admission_generation": 7}


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


def _service(*, allowed: bool) -> tuple[HeadlessService, _Preparer]:
    preparer = _Preparer()
    service = HeadlessService(
        core=_Core(),  # type: ignore[arg-type]
        manager=object(),  # type: ignore[arg-type]
        execution=_Execution(),  # type: ignore[arg-type]
        cell_admission=_Admission(allowed),
        activation_preparer=preparer,
    )
    return service, preparer


def _request(deployment: dict[str, Any] | None = None) -> ActivationPreparationRequest:
    return ActivationPreparationRequest.model_construct(
        source_document_digest=_DIGEST, scenario_id="cell",
        project_path="/projects/cell.scenario", captured_input_digest=_DIGEST,
        deployment=deployment or {"components": []}, model_source_closure={},
        program=None, program_role_assignment={}, expected_admission_generation=7,
    )


def test_preparation_needs_the_current_idle_admission() -> None:
    service, preparer = _service(allowed=False)
    with pytest.raises(HeadlessApiError) as refused:
        service.prepare_activation(_request())
    assert refused.value.code == "authoring_not_ready"
    assert preparer.prepared == []


def test_the_empty_cell_is_prepared_like_any_scenario() -> None:
    service, preparer = _service(allowed=True)
    assert service.prepare_activation(_request()) == {
        "prepared_activation": {"prepared_activation_id": "prepared.ordinary"}}
    assert preparer.prepared[0]["deployment"] == {"components": []}
    assert preparer.prepared[0]["program"] is None
