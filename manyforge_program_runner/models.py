"""Strict private HTTP models owned by the Program runner."""

from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramScenePreparationRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
)


_DIGEST = r"^sha256:[0-9a-f]{64}$"


class ProgramCycleStatusDTO(BaseModel):
    """Program Runner's complete facts; Manager owns the readiness reduction."""

    model_config = ConfigDict(extra="forbid", strict=True)

    program_runner_owner_epoch: Annotated[str, Field(min_length=1, max_length=256)]
    cycle_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    state: Literal[
        "idle_after_reset",
        "starting",
        "running",
        "stopping",
        "terminal_awaiting_reset",
    ]
    program_run_id: Annotated[str, Field(min_length=1, max_length=256)] | None
    cycle_claim_generation: (
        Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None
    )
    terminal_or_settlement_reference: (
        Annotated[str, Field(pattern=_DIGEST)] | None
    )

    @model_validator(mode="after")
    def exact_state_shape(self) -> "ProgramCycleStatusDTO":
        idle = self.state == "idle_after_reset"
        if idle:
            if not (
                self.program_run_id is None
                and self.cycle_claim_generation is None
                and self.terminal_or_settlement_reference is None
            ):
                raise ValueError("Program cycle has the wrong Idle shape")
        elif self.program_run_id is None:
            raise ValueError("active Program cycle requires its run identity")
        elif self.cycle_claim_generation is None and self.state != "starting":
            raise ValueError("acquired Program cycle requires its claim generation")
        if (
            self.state == "terminal_awaiting_reset"
            and self.terminal_or_settlement_reference is None
        ):
            raise ValueError("terminal Program cycle requires its reference")
        return self


__all__ = [
    "CycleRecoveryActionRequest",
    "ManualResultStageRequest",
    "ProgramCycleResetRequest",
    "ProgramCycleStatusDTO",
    "ProgramScenePreparationRequest",
    "RuntimeBlackboardWriteRequest",
    "RuntimeOverrideActionRequest",
    "RuntimeOverridePromotionRequest",
    "RuntimeOverrideStageRequest",
]
