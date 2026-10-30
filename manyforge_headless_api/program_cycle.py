"""Closed Program-cycle wire facts shared by their owner and thin consumers.

These models validate data only. They do not decide or advance the lifecycle.
"""

from __future__ import annotations

from typing import Annotated, Any, Literal
import unicodedata

from pydantic import BaseModel, ConfigDict, Field, model_validator

from .control_protocol import AllowedExecutionCommandDTO

PROGRAM_CONTROL_STATE_SCHEMA = "manyforge.program_control_state.v2"
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_MAXIMUM_OPERATION_FAILURES = 4_096


def _failure_text(value: Any, maximum_bytes: int, *, empty: bool = False) -> bool:
    return (
        isinstance(value, str)
        and (empty or bool(value))
        and len(value.encode("utf-8")) <= maximum_bytes
        and unicodedata.normalize("NFC", value) == value
    )


def _failure_counter(value: Any) -> bool:
    return type(value) is int and 1 <= value <= _MAXIMUM_SAFE_INTEGER


def program_operation_failures(run: dict[str, Any] | None) -> list[dict[str, Any]]:
    """The run record's actual operation-failure provenance.

    Each row names one exact failed operation of one node attempt, its
    resources and the Kernel's own facts about it, one row per active hold.
    The Runner always publishes the key (an empty list when there are no
    rows): a run without it is unknown provenance, never "no failure", and
    is refused. Bounds follow the native Program-failure condition contract;
    nothing is inferred here.
    """

    if run is None:
        return []
    if "operation_failures" not in run:
        raise ValueError("Program run omitted its operation failures")
    rows = run["operation_failures"]
    if not isinstance(rows, list) or len(rows) > _MAXIMUM_OPERATION_FAILURES:
        raise ValueError("Program run operation failures are invalid")
    for row in rows:
        operation = row.get("operation") if isinstance(row, dict) else None
        failure = row.get("failure") if isinstance(row, dict) else None
        resources = row.get("resource_ids") if isinstance(row, dict) else None
        facts = row.get("terminal_facts") if isinstance(row, dict) else None
        if (
            not isinstance(row, dict)
            or set(row)
            != {
                "node_instance_id",
                "node_attempt_id",
                "operator_facing_run_revision",
                "operation",
                "resource_ids",
                "failure",
                "recovery_class",
                "terminal_facts",
            }
            or not _failure_text(row["node_instance_id"], 128)
            or not _failure_text(row["node_attempt_id"], 512)
            or not _failure_counter(row["operator_facing_run_revision"])
            or not isinstance(operation, dict)
            or set(operation)
            != {"source_owner_epoch", "operation_id", "operation_revision"}
            or not _failure_text(operation["source_owner_epoch"], 2048)
            or not _failure_text(operation["operation_id"], 2048)
            or not _failure_counter(operation["operation_revision"])
            or not isinstance(resources, list)
            or not resources
            or len(resources) > _MAXIMUM_OPERATION_FAILURES
            or any(not _failure_text(value, 1024) for value in resources)
            or resources != sorted(set(resources))
            or not isinstance(failure, dict)
            or set(failure) != {"code", "message"}
            or not _failure_text(failure["code"], 256)
            or not _failure_text(failure["message"], 4096, empty=True)
            or not _failure_text(row["recovery_class"], 128)
            or not isinstance(facts, list)
            or len(facts) > _MAXIMUM_OPERATION_FAILURES
            or any(
                not isinstance(fact, dict)
                or set(fact) != {"kind", "effect"}
                or not _failure_text(fact["kind"], 128)
                or not _failure_text(fact["effect"], 128)
                for fact in facts
            )
        ):
            raise ValueError("Program run operation failure is invalid")
    return rows


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
                    "ordinary_held_node_count",
                    "state",
                    "stop_allowed",
                    "start_allowed",
                    "start_unavailable_reason",
                    "resume_allowed",
                    "resume_unavailable_reason",
                    "runtime_scale_selector",
                    "runtime_scale_unavailable_reason",
                )
            )
            for group in selections.get("groups", ())
        )

    return "critical" if groups(previous) != groups(cycle) else "node_progress"


Revision = Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
Identity = Annotated[str, Field(min_length=1, max_length=256)]


class _ProgramResetSelectorsDTO(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    reload_initial_scene: bool
    reset_robot_position: bool


class ProgramResetOptionsDTO(_ProgramResetSelectorsDTO):
    # D6 (additive): the accepted Reset is Scenario Close's, which keeps the
    # cell Stop fault; a continuation must carry the same value.
    keep_stop_fault: bool = False


class AcceptedProgramResetDTO(_ProgramResetSelectorsDTO):
    """Private heartbeat proof of the original accepted Reset occurrence."""

    request_owner_epoch: Identity
    request_id: Identity
    request_payload_digest: Annotated[str, Field(pattern=r"^sha256:[0-9a-f]{64}$")]
    reset_claim_generation: Revision | None


class ScenePreparationFailureDTO(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    code: Literal["program_scene_preparation_failed"]
    message: str


class ProgramResetPositionAvailabilityDTO(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    available: bool
    reason: Identity | None

    @model_validator(mode="after")
    def exact_reason(self) -> "ProgramResetPositionAvailabilityDTO":
        if self.available != (self.reason is None):
            raise ValueError("position-reset availability requires its exact reason")
        return self


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
    reset_options: ProgramResetOptionsDTO | None
    reset_robot_position: ProgramResetPositionAvailabilityDTO
    allowed_commands: list[AllowedExecutionCommandDTO]

    @model_validator(mode="after")
    def exact_state_shape(self) -> "ProgramCycleStatusDTO":
        if self.failure is not None and "prior_scene_preparation_failure" in self.failure:
            ScenePreparationFailureDTO.model_validate(self.failure["prior_scene_preparation_failure"])
        if self.state == "idle_after_reset":
            if not (
                self.program_run_id is None
                and self.cycle_claim_generation is None
                and self.terminal_or_settlement_reference is None
            ):
                raise ValueError("Program cycle has the wrong Idle shape")
        elif self.program_run_id is None:
            if self.state not in {"resetting", "recovery_required"}:
                raise ValueError("active Program cycle requires its run identity")
            if (self.state == "resetting" or self.reset_options is not None) and not (
                self.cycle_claim_generation is None
                and self.terminal_or_settlement_reference is None
                and self.reset_phase in {
                    "restoring_scene", "restoring_position", "releasing"
                }
                and self.current_command_kind == "program_reset"
                and self.reset_options is not None
                and (self.reset_options.reload_initial_scene or self.reset_options.reset_robot_position)
            ):
                raise ValueError("no-run Reset has the wrong state shape")
            if self.reset_options is not None and self.state == "recovery_required" and (
                self.failure is None or self.failure.get("failed_phase") != self.reset_phase
            ):
                raise ValueError("no-run Reset recovery requires its exact failed phase")
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
        if self.state == "resetting" and self.reset_options is None:
            raise ValueError("Resetting Program cycle requires accepted options")
        if self.reset_options is not None and self.state not in {"resetting", "recovery_required"}:
            raise ValueError("accepted Reset options have no active Reset occurrence")
        if self.reset_options is not None and (
            (self.reset_phase == "restoring_scene" and not self.reset_options.reload_initial_scene)
            or (self.reset_phase == "restoring_position" and not self.reset_options.reset_robot_position)
        ):
            raise ValueError("Reset phase requires its selected effect")
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
