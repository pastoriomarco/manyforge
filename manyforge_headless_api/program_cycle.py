"""Closed Program-cycle wire facts shared by their owner and thin consumers.

These models validate data only. They do not decide or advance the lifecycle.
"""

from __future__ import annotations

from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from .control_protocol import AllowedExecutionCommandDTO

PROGRAM_CONTROL_STATE_SCHEMA = "manyforge.program_control_state.v2"


def program_cycle_control_key(cycle: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(
        cycle.get(field)
        for field in (
            "program_runner_owner_epoch",
            "cycle_generation",
            "execution_control_revision",
        )
    )


def program_cycle_event_class(
    previous: dict[str, Any] | None, cycle: dict[str, Any]
) -> str:
    """Producer-side presentation priority; never grants command authority."""
    if previous is None or program_cycle_control_key(
        previous
    ) != program_cycle_control_key(cycle):
        return "critical"

    def groups(value: dict[str, Any]) -> tuple[Any, ...]:
        controls = value.get("runtime_controls") or {}
        selections = controls.get("execution_selections") or {}
        return tuple(
            tuple(
                group.get(field)
                for field in (
                    "group_id",
                    "state",
                    "stop_allowed",
                    "resume_allowed",
                    "resume_unavailable_reason",
                )
            )
            for group in selections.get("groups", ())
        )

    return "critical" if groups(previous) != groups(cycle) else "node_progress"


Revision = Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
Identity = Annotated[str, Field(min_length=1, max_length=256)]


class ProgramCycleStatusDTO(BaseModel):
    """Core facts published by the Program cycle's sole lifecycle owner."""

    model_config = ConfigDict(extra="forbid", strict=True)

    program_runner_owner_epoch: Identity
    cycle_generation: Revision
    state: Literal[
        "idle_after_reset",
        "starting",
        "running",
        "stopping",
        "resetting",
        "terminal_awaiting_reset",
        "recovery_required",
    ]
    program_run_id: Identity | None
    cycle_claim_generation: Revision | None
    terminal_or_settlement_reference: (
        Annotated[str, Field(pattern=r"^sha256:[0-9a-f]{64}$")] | None
    )
    execution_control_revision: Revision
    causal_correlation_id: Identity | None
    causal_session_id: Identity | None
    causal_client_sequence: Annotated[str, Field(pattern=r"^[1-9][0-9]{0,19}$")] | None
    current_command_kind: Identity | None
    failure: dict[str, Any] | None
    reset_phase: Identity | None
    allowed_commands: list[AllowedExecutionCommandDTO]

    @model_validator(mode="after")
    def exact_state_shape(self) -> "ProgramCycleStatusDTO":
        if self.state == "idle_after_reset":
            if not (
                self.program_run_id is None
                and self.cycle_claim_generation is None
                and self.terminal_or_settlement_reference is None
            ):
                raise ValueError("Program cycle has the wrong Idle shape")
        elif self.program_run_id is None and self.state != "recovery_required":
            raise ValueError("active Program cycle requires its run identity")
        elif self.cycle_claim_generation is None and self.state == "running":
            raise ValueError("acquired Program cycle requires its claim generation")
        if (
            self.state == "terminal_awaiting_reset"
            and self.terminal_or_settlement_reference is None
        ):
            raise ValueError("terminal Program cycle requires its reference")
        if (self.causal_session_id is None) != (self.causal_client_sequence is None):
            raise ValueError("causal session and client sequence must be paired")
        if (
            self.causal_client_sequence is not None
            and int(self.causal_client_sequence) > 2**64 - 1
        ):
            raise ValueError("causal client sequence exceeds uint64")
        if self.state == "resetting" and self.reset_phase is None:
            raise ValueError("Resetting Program cycle requires its phase")
        if self.state == "recovery_required" and self.failure is None:
            raise ValueError("recovery-required Program cycle requires failure")
        return self


class ProgramCycleFrameDTO(ProgramCycleStatusDTO):
    """Complete current frame; omission is not a nullable projection."""

    run_revision: Revision | None
    node_status_revision: Revision | None
    runtime_control_revision: Revision | None
    recovery_offer: dict[str, Any] | None
    run: dict[str, Any] | None
    runtime_controls: dict[str, Any] | None

    @model_validator(mode="after")
    def exact_projection_pairing(self) -> "ProgramCycleFrameDTO":
        if self.run is None:
            if self.run_revision is not None or self.node_status_revision is not None:
                raise ValueError("Program run revision has no projection")
        else:
            capture = self.run.get("capture")
            if (
                not isinstance(capture, dict)
                or capture.get("program_run_id") != self.program_run_id
                or self.run.get("run_revision") != self.run_revision
                or self.run.get("node_status_revision") != self.node_status_revision
                or (
                    self.state not in {"terminal_awaiting_reset", "resetting"}
                    and self.run.get("owner_epoch") != self.program_runner_owner_epoch
                )
            ):
                raise ValueError("Program run does not match its cycle")
        if self.runtime_controls is None:
            if self.runtime_control_revision is not None:
                raise ValueError("Runtime-control revision has no projection")
        else:
            state = self.runtime_controls.get("runtime_controls")
            if (
                self.run is None
                or not isinstance(state, dict)
                or self.runtime_controls.get("program_run_id") != self.program_run_id
                or self.runtime_controls.get("owner_epoch")
                != self.program_runner_owner_epoch
                or self.runtime_controls.get("run_revision") != self.run_revision
                or state.get("runtime_control_revision")
                != self.runtime_control_revision
            ):
                raise ValueError("Runtime controls do not match their cycle")
        return self
