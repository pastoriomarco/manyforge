"""Source-neutral diagnostic presentation tests."""

from enum import Enum

from manyforge_composer.backend import cycle_diagnostics
from manyforge_composer.backend.cycle_diagnostics import (
    HeadlessDiagnosticSnapshot,
    present_diagnostic_snapshot,
)


class _State(Enum):
    INTERVENTION_REQUIRED = "intervention_required"


def test_typed_snapshot_is_normalized_and_defensively_copied() -> None:
    details = {"resource": "arm0"}
    snapshot = HeadlessDiagnosticSnapshot(
        state=_State.INTERVENTION_REQUIRED,
        error_code=" operation_intervention_required ",
        message=" Inspect the affected resource ",
        details=details,
    )
    details["resource"] = "mutated"

    presentation = present_diagnostic_snapshot(snapshot)

    assert presentation.state == "INTERVENTION_REQUIRED"
    assert presentation.error_code == "operation_intervention_required"
    assert presentation.message == "Inspect the affected resource"
    assert presentation.details == {"resource": "arm0"}


def test_explicit_error_code_is_the_only_message_fallback() -> None:
    snapshot = HeadlessDiagnosticSnapshot(
        state="FAILED",
        error_code="typed_failure",
        details={"reason": "free-form details do not select precedence"},
    )

    presentation = present_diagnostic_snapshot(snapshot)

    assert presentation.message == "typed_failure"
    assert presentation.error_code == "typed_failure"


def test_snapshot_rejects_empty_state() -> None:
    try:
        HeadlessDiagnosticSnapshot(state="")
    except ValueError as error:
        assert "state must be non-empty" in str(error)
    else:
        raise AssertionError("empty diagnostic state was accepted")


def test_cycle_manager_kernel_merge_surface_is_absent() -> None:
    assert not hasattr(cycle_diagnostics, "manager_cycle_error_fields")
    assert not hasattr(cycle_diagnostics, "active_cycle_error_fields")
