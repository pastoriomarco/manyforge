"""Bounded Composer context of one Apply that first Resets a held run.

Applying a Draft while a Program run exists is the ordinary Apply preceded
by the ordinary Reset, chained by the browser: Reset -> observed authoring
Idle -> Apply of exactly the confirmed Draft revision. Composer records the
confirmed identities once, before Reset is sent, in its existing structured
logs, and links the later Apply request/result to the same correlation. The
Program runner stays authoritative for Reset settlement and the Manager for
Apply settlement; this module mirrors neither state machine, persists
nothing and grants no authority.
"""

from __future__ import annotations

import logging
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field

LOGGER = logging.getLogger("manyforge_composer.reset_apply")

_Identity = Annotated[str, Field(min_length=1, max_length=256)]
_Revision = Annotated[str, Field(min_length=1, max_length=256)]
_Count = Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]


class ResetApplyContextRequest(BaseModel):
    """The confirmed identities of one Apply that Resets a held run first."""

    model_config = ConfigDict(
        populate_by_name=True, extra="forbid", allow_inf_nan=False, strict=True
    )

    schema_version: Literal["manyforge.composer_reset_apply_context.v1"] = Field(
        alias="schemaVersion"
    )
    correlation_id: _Identity = Field(alias="correlationId")
    document: Literal["program", "deployment"]
    program_run_id: _Identity = Field(alias="programRunId")
    program_runner_owner_epoch: _Identity = Field(alias="programRunnerOwnerEpoch")
    cycle_generation: _Count = Field(alias="cycleGeneration")
    draft_id: _Identity = Field(alias="draftId")
    draft_revision: _Count = Field(alias="draftRevision")
    draft_content_revision: _Revision | None = Field(alias="draftContentRevision")
    applied_revision: _Revision | None = Field(alias="appliedRevision")


def record_reset_apply_context(body: ResetApplyContextRequest) -> dict[str, Any]:
    """Log the confirmed identities once; the record is not a workflow object."""

    LOGGER.info(
        "reset_apply_context",
        extra={
            "reset_apply_correlation_id": body.correlation_id,
            "document": body.document,
            "program_run_id": body.program_run_id,
            "program_runner_owner_epoch": body.program_runner_owner_epoch,
            "cycle_generation": body.cycle_generation,
            "draft_id": body.draft_id,
            "draft_revision": body.draft_revision,
            "draft_content_revision": body.draft_content_revision,
            "applied_revision": body.applied_revision,
        },
    )
    return {"correlationId": body.correlation_id, "recorded": True}


def log_reset_apply_link(
    phase: Literal["program_apply", "deployment_apply"],
    *,
    correlation_id: str,
    request_id: str,
    outcome: str,
    **identities: Any,
) -> None:
    """Link one Apply request/result to the recorded context by correlation."""

    LOGGER.info(
        "reset_apply_link",
        extra={
            "reset_apply_correlation_id": correlation_id,
            "reset_apply_phase": phase,
            "request_id": request_id,
            "outcome": outcome,
            **{key: value for key, value in identities.items() if value is not None},
        },
    )


__all__ = [
    "ResetApplyContextRequest",
    "log_reset_apply_link",
    "record_reset_apply_context",
]
