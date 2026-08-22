"""Strict private HTTP models owned by the Program runner."""

from __future__ import annotations

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from manyforge_headless_api.models import CallerAttributionRequest


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
        if idle != (
            self.program_run_id is None
            and self.cycle_claim_generation is None
            and self.terminal_or_settlement_reference is None
        ):
            raise ValueError("Program cycle has the wrong Idle/non-Idle shape")
        if (
            self.state == "terminal_awaiting_reset"
            and self.terminal_or_settlement_reference is None
        ):
            raise ValueError("terminal Program cycle requires its reference")
        return self


class ProgramCycleResetRequest(CallerAttributionRequest):
    """Release one exact terminal Program-cycle claim and publish Idle."""

    program_run_id: Annotated[str, Field(min_length=1, max_length=256)]
    expected_owner_epoch: Annotated[str, Field(min_length=1, max_length=256)]
    expected_cycle_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_claim_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    terminal_reference: Annotated[str, Field(pattern=_DIGEST)]
    settlement_evidence_digest: Annotated[str, Field(pattern=_DIGEST)]

    @field_validator("program_run_id", "expected_owner_epoch")
    @classmethod
    def bounded_identity(cls, value: str) -> str:
        if len(value.encode("utf-8")) > 256:
            raise ValueError("identity exceeds 256 UTF-8 bytes")
        return value


__all__ = ["ProgramCycleResetRequest", "ProgramCycleStatusDTO"]
