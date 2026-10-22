"""A Program-only Revert may restore the Scenario only while the Saved
Deployment is the Applied one."""

from __future__ import annotations

from types import SimpleNamespace

from manyforge_composer.backend.scenario_restore import saved_deployment_differs_from


def _state(saved):
    session = SimpleNamespace(retained_state=lambda: {"saved_document": saved})
    return SimpleNamespace(deployment_draft_session=session)


def test_revisions_decide_when_both_documents_carry_one() -> None:
    saved = {"deployment_revision": "sha256:" + "1" * 64, "name": "cell"}
    assert not saved_deployment_differs_from(_state(saved), {**saved, "name": "other"})
    assert saved_deployment_differs_from(
        _state(saved), {**saved, "deployment_revision": "sha256:" + "2" * 64}
    )


def test_documents_decide_without_revisions_and_absence_is_no_difference() -> None:
    saved = {"api_version": "manyforge.deployment_intent.v2", "name": "cell"}
    assert saved_deployment_differs_from(_state(saved), dict(saved)) is False
    assert saved_deployment_differs_from(_state(saved), {**saved, "name": "edited"}) is True
    # Unknown, which the Revert guard treats as no proven difference.
    assert saved_deployment_differs_from(_state(None), saved) is None
    assert saved_deployment_differs_from(_state(saved), None) is None
