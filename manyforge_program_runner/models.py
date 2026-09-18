"""Strict private HTTP models owned by the Program runner."""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, ConfigDict

from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    ProgramCycleResetRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramScenePreparationRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
)
from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO


class ProgramNodeAvailabilityRequest(BaseModel):
    """Exact current inputs supplied by Headless for one readiness reduction."""

    model_config = ConfigDict(extra="forbid", strict=True)

    applied: dict[str, Any]
    selection: dict[str, Any]


__all__ = [
    "CycleRecoveryActionRequest",
    "ManualResultStageRequest",
    "ProgramCycleResetRequest",
    "ProgramCycleStatusDTO",
    "ProgramNodeAvailabilityRequest",
    "ProgramFullStopResetRequest",
    "ProgramFullStopResumeRequest",
    "ProgramScenePreparationRequest",
    "ProgramSelectionStopRequest",
    "ProgramSelectionStartRequest",
    "ProgramSelectionResumeRequest",
    "ProgramManualHandoverRequest",
    "RuntimeBlackboardWriteRequest",
    "RuntimeOverrideActionRequest",
    "RuntimeOverridePromotionRequest",
    "RuntimeOverrideStageRequest",
]
