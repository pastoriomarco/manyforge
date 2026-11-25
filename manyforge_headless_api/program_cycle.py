"""Closed Program-cycle wire facts shared by their owner and thin consumers.

These models validate data only. They do not decide or advance the lifecycle.
"""

from __future__ import annotations

from typing import Annotated, Any, Literal
import unicodedata

from pydantic import BaseModel, ConfigDict, Field, model_validator

from .control_protocol import AllowedExecutionCommandDTO

PROGRAM_CONTROL_STATE_SCHEMA = "manyforge.program_control_state.v4"
# The reply of every runner command route answered at acceptance (OD-214).
PROGRAM_COMMAND_REPLY_SCHEMA = "manyforge.program_command_reply.v1"
# Ended command rows the runner keeps (OD-214): one command at a time, so a
# few cover a client's reconnect window.
MAXIMUM_ENDED_COMMAND_ROWS = 8
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
            or not _failure_counter(row["node_attempt_id"])
            or not _failure_counter(row["operator_facing_run_revision"])
            or not isinstance(operation, dict)
            or set(operation) != {"operation_id", "operation_revision"}
            or not _failure_counter(operation["operation_id"])
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
                    "stopped_by_node_instance_id",
                )
            )
            for group in selections.get("groups", ())
        )

    def questions(value: dict[str, Any]) -> tuple[Any, ...]:
        # An operator question asked or answered is operator-facing.
        controls = value.get("runtime_controls") or {}
        return tuple(
            (question.get("offer_id"), question.get("offer_revision"))
            for question in controls.get("operator_questions") or ()
            if isinstance(question, dict)
        )

    return (
        "critical"
        if groups(previous) != groups(cycle) or questions(previous) != questions(cycle)
        else "node_progress"
    )


Revision = Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
Identity = Annotated[str, Field(min_length=1, max_length=256)]
# The Program runner's command kinds (its request kinds, plus the scene
# preparation, which records no request).
RunnerCommandKind = Literal[
    "start",
    "reset",
    "scene_preparation",
    "runtime_blackboard_write",
    "manual_result_stage",
    "recovery_action",
    "resume_halt",
    "start_motion",
    "resume_motion",
    "prepare_manual_handover",
    "persistent_value_set",
    "persistent_value_reset",
    "persistent_values_clear",
]
# The stages a runner command publishes (OD-214); each kind uses a closed
# subset (contract od214 §1.3). The Reset tokens are the cycle's `reset_phase`.
RunnerCommandStep = Literal[
    "admitting",
    "claiming",
    "starting_run",
    "applying",
    "recovering",
    "resuming",
    "stopping",
    "settling",
    "restoring_scene",
    "restoring_position",
    "releasing",
    "preparing_scene",
]
CommandOutcome = Literal["succeeded", "refused", "failed", "unknown", "interrupted"]
# Wall-clock ns since the Unix epoch, as canonical decimal text (beyond a
# JSON-safe integer), at most the int64 maximum.
_EPOCH_NS = Annotated[str, Field(pattern=r"^(0|[1-9][0-9]{0,18})$")]


class CommandEndDTO(BaseModel):
    """How one command ended: its outcome and, unless it succeeded, its typed
    code with the error's own details (no text: the catalogue gives it)."""

    model_config = ConfigDict(extra="forbid", strict=True)

    outcome: CommandOutcome
    code: Identity | None
    details: dict[str, Any] | None
    finished_at_ns: _EPOCH_NS

    @model_validator(mode="after")
    def code_unless_succeeded(self) -> "CommandEndDTO":
        if (self.outcome == "succeeded") != (self.code is None):
            raise ValueError("a command end has a code exactly when it did not succeed")
        if self.outcome == "succeeded" and self.details is not None:
            raise ValueError("a succeeded command end carries no details")
        return self


class RunnerCommandRowDTO(BaseModel):
    """One runner command from taking the runner to its end (OD-214)."""

    model_config = ConfigDict(extra="forbid", strict=True)

    kind: RunnerCommandKind
    request_key: Annotated[str, Field(pattern=r"^[0-9a-f]{32}$")]
    step: RunnerCommandStep | None
    accepted_at_ns: _EPOCH_NS | None
    end: CommandEndDTO | None

    @model_validator(mode="after")
    def accepted_before_steps(self) -> "RunnerCommandRowDTO":
        if self.accepted_at_ns is None and self.step is not None:
            raise ValueError("a command not yet accepted has no step")
        if (self.accepted_at_ns is None and self.end is not None
                and self.end.outcome not in {"refused", "unknown"}):
            raise ValueError("a command ended before acceptance was refused or unknown")
        return self


class RunnerCommandsDTO(BaseModel):
    """The command holding the runner and the last ended ones, newest first."""

    model_config = ConfigDict(extra="forbid", strict=True)

    in_progress: RunnerCommandRowDTO | None
    ended: Annotated[
        list[RunnerCommandRowDTO], Field(max_length=MAXIMUM_ENDED_COMMAND_ROWS)
    ]

    @model_validator(mode="after")
    def ends_where_they_belong(self) -> "RunnerCommandsDTO":
        if self.in_progress is not None and self.in_progress.end is not None:
            raise ValueError("the command in progress has not ended")
        if any(row.end is None for row in self.ended):
            raise ValueError("an ended command row requires its end")
        return self


class ProgramCommandReplyDTO(BaseModel):
    """A runner command's reply: its row at acceptance (or a replay)."""

    model_config = ConfigDict(extra="forbid", strict=True)

    schema_version: Literal["manyforge.program_command_reply.v1"]
    command: RunnerCommandRowDTO


class _ProgramResetSelectorsDTO(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    reload_initial_scene: bool
    reset_robot_position: bool


class ProgramResetOptionsDTO(_ProgramResetSelectorsDTO):
    """The accepted Reset's options; a continuation must carry the same."""

    reset_stored_values: bool


class AcceptedProgramResetDTO(_ProgramResetSelectorsDTO):
    """Private heartbeat proof of the original accepted Reset occurrence."""

    request_key: Annotated[str, Field(pattern=r"^[0-9a-f]{32}$")]
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

    cycle_generation: Revision
    state: Literal[
        "idle_after_reset",
        "starting",
        "running",
        "resetting",
        "terminal_awaiting_reset",
        "recovery_required",
    ]
    program_run_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None
    cycle_claim_generation: Revision | None
    terminal_or_settlement_reference: (
        Annotated[str, Field(pattern=r"^sha256:[0-9a-f]{64}$")] | None
    )
    execution_control_revision: Revision
    # The key of the request that caused the current command (1.15.3).
    causal_request_key: Annotated[str, Field(pattern=r"^[0-9a-f]{32}$")] | None
    current_command_kind: Identity | None
    # OD-144/OD-214: the runner command in progress and the last ended ones.
    runner_commands: RunnerCommandsDTO
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
            # A run number is announced only after its Kernel claim (K-6), so
            # a start in its window has neither run nor claim yet (1.15.5).
            if self.state == "starting":
                if self.cycle_claim_generation is not None:
                    raise ValueError("starting Program cycle has no claim before its run")
            elif self.state not in {"resetting", "recovery_required"}:
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
    # ProgramRunEnd of the frame's run (None while it runs).
    run_end: dict[str, Any] | None = None

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
                or self.runtime_controls.get("run_revision") != self.run_revision
                or state.get("runtime_control_revision")
                != self.runtime_control_revision
            ):
                raise ValueError("Runtime controls do not match their cycle")
        return self
