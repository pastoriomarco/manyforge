"""Pure recovery-guidance tests independent of any assistant transport."""

from manyforge_composer.backend import assistant_recovery
from manyforge_composer.backend.assistant_recovery import (
    NUDGES,
    RecoveryAttempt,
    compose_recovery,
    format_stuck_nudge,
    impasse_message,
    meta_nudge,
    nudge_for_index,
    truncation_pattern_nudge,
)


def test_nudges_rotate_deterministically_without_tool_vocabulary() -> None:
    assert nudge_for_index(1) == NUDGES[0]
    assert nudge_for_index(len(NUDGES) + 1) == NUDGES[0]
    assert nudge_for_index(-5) == NUDGES[0]
    assert all("tree_draft_" not in nudge for nudge in NUDGES)


def test_envelope_renders_without_mutating_or_serializing_detail() -> None:
    detail = {"message": "Rejected request", "code": "typed_rejection"}
    original = dict(detail)

    envelope = compose_recovery(
        detail,
        "Choose a valid typed input",
        "Review the current state",
        extra_lines=("Use a bounded operation", ""),
    )

    assert envelope.detail is detail
    assert detail == original
    assert envelope.extra_lines == ("Use a bounded operation",)
    assert envelope.rendered_message() == (
        "Rejected request. Action: Choose a valid typed input. "
        "Review the current state. Use a bounded operation."
    )


def test_envelope_leaves_non_text_diagnostic_unrendered() -> None:
    envelope = compose_recovery(17, None, "Retry")
    assert envelope.rendered_message() is None


def test_repetition_detectors_use_typed_operation_history() -> None:
    truncations = tuple(
        RecoveryAttempt(
            operation="replace-subtree",
            failed=True,
            error_class="invalid_payload",
            detail="invalid JSON at line 1 column 4097",
        )
        for _ in range(3)
    )
    assert "same parse offset" in (truncation_pattern_nudge(truncations) or "")
    assert "replace-subtree" in (format_stuck_nudge(truncations) or "")

    recovered = truncations + (
        RecoveryAttempt(operation="replace-subtree", failed=False),
    )
    assert truncation_pattern_nudge(recovered) is None
    assert format_stuck_nudge(recovered) is None


def test_meta_and_impasse_summaries_are_bounded_and_deterministic() -> None:
    history = (
        RecoveryAttempt("inspect", True, "stale"),
        RecoveryAttempt("apply", True, "conflict"),
        RecoveryAttempt("inspect", True, "stale"),
    )

    summary = meta_nudge(history, threshold=3)
    assert summary is not None
    assert "inspect×2" in summary
    assert "stale×2" in summary

    impasse = impasse_message(failure_count=3, history=history)
    assert "3 consecutive failed attempts" in impasse
    assert "request direction" in impasse


def test_v1_free_form_classifier_surface_is_absent() -> None:
    assert not hasattr(assistant_recovery, "ACTION_BY_CLASS")
    assert not hasattr(assistant_recovery, "classify_detail")
    assert not hasattr(assistant_recovery, "context_aware_action")
