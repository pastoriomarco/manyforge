"""Restore the Scenario's Drafts to their Saved baselines.

Revert acts on the whole Scenario: whichever document drives the activation
(the Deployment when its Saved baseline differs from Manager Applied, the
Program otherwise), both Drafts return to Saved in the same commit.
"""

from __future__ import annotations

from collections.abc import Callable
from copy import deepcopy
from dataclasses import dataclass
import json
from typing import Any

from manyforge_behavior.program import Program

from .authoring_forest import derive_candidate, from_program
from .authoring_workspace import AuthoringWorkspaceIdentity
from .draft_session import DraftSessionError
from .state import ComposerState


@dataclass(frozen=True)
class SavedProgramRestore:
    """The Saved Program and the prepared Draft replacement that restores it."""

    saved_document: dict[str, Any]
    saved_program: Program | None
    replacement: Any


def saved_program_restore(
    state: ComposerState, retained: dict[str, Any], catalog: Any
) -> SavedProgramRestore:
    """Prepare restoring the Program Draft to its Saved baseline.

    An empty Saved Program (no forest, no scene) restores as authoring only.
    """

    saved_document = retained.get("saved_document")
    if not isinstance(saved_document, dict):
        raise DraftSessionError(
            "composer_saved_program_unavailable",
            "Composer has no retained Saved Program",
            status_code=404,
        )
    saved_empty = saved_document.get("forest") == [] and not saved_document.get("scene")
    saved_program = None if saved_empty else Program.from_dict(
        saved_document, catalog, path="<composer-program-revert-saved>")
    replacement_state = deepcopy(retained)
    restored_program = saved_program.to_dict() if saved_program is not None else None
    restored_authoring = (
        from_program(saved_program) if saved_program is not None else deepcopy(saved_document)
    )
    if replacement_state.get("authoring") != restored_authoring:
        replacement_state["draft_revision"] += 1
    replacement_state["program"] = restored_program
    replacement_state["authoring"] = restored_authoring
    replacement_state["diagnostics"] = derive_candidate(restored_authoring, catalog)[1]
    return SavedProgramRestore(
        saved_document=saved_document,
        saved_program=saved_program,
        replacement=state.draft_session.prepare_load_replacement(replacement_state, catalog),
    )


def saved_deployment_restore(
    state: ComposerState,
) -> Callable[[AuthoringWorkspaceIdentity], bool] | None:
    """Prepare returning the Deployment Draft to its Saved baseline.

    Returns ``None`` when there is no Saved Deployment or the Draft already is
    it; otherwise the no-I/O install for the identity the caller fenced, to run
    inside the caller's single whole-workspace commit.
    """

    retained = state.deployment_draft_session.retained_state()
    saved = retained.get("saved_document")
    if not isinstance(saved, dict) or retained.get("document") == saved:
        return None
    compiled = state.deployment_draft_session.preview_source(
        json.dumps(saved, ensure_ascii=False, allow_nan=False)
    )
    return lambda identity: state.deployment_draft_session.commit_compiled(
        compiled,
        draft_id=identity.deployment_draft_id,
        expected_revision=identity.deployment_draft_revision,
    )


def saved_deployment_differs_from(state: ComposerState, applied: Any) -> bool | None:
    """The current Saved Deployment is not the Applied one (a Program-only
    Revert cannot restore the Scenario then)."""

    return saved_deployment_differs(
        state.deployment_draft_session.retained_state().get("saved_document"), applied
    )


def saved_deployment_differs(saved: Any, applied: Any) -> bool | None:
    """One Saved Deployment document is not the Applied one. Revisions decide
    when both carry one; ``None`` (no difference proven) when either document
    is unknown. Pure: a caller compares the exact Saved it already read."""

    if not isinstance(saved, dict) or not isinstance(applied, dict):
        return None
    saved_revision = saved.get("deployment_revision")
    applied_revision = applied.get("deployment_revision")
    if saved_revision is not None and applied_revision is not None:
        return saved_revision != applied_revision
    return saved != applied


__all__ = [
    "SavedProgramRestore",
    "saved_deployment_differs",
    "saved_deployment_differs_from",
    "saved_deployment_restore",
    "saved_program_restore",
]
