"""Strict private HTTP models owned by the Program runner."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramScenePreparationRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
)
from manyforge_headless_api.control_protocol import AllowedExecutionCommandDTO


_DIGEST = r"^sha256:[0-9a-f]{64}$"


class ProgramNodeAvailabilityRequest(BaseModel):
    """Exact current inputs supplied by Headless for one readiness reduction."""

    model_config = ConfigDict(extra="forbid", strict=True)

    applied: dict[str, Any]
    selection: dict[str, Any]


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
        "resetting",
        "terminal_awaiting_reset",
        "recovery_required",
    ]
    program_run_id: Annotated[str, Field(min_length=1, max_length=256)] | None
    cycle_claim_generation: (
        Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None
    )
    terminal_or_settlement_reference: (
        Annotated[str, Field(pattern=_DIGEST)] | None
    )
    execution_control_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    causal_correlation_id: Annotated[
        str, Field(min_length=1, max_length=256)
    ] | None
    causal_session_id: Annotated[str, Field(min_length=1, max_length=256)] | None
    causal_client_sequence: Annotated[
        str, Field(pattern=r"^[1-9][0-9]{0,19}$")
    ] | None
    current_command_kind: Annotated[
        str, Field(min_length=1, max_length=256)
    ] | None
    failure: dict[str, Any] | None
    reset_phase: Annotated[str, Field(min_length=1, max_length=256)] | None
    allowed_commands: list[AllowedExecutionCommandDTO]

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
        elif self.program_run_id is None and self.state != "recovery_required":
            raise ValueError("active Program cycle requires its run identity")
        elif self.cycle_claim_generation is None and self.state not in {
            "starting",
            "stopping",
            "resetting",
            "terminal_awaiting_reset",
            "recovery_required",
        }:
            raise ValueError("acquired Program cycle requires its claim generation")
        if (
            self.state == "terminal_awaiting_reset"
            and self.terminal_or_settlement_reference is None
        ):
            raise ValueError("terminal Program cycle requires its reference")
        if (self.causal_session_id is None) != (
            self.causal_client_sequence is None
        ):
            raise ValueError("causal session and client sequence must be paired")
        if self.state == "resetting" and self.reset_phase is None:
            raise ValueError("Resetting Program cycle requires its phase")
        if self.state == "recovery_required" and self.failure is None:
            raise ValueError("recovery-required Program cycle requires failure")
        return self


__all__ = [
    "CycleRecoveryActionRequest",
    "ManualResultStageRequest",
    "ProgramCycleResetRequest",
    "ProgramCycleStatusDTO",
    "ProgramNodeAvailabilityRequest",
    "ProgramFullStopResetRequest",
    "ProgramFullStopResumeRequest",
    "ProgramScenePreparationRequest",
    "RuntimeBlackboardWriteRequest",
    "RuntimeOverrideActionRequest",
    "RuntimeOverridePromotionRequest",
    "RuntimeOverrideStageRequest",
]
